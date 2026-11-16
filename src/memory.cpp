// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "image.hpp"
#include "lower.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/Alignment.h>
#include <vector>

namespace armv5::detail {
llvm::Value *Lowering::slow_access(
    Values &values, llvm::Value *address, unsigned width, unsigned operation, llvm::Value *value) {
  materialize(values); // Only the dynamic callback path exposes the current prefix.
  auto *replacement = c64(0);
  auto role = CallbackBinding::Role::swap;
  if (operation == 0) { role = CallbackBinding::Role::read; }
  if (operation == 1) { role = CallbackBinding::Role::write; }
  for (const auto &binding : image_.options.callbacks) {
    if (binding.role != role) { continue; }
    llvm::Type *last = ir_.getInt32Ty();
    if (operation != 1) { last = ir_.getPtrTy(); }
    auto *type = llvm::FunctionType::get(
        ir_.getInt1Ty(), {ir_.getPtrTy(), ir_.getInt32Ty(), ir_.getInt32Ty(), last}, false);
    auto callback = module_.getOrInsertFunction(binding.symbol, type);
    replacement = ir_.CreatePtrToInt(callback.getCallee(), ir_.getInt64Ty());
  }
  auto *type = llvm::FunctionType::get(ir_.getInt64Ty(),
                                       {ir_.getPtrTy(),
                                        ir_.getInt32Ty(),
                                        ir_.getInt32Ty(),
                                        ir_.getInt32Ty(),
                                        ir_.getInt32Ty(),
                                        ir_.getInt64Ty()},
                                       false);
  auto *callee = load(ir_.getPtrTy(), offsetof(Runtime, access));
  auto *result = ir_.CreateCall(
      type, callee, {runtime_, address, c32(width), c32(operation), value, replacement});
  auto *ordinary = block("memory.accepted");
  auto *failed = block("memory.failed");
  cold_branch(ir_.CreateICmpEQ(ir_.CreateLShr(result, c64(32)), c64(0)), ordinary, failed);
  ir_.SetInsertPoint(failed);
  finish_exit(values, load(ir_.getInt32Ty(), offsetof(Runtime, signal)), address);
  ir_.SetInsertPoint(ordinary);
  pending_signal_ = load(ir_.getInt32Ty(), offsetof(Runtime, signal));
  return ir_.CreateTrunc(result, ir_.getInt32Ty());
}

llvm::Value *Lowering::access(
    Values &values, llvm::Value *address, unsigned width, unsigned operation, llvm::Value *value) {
  if (operation == 2 || variant_.layout.empty()) {
    return slow_access(values, address, width, operation, value);
  }
  auto *previous_signal = pending_signal_;
  auto *merged = block("memory.result");
  llvm::IRBuilder<> phi_builder(merged);
  auto *result = phi_builder.CreatePHI(ir_.getInt32Ty(), unsigned(variant_.layout.size() + 1));
  auto *signal = phi_builder.CreatePHI(ir_.getInt32Ty(), unsigned(variant_.layout.size() + 1));
  std::vector<std::size_t> order;
  order.reserve(variant_.layout.size());
  for (std::size_t index = 0; index < variant_.layout.size(); ++index) { order.push_back(index); }
  std::ranges::stable_sort(order, {}, [&](std::size_t index) {
    const auto &region = variant_.layout[index];
    return !region.readable || !region.writable || region.executable;
  });
  for (const auto index : order) {
    const auto &region = variant_.layout[index];
    auto *mapped = block("memory.direct");
    auto *next = block("memory.next");
    auto *wide_address = ir_.CreateZExt(address, ir_.getInt64Ty());
    // Unsigned underflow rejects addresses below the region in the same compare.
    auto *offset = ir_.CreateSub(wide_address, c64(region.address));
    auto *within =
        region.size < width ? ir_.getFalse() : ir_.CreateICmpULE(offset, c64(region.size - width));
    ir_.CreateCondBr(within, mapped, next);
    ir_.SetInsertPoint(mapped);
    const bool allowed = operation == 0 ? region.readable : region.writable;
    guard(ir_.getInt1(allowed), values, Signal::data_abort, address);
    auto *descriptor = ir_.CreateGEP(ir_.getInt8Ty(), direct_, c64(index * sizeof(DirectRegion)));
    if (operation == 1) {
      auto *safe = ir_.CreateLoad(
          ir_.getInt32Ty(),
          ir_.CreateGEP(ir_.getInt8Ty(), descriptor, c64(offsetof(DirectRegion, safe_write))));
      auto *write = block("memory.store");
      ir_.CreateCondBr(ir_.CreateICmpNE(safe, c32(0)), write, next);
      ir_.SetInsertPoint(write);
    }
    auto *bias = ir_.CreateLoad(ir_.getInt64Ty(), descriptor);
    auto *pointer = ir_.CreateIntToPtr(ir_.CreateAdd(bias, wide_address), ir_.getPtrTy());
    auto *type = ir_.getIntNTy(width * 8);
    llvm::Value *loaded = c32(0);
    const bool swap_endian = image_.options.big_endian != (std::endian::native == std::endian::big);
    if (operation == 0) {
      loaded = ir_.CreateAlignedLoad(type, pointer, llvm::Align(1));
      if (swap_endian && width > 1) {
        loaded = ir_.CreateIntrinsic(llvm::Intrinsic::bswap, {type}, {loaded});
      }
      loaded = ir_.CreateZExtOrTrunc(loaded, ir_.getInt32Ty());
    } else {
      auto *stored = ir_.CreateTruncOrBitCast(value, type);
      if (swap_endian && width > 1) {
        stored = ir_.CreateIntrinsic(llvm::Intrinsic::bswap, {type}, {stored});
      }
      ir_.CreateAlignedStore(stored, pointer, llvm::Align(1));
    }
    result->addIncoming(loaded, ir_.GetInsertBlock());
    signal->addIncoming(previous_signal, ir_.GetInsertBlock());
    ir_.CreateBr(merged);
    ir_.SetInsertPoint(next);
  }
  auto *dynamic = slow_access(values, address, width, operation, value);
  result->addIncoming(dynamic, ir_.GetInsertBlock());
  signal->addIncoming(pending_signal_, ir_.GetInsertBlock());
  ir_.CreateBr(merged);
  ir_.SetInsertPoint(merged);
  pending_signal_ = signal;
  return result;
}

void Lowering::swap_memory(const Instruction &instruction, Values &values) {
  const auto word = instruction.word;
  const auto base_index = (word >> 16) & 15;
  const auto destination = (word >> 12) & 15;
  if (base_index == 15 || destination == 15 || (word & 15) == 15 || base_index == destination ||
      base_index == (word & 15)) {
    refuse(image_.identity, instruction.pc, "unpredictable SWP register overlap/PC");
  }
  auto *base = values.r[base_index];
  const unsigned width = (word & 0x00400000) != 0 ? 1 : 4;
  auto *address = width == 4 ? ir_.CreateAnd(base, c32(arm_pc_mask)) : base;
  auto *loaded = access(values, address, width, 2, values.r[word & 15]);
  if (width == 4) {
    loaded =
        ir_.CreateIntrinsic(llvm::Intrinsic::fshr,
                            {ir_.getInt32Ty()},
                            {loaded, loaded, ir_.CreateMul(ir_.CreateAnd(base, c32(3)), c32(8))});
  }
  values.r[destination] = loaded;
}

Lowering::Transfer Lowering::transfer_plan(const Instruction &instruction) {
  const auto word = instruction.word;
  const auto base = (word >> 16) & 15;
  const auto destination = (word >> 12) & 15;
  const bool extra = instruction.operation == Operation::extra;
  const unsigned kind = (word >> 5) & 3;
  const bool pair = extra && (word & 0x00100000) == 0 && kind >= 2;
  const bool reading = pair ? kind == 2 : (word & 0x00100000) != 0;
  const bool pre = (word & 0x01000000) != 0;
  const bool writeback = !pre || (word & 0x00200000) != 0;
  const bool register_offset = extra ? (word & 0x00400000) == 0 : (word & 0x02000000) != 0;
  unsigned width = (word & 0x00400000) != 0 ? 1 : 4;
  if (extra) {
    width = kind == 2 ? 1 : 2;
    if (pair) { width = 4; }
  }
  return {.base = base,
          .destination = destination,
          .width = width,
          .count = pair ? 2U : 1U,
          .extra = extra,
          .reading = reading,
          .pre = pre,
          .up = (word & 0x00800000) != 0,
          .writeback = writeback,
          .register_offset = register_offset,
          .sign_extend = extra && !pair && kind >= 2};
}

void Lowering::validate_transfer(const Instruction &instruction, const Transfer &plan) const {
  const auto offset_register = instruction.word & 15;
  const bool pair = plan.count == 2;
  const bool base_overlap =
      plan.base == plan.destination || (pair && plan.base == plan.destination + 1);
  if (plan.writeback && (plan.base == 15 || ((plan.reading || pair) && base_overlap))) {
    refuse(image_.identity, instruction.pc, "unpredictable load/store writeback overlap/PC");
  }
  if (plan.register_offset && offset_register == 15) {
    refuse(image_.identity, instruction.pc, "unpredictable transfer PC offset");
  }
  if (pair && plan.reading && plan.register_offset &&
      (offset_register == plan.destination || offset_register == plan.destination + 1)) {
    refuse(image_.identity, instruction.pc, "unpredictable doubleword index/destination overlap");
  }
}

llvm::Value *Lowering::transfer_offset(const Instruction &instruction,
                                       const Values &values,
                                       const Transfer &plan) {
  const auto word = instruction.word;
  if (plan.extra) {
    return plan.register_offset ? values.r[word & 15]
                                : c32(((word >> 4) & high_offset_mask) | (word & 15));
  }
  if (plan.register_offset) {
    return shift(values.r[word & 15],
                 (word >> 5) & 3,
                 c32((word >> 7) & shift_count_mask),
                 true,
                 flag(values.status, status_c))
        .value;
  }
  return c32(word & transfer_offset_mask);
}

void Lowering::memory(const Instruction &instruction, Values &values) {
  if (instruction.operation == Operation::swap) {
    swap_memory(instruction, values);
    return;
  }
  const auto plan = transfer_plan(instruction);
  validate_transfer(instruction, plan);
  auto *base = reg(instruction, values, plan.base);
  auto *offset = transfer_offset(instruction, values, plan);
  auto *adjusted = plan.up ? ir_.CreateAdd(base, offset) : ir_.CreateSub(base, offset);
  auto *address = plan.pre ? adjusted : base;
  if (plan.extra && plan.width > 1) {
    guard(ir_.CreateICmpEQ(ir_.CreateAnd(address, c32(plan.count == 2 ? 7 : 1)), c32(0)),
          values,
          Signal::unsupported,
          address);
  }
  auto *aligned = plan.width == 4 ? ir_.CreateAnd(address, c32(arm_pc_mask)) : address;
  for (unsigned index = 0; index < plan.count; ++index) {
    auto *access_address = index == 0 ? aligned : ir_.CreateAdd(aligned, c32(4));
    auto *value = reg(instruction, values, plan.destination + index);
    auto *loaded = access(values, access_address, plan.width, plan.reading ? 0 : 1, value);
    if (!plan.reading) { continue; }
    if (plan.width == 4 && plan.count == 1) {
      loaded = ir_.CreateIntrinsic(
          llvm::Intrinsic::fshr,
          {ir_.getInt32Ty()},
          {loaded, loaded, ir_.CreateMul(ir_.CreateAnd(address, c32(3)), c32(8))});
    }
    if (plan.sign_extend) {
      loaded =
          ir_.CreateSExt(ir_.CreateTrunc(loaded, ir_.getIntNTy(plan.width * 8)), ir_.getInt32Ty());
    }
    values.r[plan.destination + index] = loaded;
  }
  if (plan.writeback) { values.r[plan.base] = adjusted; }
}

bool Lowering::multiple_user_bank(const Instruction &instruction) const {
  const auto word = instruction.word;
  constexpr std::uint32_t list_mask = 0xffff;
  const auto list = word & list_mask;
  const auto base_index = (word >> 16) & 15;
  const bool reading = (word & 0x00100000) != 0;
  const bool writeback = (word & 0x00200000) != 0;
  const bool user = (word & 0x00400000) != 0 && !exception_return(instruction);
  if ((word & 0x00400000) != 0 && (mode_ == user_mode || mode_ == system_mode)) {
    refuse(image_.identity, instruction.pc, "unpredictable user-bank transfer in User/System mode");
  }
  if (user && writeback) {
    refuse(image_.identity, instruction.pc, "unpredictable user-bank transfer with writeback");
  }
  if (writeback && (list & (1U << base_index)) != 0 &&
      (reading || unsigned(std::countr_zero(list)) != base_index)) {
    refuse(image_.identity, instruction.pc, "unpredictable/unknown multiple writeback overlap");
  }
  return user;
}

void Lowering::multiple(const Instruction &instruction, Values &values) {
  const auto word = instruction.word;
  constexpr std::uint32_t list_mask = 0xffff;
  const auto list = word & list_mask;
  const auto count = unsigned(std::popcount(list));
  const auto base_index = (word >> 16) & 15;
  const bool reading = (word & 0x00100000) != 0;
  const bool writeback = (word & 0x00200000) != 0;
  const bool user = multiple_user_bank(instruction);
  const bool up = (word & 0x00800000) != 0;
  const bool pre = (word & 0x01000000) != 0;
  auto *base = values.r[base_index];
  // ARMv5 multiple transfers ignore address bits 1:0 when alignment checking is off.
  auto *start = up ? ir_.CreateAdd(base, c32(pre ? 4 : 0))
                   : ir_.CreateSub(base, c32((count * 4) - (pre ? 0 : 4)));
  start = ir_.CreateAnd(start, c32(arm_pc_mask));
  unsigned offset = 0;
  for (unsigned index = 0; index < 16; ++index) {
    if ((list & (1U << index)) == 0) { continue; }
    auto *address = ir_.CreateAdd(start, c32(offset));
    auto *stored = user && index != 15 ? user_reg(values, index) : reg(instruction, values, index);
    auto *loaded = access(values, address, 4, reading ? 0 : 1, stored);
    if (reading) {
      if (user) {
        set_user_reg(values, index, loaded);
      } else {
        values.r[index] = loaded;
      }
    }
    offset += 4;
  }
  if (writeback) {
    values.r[base_index] =
        up ? ir_.CreateAdd(base, c32(count * 4)) : ir_.CreateSub(base, c32(count * 4));
  }
}
} // namespace armv5::detail
