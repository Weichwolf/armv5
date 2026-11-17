// SPDX-License-Identifier: GPL-3.0-only
#include "system.hpp"

#include "armv5/core.hpp"
#include "image.hpp"
#include "lower.hpp"
#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <llvm/IR/Value.h>
#include <optional>

namespace armv5::detail {
namespace {
using System = Registers::System;
using Mapping = System::Mapping;
constexpr auto system_base = offsetof(Runtime, system);
constexpr auto mapping_base = system_base + offsetof(System, mapping);
constexpr std::uint32_t processor_id = 0x41069265, cache_type = 0x1d112152, table_mask = 0xffffc000,
                        process_mask = 0xfe000000;

struct RegisterSelection {
  unsigned number, selector;
};

std::optional<std::size_t> register_offset(RegisterSelection reg) {
  const auto [number, selector] = reg;
  switch (number) {
    case 1: return mapping_base + offsetof(Mapping, control);
    case 2: return mapping_base + offsetof(Mapping, translation_table);
    case 3: return mapping_base + offsetof(Mapping, domains);
    case 5:
      return system_base + (selector == 0 ? offsetof(System, data_fault_status)
                                          : offsetof(System, instruction_fault_status));
    case 6: return system_base + offsetof(System, fault_address);
    case 9:
      switch (selector) {
        case 0: return system_base + offsetof(System, data_cache_lockdown);
        case 1: return system_base + offsetof(System, instruction_cache_lockdown);
        case 8: return mapping_base + offsetof(Mapping, data_tcm);
        case 9: return mapping_base + offsetof(Mapping, instruction_tcm);
        default: return std::nullopt;
      }
    case 10: return system_base + offsetof(System, tlb_lockdown);
    case 13:
      return selector == 0 ? mapping_base + offsetof(Mapping, process_id)
                           : system_base + offsetof(System, context_id);
    default: return std::nullopt;
  }
}

bool selector_valid(RegisterSelection reg) {
  const auto [number, selector] = reg;
  switch (number) {
    case 0: return selector < 8;
    case 1:
    case 2:
    case 3:
    case 6:
    case 10: return selector == 0;
    case 5:
    case 13: return selector < 2;
    case 9: return selector == 0 || selector == 1 || selector == 8 || selector == 9;
    default: return false;
  }
}
} // namespace

llvm::Value *
Lowering::tcm_write(const Instruction &instruction, Values &values, llvm::Value *input) {
  const bool instruction_side = ((instruction.word >> 5) & 7) == 1;
  const auto offset = mapping_base + (instruction_side ? offsetof(Mapping, instruction_tcm)
                                                       : offsetof(Mapping, data_tcm));
  auto *size_bits = ir_.CreateAnd(load(ir_.getInt32Ty(), offset), c32(tcm_size_mask));
  auto *present = ir_.CreateICmpNE(size_bits, c32(0));
  auto *size = ir_.CreateShl(c32(1), ir_.CreateAdd(ir_.CreateLShr(size_bits, c32(2)), c32(9)));
  auto *base = ir_.CreateAnd(input, c32(tcm_base_mask));
  auto *enabled = ir_.CreateICmpNE(ir_.CreateAnd(input, c32(1)), c32(0));
  auto *aligned = ir_.CreateICmpEQ(ir_.CreateAnd(base, ir_.CreateSub(size, c32(1))), c32(0));
  auto *valid = ir_.CreateAnd(ir_.CreateOr(ir_.CreateNot(enabled), present),
                              ir_.CreateOr(ir_.CreateNot(present), aligned));
  const auto other =
      tcm_range(instruction_side ? variant_.mapping.data_tcm : variant_.mapping.instruction_tcm);
  if (other.size != 0) {
    auto *wide = ir_.CreateZExt(base, ir_.getInt64Ty());
    auto *end = ir_.CreateAdd(wide, ir_.CreateZExt(size, ir_.getInt64Ty()));
    auto *disjoint =
        ir_.CreateOr(ir_.CreateICmpUGE(wide, c64(std::uint64_t(other.base) + other.size)),
                     ir_.CreateICmpULE(end, c64(other.base)));
    valid = ir_.CreateAnd(valid, ir_.CreateOr(ir_.CreateNot(enabled), disjoint));
  }
  guard(valid, values, Signal::unsupported, c32(instruction.pc));
  return ir_.CreateOr(ir_.CreateAnd(input, c32(tcm_writable)), size_bits);
}

void Lowering::write_system(const Instruction &instruction, Values &values, std::size_t offset) {
  const auto word = instruction.word;
  const auto number = (word >> 16) & 15;
  const auto selector = ((word & 15) << 3) | ((word >> 5) & 7);
  auto *value = reg(instruction, values, (word >> 12) & 15);
  switch (number) {
    case 1:
      value =
          ir_.CreateOr(ir_.CreateAnd(value, c32(control_writable)), c32(Mapping::reset_control));
      break;
    case 2: value = ir_.CreateAnd(value, c32(table_mask)); break;
    case 3:
    case 6: break;
    case 5: value = ir_.CreateAnd(value, c32(immediate_mask)); break;
    case 9:
      if (selector < 8) {
        refuse(
            image_.identity, instruction.pc, "CP15 cache lockdown effects are not implemented yet");
      }
      value = tcm_write(instruction, values, value);
      break;
    case 13:
      if (selector == 0) { value = ir_.CreateAnd(value, c32(process_mask)); }
      break;
    default:
      refuse(image_.identity, instruction.pc, "CP15 write requires pending memory-system support");
  }
  store(value, offset);
}

void Lowering::system_transfer(const Instruction &instruction, Values &values) {
  const auto word = instruction.word;
  const auto number = (word >> 16) & 15;
  const auto selector = ((word & 15) << 3) | ((word >> 5) & 7);
  if (((word >> opcode_shift) & 7) != 0 ||
      !selector_valid({.number = number, .selector = selector})) {
    refuse(image_.identity, instruction.pc, "unsupported CP15 register/operation selector");
  }
  const bool reading = instruction.operation == Operation::system_read;
  const auto offset = register_offset({.number = number, .selector = selector});
  const auto destination = (word >> 12) & 15;
  if (!reading) {
    if (!offset) { refuse(image_.identity, instruction.pc, "CP15 register is not writable"); }
    write_system(instruction, values, *offset);
    return;
  }
  llvm::Value *value = nullptr;
  if (number == 0) {
    value = selector == 1 ? c32(cache_type) : c32(processor_id);
    if (selector == 2) { value = load(ir_.getInt32Ty(), offsetof(Runtime, tcm_status)); }
  } else if (offset) {
    value = load(ir_.getInt32Ty(), *offset);
  }
  if (value == nullptr) {
    refuse(image_.identity, instruction.pc, "CP15 register is not readable");
  }
  if (destination == 15) {
    values.status = ir_.CreateOr(ir_.CreateAnd(values.status, c32(~nzcv_mask)),
                                 ir_.CreateAnd(value, c32(nzcv_mask)));
  } else {
    values.r[destination] = value;
  }
}
} // namespace armv5::detail
