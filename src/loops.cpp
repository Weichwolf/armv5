// SPDX-License-Identifier: GPL-3.0-only
#include "lower.hpp"

#include <cstdint>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <optional>

namespace armv5::detail {
namespace {
constexpr std::uint32_t immediate_bit = 0x02000000, flags_bit = 0x00100000,
                        decrement_mask = 0xfff00fff, decrement_one = 0xe2500001;

bool operand_independent(std::uint32_t word, unsigned counter) {
  if ((word & immediate_bit) != 0) { return true; }
  if ((word & 15) == counter) { return false; }
  if ((word & 0x10) != 0) { return ((word >> 8) & 15) != counter; }
  // RRX observes carry from the previous SUBS, unlike the other non-S shifts.
  constexpr std::uint32_t shift_mask = 0xff0;
  constexpr std::uint32_t rrx = 0x60;
  return (word & shift_mask) != rrx;
}

bool independent(const Instruction &instruction, unsigned counter) {
  const auto word = instruction.word;
  if (instruction.terminates || (word >> condition_shift) != 14) { return false; }
  switch (instruction.operation) {
    case Operation::alu: {
      const auto opcode = (word >> opcode_shift) & 15;
      if ((word & flags_bit) != 0 || (opcode >= 5 && opcode <= 11)) { return false; }
      const bool moves = opcode == 13 || opcode == 15;
      return ((word >> 12) & 15) != counter && (moves || ((word >> 16) & 15) != counter) &&
             operand_independent(word, counter);
    }
    case Operation::multiply:
      return (word & flags_bit) == 0 && ((word >> 16) & 15) != counter &&
             ((word >> 8) & 15) != counter && (word & 15) != counter &&
             ((word & 0x00200000) == 0 || ((word >> 12) & 15) != counter);
    case Operation::single:
      // A single-transfer register offset has the opposite immediate encoding.
      return ((word >> 16) & 15) != counter && ((word >> 12) & 15) != counter &&
             operand_independent(word ^ immediate_bit, counter);
    default: return false;
  }
}
} // namespace

std::optional<unsigned> Lowering::loop_counter(const Block &body) const {
  if (body.end - body.first < 2) { return std::nullopt; }
  const auto &last = code_[body.end - 1];
  const auto word = code_[body.end - 2].word;
  const auto counter = (word >> 12) & 15;
  if (code_[body.end - 2].operation != Operation::alu || last.operation != Operation::branch ||
      last.link || last.exchange || (last.word >> condition_shift) != 1 ||
      last.target != code_[body.first].pc || (word & decrement_mask) != decrement_one ||
      ((word >> 16) & 15) != counter || counter == 15) {
    return std::nullopt;
  }
  for (auto index = body.first; index < body.end - 2; ++index) {
    if (!independent(code_[index], counter)) { return std::nullopt; }
  }
  return counter;
}

llvm::Value *Lowering::decrement_flags(llvm::Value *status, llvm::Value *result) {
  constexpr std::uint32_t all_ones = 0xffffffff;
  constexpr std::uint32_t signed_max = 0x7fffffff;
  auto *flags = ir_.CreateAnd(result, c32(status_n));
  flags = ir_.CreateOr(flags,
                       ir_.CreateSelect(ir_.CreateICmpEQ(result, c32(0)), c32(status_z), c32(0)));
  flags = ir_.CreateOr(
      flags, ir_.CreateSelect(ir_.CreateICmpNE(result, c32(all_ones)), c32(status_c), c32(0)));
  flags = ir_.CreateOr(
      flags, ir_.CreateSelect(ir_.CreateICmpEQ(result, c32(signed_max)), c32(status_v), c32(0)));
  return ir_.CreateOr(ir_.CreateAnd(status, c32(~nzcv_mask)), flags);
}

Values Lowering::observable_values(Values values) {
  if (!loop_prefix_) { return values; }
  const auto &prefix = *loop_prefix_;
  values.instructions =
      ir_.CreateAdd(values.instructions, ir_.CreateMul(prefix.completed, c64(prefix.instructions)));
  values.cycles = ir_.CreateAdd(values.cycles, ir_.CreateMul(prefix.completed, c64(prefix.cycles)));
  auto *counter =
      ir_.CreateSub(values.r[prefix.counter], ir_.CreateTrunc(prefix.completed, ir_.getInt32Ty()));
  values.r[prefix.counter] = counter;
  values.status = ir_.CreateSelect(ir_.CreateICmpEQ(prefix.completed, c64(0)),
                                   values.status,
                                   decrement_flags(values.status, counter));
  return values;
}

void Lowering::loop_grant(const Block &body) {
  const auto counter = loop_counter(body);
  if (!counter) { return; }
  const auto initial = body.head.values;
  const auto instructions = body.end - body.first;
  auto *turns =
      ir_.CreateUDiv(ir_.CreateSub(instruction_bound_, initial.instructions), c64(instructions));
  auto *cycle_turns = ir_.CreateUDiv(ir_.CreateSub(cycle_bound_, initial.cycles), c64(body.cycles));
  auto *count = ir_.CreateZExt(initial.r[*counter], ir_.getInt64Ty());
  count = ir_.CreateSelect(ir_.CreateICmpEQ(count, c64(0)), c64(std::uint64_t{1} << 32), count);
  turns = ir_.CreateIntrinsic(llvm::Intrinsic::umin, {ir_.getInt64Ty()}, {turns, cycle_turns});
  turns = ir_.CreateIntrinsic(llvm::Intrinsic::umin, {ir_.getInt64Ty()}, {turns, count});
  auto *fits =
      ir_.CreateAnd(ir_.CreateICmpNE(turns, c64(0)),
                    ir_.CreateAnd(ir_.CreateICmpULT(initial.instructions, instruction_bound_),
                                  ir_.CreateICmpULT(initial.cycles, cycle_bound_)));
  auto *fallback = block("grant.exact");
  auto *done = block("grant.done");
  auto loop = join("grant.loop");
  llvm::IRBuilder<> builder(loop.block);
  auto *remaining = builder.CreatePHI(ir_.getInt64Ty(), 2);
  remaining->addIncoming(turns, ir_.GetInsertBlock());
  incoming(loop, initial, ir_.GetInsertBlock());
  cold_branch(fits, loop.block, fallback);
  ir_.SetInsertPoint(loop.block);
  auto values = loop.values;
  loop_prefix_ = {.counter = *counter,
                  .instructions = instructions,
                  .cycles = body.cycles,
                  .completed = ir_.CreateSub(turns, remaining)};
  admitted_path_ = true;
  for (auto index = body.first; index < body.end - 2; ++index) {
    emit_instruction(code_[index], values);
  }
  loop_prefix_.reset();
  auto carried = values;
  carried.instructions = initial.instructions;
  carried.cycles = initial.cycles;
  auto *next = ir_.CreateSub(remaining, c64(1));
  remaining->addIncoming(next, ir_.GetInsertBlock());
  incoming(loop, carried, ir_.GetInsertBlock());
  ir_.CreateCondBr(ir_.CreateICmpNE(next, c64(0)), loop.block, done);
  ir_.SetInsertPoint(done);
  values.r[*counter] = ir_.CreateSub(initial.r[*counter], ir_.CreateTrunc(turns, ir_.getInt32Ty()));
  values.status = decrement_flags(initial.status, values.r[*counter]);
  values.instructions =
      ir_.CreateAdd(initial.instructions, ir_.CreateMul(turns, c64(instructions)));
  values.cycles = ir_.CreateAdd(initial.cycles, ir_.CreateMul(turns, c64(body.cycles)));
  auto *complete = block("grant.complete");
  auto *partial = block("grant.partial");
  ir_.CreateCondBr(ir_.CreateICmpEQ(values.r[*counter], c32(0)), complete, partial);
  admitted_path_ = false;
  ir_.SetInsertPoint(partial);
  edge(values, code_[body.first].pc);
  ir_.SetInsertPoint(complete);
  values.cycles = ir_.CreateSub(values.cycles, c64(2)); // Final BNE is not taken.
  edge(values, code_[body.end - 1].next());
  ir_.SetInsertPoint(fallback);
}
} // namespace armv5::detail
