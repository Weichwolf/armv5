// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"
#include "armv5/memory.hpp"

#include <cstdint>

namespace armv5::detail {
inline constexpr std::uint32_t control_writable = 0xf387, tcm_base_mask = 0xfffff000,
                               tcm_size_mask = 0x3c, tcm_writable = 0xfffff001;

[[nodiscard]] inline bool cp14_transfer(std::uint32_t word) {
  constexpr std::uint32_t mask = 0x0fe00fff;
  constexpr std::uint32_t debug = 0x0e000e10;
  constexpr std::uint32_t jazelle = 0x0ee00e10;
  const auto encoding = word & mask;
  return (encoding == debug || encoding == jazelle) && ((word >> 16) & 15) <= 2;
}

[[nodiscard]] inline bool jazelle_transfer(std::uint32_t word) {
  constexpr std::uint32_t opcode_mask = 0x00e00000;
  return cp14_transfer(word) && (word & opcode_mask) == opcode_mask;
}

[[nodiscard]] inline bool same_mapping(Registers::System::Mapping first,
                                       Registers::System::Mapping second) {
  constexpr std::uint32_t tcm_size_bits = 0x3c;
  if ((first.instruction_tcm & 1) == 0) { first.instruction_tcm &= ~tcm_size_bits; }
  if ((second.instruction_tcm & 1) == 0) { second.instruction_tcm &= ~tcm_size_bits; }
  if ((first.data_tcm & 1) == 0) { first.data_tcm &= ~tcm_size_bits; }
  if ((second.data_tcm & 1) == 0) { second.data_tcm &= ~tcm_size_bits; }
  return first == second;
}

struct TcmRange {
  std::uint32_t base = 0, size = 0;

  [[nodiscard]] bool contains(std::uint32_t address) const { return address - base < size; }
};

[[nodiscard]] inline std::uint32_t tcm_size(std::uint32_t value) {
  const auto encoded = (value >> 2) & 15;
  return encoded >= 3 && encoded <= 11 ? 1U << (encoded + 9) : 0;
}

[[nodiscard]] inline TcmRange tcm_range(std::uint32_t value) {
  return {.base = value & tcm_base_mask, .size = (value & 1) != 0 ? tcm_size(value) : 0};
}

[[nodiscard]] inline bool valid_tcm(std::uint32_t value) {
  const auto size = tcm_size(value);
  const auto encoded = value & tcm_size_mask;
  if ((value & ~(tcm_base_mask | tcm_size_mask | 1U)) != 0) { return false; }
  if (size == 0) { return encoded == 0 && (value & 1) == 0; }
  return (value & tcm_base_mask & (size - 1)) == 0;
}

[[nodiscard]] inline bool valid_tcms(const Registers::System::Mapping &mapping) {
  if (!valid_tcm(mapping.instruction_tcm) || !valid_tcm(mapping.data_tcm)) { return false; }
  const auto instruction = tcm_range(mapping.instruction_tcm);
  const auto data = tcm_range(mapping.data_tcm);
  return instruction.size == 0 || data.size == 0 ||
         std::uint64_t(instruction.base) + instruction.size <= data.base ||
         std::uint64_t(data.base) + data.size <= instruction.base;
}

[[nodiscard]] inline std::uint32_t tcm_register(const Registers::System::Mapping &mapping,
                                                Memory::Space space) {
  return space == Memory::Space::itcm ? mapping.instruction_tcm : mapping.data_tcm;
}
} // namespace armv5::detail
