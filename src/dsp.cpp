// SPDX-License-Identifier: GPL-3.0-only
#include "lower.hpp"

#include <cstdint>
#include <llvm/IR/Value.h>

namespace armv5::detail {
namespace {
constexpr unsigned high_word_shift = 32, halfword_shift = 16;
constexpr std::uint32_t status_q = 0x08000000, signed_minimum = 0x80000000,
                        signed_maximum = 0x7fffffff;
}

void Lowering::long_multiply(const Instruction &instruction, Values &values) {
  const auto word = instruction.word;
  const auto high = (word >> 16) & 15;
  const auto low = (word >> 12) & 15;
  const bool signed_product = (word & 0x00400000) != 0;
  auto extend = [&](llvm::Value *value) {
    return signed_product ? ir_.CreateSExt(value, ir_.getInt64Ty())
                          : ir_.CreateZExt(value, ir_.getInt64Ty());
  };
  auto *product = ir_.CreateMul(extend(values.r[word & 15]), extend(values.r[(word >> 8) & 15]));
  if ((word & 0x00200000) != 0) {
    auto *accumulator = ir_.CreateOr(
        ir_.CreateZExt(values.r[low], ir_.getInt64Ty()),
        ir_.CreateShl(ir_.CreateZExt(values.r[high], ir_.getInt64Ty()), c64(high_word_shift)));
    product = ir_.CreateAdd(product, accumulator);
  }
  values.r[low] = ir_.CreateTrunc(product, ir_.getInt32Ty());
  values.r[high] = ir_.CreateTrunc(ir_.CreateLShr(product, c64(high_word_shift)), ir_.getInt32Ty());
  if ((word & 0x00100000) != 0) {
    auto *flags =
        ir_.CreateOr(ir_.CreateAnd(values.r[high], c32(status_n)),
                     ir_.CreateSelect(ir_.CreateICmpEQ(product, c64(0)), c32(status_z), c32(0)));
    values.status = ir_.CreateOr(ir_.CreateAnd(values.status, c32(~nz_mask)), flags);
  }
}

void Lowering::sticky_overflow(llvm::Value *wide, Values &values) {
  auto *narrow = ir_.CreateTrunc(wide, ir_.getInt32Ty());
  auto *changed = ir_.CreateICmpNE(wide, ir_.CreateSExt(narrow, ir_.getInt64Ty()));
  values.status = ir_.CreateOr(values.status, ir_.CreateSelect(changed, c32(status_q), c32(0)));
}

llvm::Value *Lowering::saturate(llvm::Value *wide, Values &values) {
  sticky_overflow(wide, values);
  auto *low = ir_.CreateSExt(c32(signed_minimum), ir_.getInt64Ty());
  auto *high = ir_.CreateSExt(c32(signed_maximum), ir_.getInt64Ty());
  return ir_.CreateTrunc(
      ir_.CreateSelect(ir_.CreateICmpSLT(wide, low),
                       low,
                       ir_.CreateSelect(ir_.CreateICmpSGT(wide, high), high, wide)),
      ir_.getInt32Ty());
}

void Lowering::saturating(const Instruction &instruction, Values &values) {
  const auto word = instruction.word;
  auto *first = ir_.CreateSExt(values.r[word & 15], ir_.getInt64Ty());
  auto *second = ir_.CreateSExt(values.r[(word >> 16) & 15], ir_.getInt64Ty());
  if ((word & 0x00400000) != 0) {
    second = ir_.CreateSExt(saturate(ir_.CreateAdd(second, second), values), ir_.getInt64Ty());
  }
  auto *result =
      (word & 0x00200000) != 0 ? ir_.CreateSub(first, second) : ir_.CreateAdd(first, second);
  values.r[(word >> 12) & 15] = saturate(result, values);
}

void Lowering::dsp_multiply(const Instruction &instruction, Values &values) {
  const auto word = instruction.word;
  const auto kind = (word >> opcode_shift) & 3;
  const auto destination = (word >> 16) & 15;
  const auto accumulator = (word >> 12) & 15;
  auto half = [&](llvm::Value *value, bool top) {
    if (top) { value = ir_.CreateLShr(value, c32(halfword_shift)); }
    return ir_.CreateSExt(ir_.CreateTrunc(value, ir_.getInt16Ty()), ir_.getInt64Ty());
  };
  auto *first = kind == 1 ? ir_.CreateSExt(values.r[word & 15], ir_.getInt64Ty())
                          : half(values.r[word & 15], (word & 0x20) != 0);
  auto *product = ir_.CreateMul(first, half(values.r[(word >> 8) & 15], (word & 0x40) != 0));
  if (kind == 1) { product = ir_.CreateAShr(product, c64(halfword_shift)); }
  if (kind == 2) {
    auto *old = ir_.CreateOr(ir_.CreateZExt(values.r[accumulator], ir_.getInt64Ty()),
                             ir_.CreateShl(ir_.CreateZExt(values.r[destination], ir_.getInt64Ty()),
                                           c64(high_word_shift)));
    product = ir_.CreateAdd(product, old);
    values.r[accumulator] = ir_.CreateTrunc(product, ir_.getInt32Ty());
    values.r[destination] =
        ir_.CreateTrunc(ir_.CreateLShr(product, c64(high_word_shift)), ir_.getInt32Ty());
    return;
  }
  if (kind == 0 || (kind == 1 && (word & 0x20) == 0)) {
    product = ir_.CreateAdd(product, ir_.CreateSExt(values.r[accumulator], ir_.getInt64Ty()));
    sticky_overflow(product, values);
  }
  values.r[destination] = ir_.CreateTrunc(product, ir_.getInt32Ty());
}
} // namespace armv5::detail
