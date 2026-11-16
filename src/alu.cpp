// SPDX-License-Identifier: GPL-3.0-only
#include "image.hpp"
#include "lower.hpp"
#include "runtime.hpp"

#include <bit>
#include <cstdint>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>
#include <utility>

namespace armv5::detail {
llvm::Value *Lowering::flag(llvm::Value *status, std::uint32_t mask) {
  return ir_.CreateICmpNE(ir_.CreateAnd(status, c32(mask)), c32(0));
}

llvm::Value *Lowering::condition(unsigned code, llvm::Value *status) {
  auto *negative = flag(status, status_n);
  auto *zero = flag(status, status_z);
  auto *carry = flag(status, status_c);
  auto *overflow = flag(status, status_v);
  switch (code) {
    case 0: return zero;
    case 1: return ir_.CreateNot(zero);
    case 2: return carry;
    case 3: return ir_.CreateNot(carry);
    case 4: return negative;
    case 5: return ir_.CreateNot(negative);
    case 6: return overflow;
    case 7: return ir_.CreateNot(overflow);
    case 8: return ir_.CreateAnd(carry, ir_.CreateNot(zero));
    case 9: return ir_.CreateOr(ir_.CreateNot(carry), zero);
    case 10: return ir_.CreateICmpEQ(negative, overflow);
    case 11: return ir_.CreateICmpNE(negative, overflow);
    case 12: return ir_.CreateAnd(ir_.CreateNot(zero), ir_.CreateICmpEQ(negative, overflow));
    case 13: return ir_.CreateOr(zero, ir_.CreateICmpNE(negative, overflow));
    default: return ir_.getTrue();
  }
}

llvm::Value *Lowering::reg(const Instruction &instruction, const Values &values, unsigned index) {
  return index == 15 ? c32(instruction.pc + 8) : values.r[index];
}

Shifted Lowering::shift(
    llvm::Value *value, unsigned type, llvm::Value *amount, bool immediate, llvm::Value *carry) {
  constexpr unsigned top_bit = 31;
  const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(amount);
  if (immediate && constant != nullptr && constant->isZero()) {
    if (type == 3) {
      return {.value = ir_.CreateOr(
                  ir_.CreateLShr(value, c32(1)),
                  ir_.CreateShl(ir_.CreateZExt(carry, ir_.getInt32Ty()), c32(top_bit))),
              .carry = ir_.CreateICmpNE(ir_.CreateAnd(value, c32(1)), c32(0))};
    }
    if (type == 1 || type == 2) { amount = c32(32); }
  }
  auto *safe = ir_.CreateAnd(amount, c32(top_bit));
  auto *zero = ir_.CreateICmpEQ(amount, c32(0));
  auto *within = ir_.CreateICmpULT(amount, c32(32));
  llvm::Value *result = nullptr;
  llvm::Value *out = nullptr;
  switch (type) {
    case 0:
      result = ir_.CreateSelect(within, ir_.CreateShl(value, safe), c32(0));
      out = ir_.CreateAnd(
          ir_.CreateLShr(value, ir_.CreateAnd(ir_.CreateSub(c32(32), amount), c32(top_bit))),
          c32(1));
      out = ir_.CreateSelect(ir_.CreateICmpULE(amount, c32(32)), out, c32(0));
      break;
    case 1:
      result = ir_.CreateSelect(within, ir_.CreateLShr(value, safe), c32(0));
      out = ir_.CreateAnd(
          ir_.CreateLShr(value, ir_.CreateAnd(ir_.CreateSub(amount, c32(1)), c32(top_bit))),
          c32(1));
      out = ir_.CreateSelect(ir_.CreateICmpULE(amount, c32(32)), out, c32(0));
      break;
    case 2:
      result = ir_.CreateAShr(value, ir_.CreateSelect(within, safe, c32(top_bit)));
      out =
          ir_.CreateAnd(ir_.CreateLShr(value,
                                       ir_.CreateSelect(within,
                                                        ir_.CreateAnd(ir_.CreateSub(amount, c32(1)),
                                                                      c32(top_bit)),
                                                        c32(top_bit))),
                        c32(1));
      break;
    default:
      result = ir_.CreateIntrinsic(llvm::Intrinsic::fshr, {ir_.getInt32Ty()}, {value, value, safe});
      out = ir_.CreateLShr(result, c32(top_bit));
      break;
  }
  return {.value = ir_.CreateSelect(zero, value, result),
          .carry = ir_.CreateSelect(zero, carry, ir_.CreateICmpNE(out, c32(0)))};
}

Shifted Lowering::operand(const Instruction &instruction, const Values &values) {
  auto *carry = flag(values.status, status_c);
  const auto word = instruction.word;
  if ((word & 0x02000000) != 0) {
    const unsigned rotate = ((word >> 8) & 15) * 2;
    const auto value = std::rotr(word & immediate_mask, int(rotate));
    return {.value = c32(value),
            .carry = rotate == 0 ? carry : ir_.getInt1((value & status_n) != 0)};
  }
  const bool immediate = (word & 0x10) == 0;
  auto *amount = immediate ? c32((word >> 7) & shift_count_mask)
                           : ir_.CreateAnd(values.r[(word >> 8) & 15], c32(immediate_mask));
  return shift(reg(instruction, values, word & 15), (word >> 5) & 3, amount, immediate, carry);
}

void Lowering::alu(const Instruction &instruction, Values &values) {
  const auto opcode = (instruction.word >> opcode_shift) & 15;
  const auto shifted = operand(instruction, values);
  auto *first = reg(instruction, values, (instruction.word >> 16) & 15);
  auto *second = shifted.value;
  llvm::Value *result = nullptr;
  llvm::Value *carry = shifted.carry;
  llvm::Value *overflow = nullptr;
  const bool arithmetic = (opcode >= 2 && opcode <= 7) || opcode == 10 || opcode == 11;
  if (arithmetic) {
    const bool subtract = opcode != 4 && opcode != 5 && opcode != 11;
    const bool reverse = opcode == 3 || opcode == 7;
    if (reverse) { std::swap(first, second); }
    if (subtract) { second = ir_.CreateNot(second); }
    auto *input = opcode == 5 || opcode == 6 || opcode == 7 ? flag(values.status, status_c)
                                                            : ir_.getInt1(subtract);
    auto *wide = ir_.CreateAdd(ir_.CreateAdd(ir_.CreateZExt(first, ir_.getInt64Ty()),
                                             ir_.CreateZExt(second, ir_.getInt64Ty())),
                               ir_.CreateZExt(input, ir_.getInt64Ty()));
    result = ir_.CreateTrunc(wide, ir_.getInt32Ty());
    carry = ir_.CreateICmpNE(ir_.CreateLShr(wide, c64(32)), c64(0));
    overflow = flag(
        ir_.CreateAnd(ir_.CreateNot(ir_.CreateXor(first, second)), ir_.CreateXor(first, result)),
        status_n);
  } else {
    switch (opcode) {
      case 0:
      case 8: result = ir_.CreateAnd(first, second); break;
      case 1:
      case 9: result = ir_.CreateXor(first, second); break;
      case 12: result = ir_.CreateOr(first, second); break;
      case 13: result = second; break;
      case 14: result = ir_.CreateAnd(first, ir_.CreateNot(second)); break;
      default: result = ir_.CreateNot(second); break;
    }
  }
  const bool test = opcode >= 8 && opcode <= 11;
  if (!test) { values.r[(instruction.word >> 12) & 15] = result; }
  if ((instruction.word & 0x00100000) == 0 || exception_return(instruction)) { return; }
  auto *flags =
      ir_.CreateOr(ir_.CreateAnd(result, c32(status_n)),
                   ir_.CreateSelect(ir_.CreateICmpEQ(result, c32(0)), c32(status_z), c32(0)));
  flags = ir_.CreateOr(flags, ir_.CreateSelect(carry, c32(status_c), c32(0)));
  if (arithmetic) {
    flags = ir_.CreateOr(flags, ir_.CreateSelect(overflow, c32(status_v), c32(0)));
  }
  values.status = ir_.CreateOr(
      ir_.CreateAnd(values.status, c32(~(arithmetic ? nzcv_mask : nz_mask | status_c))), flags);
}

bool Lowering::exception_return(const Instruction &instruction) const {
  const auto word = instruction.word;
  const bool returning = (instruction.operation == Operation::alu && instruction.terminates &&
                          (word & 0x00100000) != 0) ||
                         (instruction.operation == Operation::multiple && instruction.terminates &&
                          (word & 0x00400000) != 0);
  if (returning && (mode_ == user_mode || mode_ == system_mode)) {
    refuse(image_.identity, instruction.pc, "unpredictable exception return without SPSR");
  }
  return returning;
}

llvm::Value *Lowering::user_reg(const Values &values, unsigned index) const {
  return index < 8 || (index < 13 && mode_ != fiq_mode) || mode_ == user_mode ||
                 mode_ == system_mode
             ? values.r[index]
             : values.user[index - 8];
}

void Lowering::set_user_reg(Values &values, unsigned index, llvm::Value *value) const {
  if (index < 8 || (index < 13 && mode_ != fiq_mode) || mode_ == user_mode ||
      mode_ == system_mode) {
    values.r[index] = value;
  } else {
    values.user[index - 8] = value;
  }
}

void Lowering::multiply(const Instruction &instruction, Values &values) {
  const auto word = instruction.word;
  auto *result = ir_.CreateMul(values.r[word & 15], values.r[(word >> 8) & 15]);
  if ((word & 0x00200000) != 0) { result = ir_.CreateAdd(result, values.r[(word >> 12) & 15]); }
  values.r[(word >> 16) & 15] = result;
  if ((word & 0x00100000) != 0) {
    values.status = ir_.CreateOr(
        ir_.CreateAnd(values.status, c32(~nz_mask)),
        ir_.CreateOr(ir_.CreateAnd(result, c32(status_n)),
                     ir_.CreateSelect(ir_.CreateICmpEQ(result, c32(0)), c32(status_z), c32(0))));
  }
}

void Lowering::status_transfer(const Instruction &instruction, Values &values) {
  const auto word = instruction.word;
  const bool saved = (word & 0x00400000) != 0;
  if (saved && (mode_ == user_mode || mode_ == system_mode)) {
    refuse(image_.identity, instruction.pc, "unpredictable SPSR access without SPSR");
  }
  if (instruction.operation == Operation::mrs) {
    const auto destination = (word >> 12) & 15;
    if (destination == 15) { refuse(image_.identity, instruction.pc, "unpredictable MRS PC"); }
    values.r[destination] = saved ? values.spsr : values.status;
    return;
  }
  if ((word & 0x02000000) == 0 && (word & 15) == 15) {
    refuse(image_.identity, instruction.pc, "unpredictable MSR PC operand");
  }
  constexpr std::uint32_t unallocated = 0x06ffff00;
  constexpr std::uint32_t state = 0x01000020;
  constexpr std::uint32_t privileged = 0xdf;
  std::uint32_t bytes = 0;
  for (unsigned index = 0; index < 4; ++index) {
    if ((word & (1U << (16 + index))) != 0) { bytes |= immediate_mask << (index * 8); }
  }
  auto writable = flags_mask;
  if (saved) {
    writable |= privileged | state;
  } else if (mode_ != user_mode) {
    writable |= privileged;
  }
  const auto mask = bytes & writable;
  auto *value = operand(instruction, values).value;
  const auto forbidden = unallocated | (!saved && mode_ != user_mode ? state : 0);
  guard(ir_.CreateICmpEQ(ir_.CreateAnd(value, c32(forbidden & bytes)), c32(0)),
        values,
        Signal::unsupported,
        c32(instruction.pc));
  llvm::Value *&status = saved ? values.spsr : values.status;
  status = ir_.CreateOr(ir_.CreateAnd(status, c32(~mask)), ir_.CreateAnd(value, c32(mask)));
}
} // namespace armv5::detail
