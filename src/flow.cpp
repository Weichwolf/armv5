// SPDX-License-Identifier: GPL-3.0-only
#include "lower.hpp"
#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <llvm/IR/Value.h>
#include <vector>

namespace armv5::detail {
namespace {
constexpr std::uint32_t bx_mask = 0x2f, move_mask = 0x03f0ffff, move_pc_lr = 0x01a0f00e,
                        load_mask = 0x001ff000, load_pc_sp = 0x001df000, multiple_mask = 0x005f8000,
                        multiple_pc_sp = 0x001d8000;
}

bool Lowering::return_instruction(const Instruction &instruction) {
  const auto word = instruction.word;
  switch (instruction.operation) {
    case Operation::exchange: return (word & bx_mask) == 14;
    case Operation::alu:
      return (word & move_mask) == move_pc_lr; // MOV PC,LR without S or shifting.
    case Operation::single: return (word & load_mask) == load_pc_sp; // LDR PC based on SP.
    case Operation::multiple:
      return (word & multiple_mask) == multiple_pc_sp; // LDM SP,...PC without exception return.
    default: return false;
  }
}

void Lowering::analyze_calls() {
  // Specialize forward leaf calls. Their increasing-PC body is already covered
  // by the caller's acyclic admission proof. Other control flow keeps SSA edges.
  for (const auto &instruction : code_) {
    if (instruction.operation != Operation::branch || !instruction.link || instruction.exchange ||
        instruction.target <= instruction.pc || !indices_.contains(instruction.next())) {
      continue;
    }
    std::vector<std::size_t> body;
    for (auto pc = instruction.target;;) {
      const auto found = indices_.find(pc);
      if (found == indices_.end()) { break; }
      const auto &next = code_[found->second];
      body.push_back(found->second);
      if (next.terminates) {
        if ((next.word >> condition_shift) == 14 && return_instruction(next)) {
          inline_calls_.emplace(instruction.pc, body);
        }
        break;
      }
      if (next.next() <= pc) { break; }
      pc = next.next();
    }
  }
}

bool Lowering::inline_call(const Instruction &instruction, Values values) {
  const auto found = inline_calls_.find(instruction.pc);
  if (!admitted_path_ || found == inline_calls_.end()) { return false; }
  inline_return_ = instruction.next() | (thumb_ ? 1U : 0U);
  for (const auto index : found->second) { emit_instruction(code_[index], values); }
  inline_return_.reset();
  return true;
}

void Lowering::return_transfer(Values values, bool exchange) {
  if (!inline_return_) {
    transfer(values, values.r[15], exchange);
    return;
  }
  auto *expected = block("expected.return");
  auto *unexpected = block("unexpected.return");
  cold_branch(ir_.CreateICmpEQ(values.r[15], c32(*inline_return_)), expected, unexpected);
  ir_.SetInsertPoint(expected);
  edge(values, *inline_return_ & (thumb_ ? ~std::uint32_t{1} : arm_pc_mask));
  ir_.SetInsertPoint(unexpected);
  transfer(values, values.r[15], exchange, TransferExit::owner);
}

void Lowering::cold_transfer(Values values, llvm::Value *target) {
  values.r[15] = target;
  auto *missing = block("unexpected.return.missing");
  auto *prepared = block("unexpected.return.prepared");
  auto *invalid = block("unexpected.return.invalid");
  auto *dispatch = ir_.CreateSwitch(target, missing, unsigned(code_.size()));
  for (const auto &instruction : code_) {
    dispatch->addCase(ir_.getInt32(instruction.pc),
                      instruction.continuation_only ? invalid : prepared);
  }
  for (const auto &transition : image_.transitions) {
    if (transition.source_variant != variant_.id || indices_.contains(transition.address)) {
      continue;
    }
    auto *next = block("unexpected.return.variant");
    dispatch->addCase(ir_.getInt32(transition.address), next);
    ir_.SetInsertPoint(next);
    exit(values, Signal::transfer, target);
  }
  ir_.SetInsertPoint(prepared);
  exit(values, Signal::redispatch, target);
  ir_.SetInsertPoint(invalid);
  exit(values, Signal::unsupported, target);
  ir_.SetInsertPoint(missing);
  unknown_target(values, target);
}
} // namespace armv5::detail
