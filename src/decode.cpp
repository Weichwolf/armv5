// SPDX-License-Identifier: GPL-3.0-only
#include "decode.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace armv5::detail {
namespace {

constexpr unsigned condition_shift = 28, major_shift = 25, opcode_shift = 21;
constexpr std::uint32_t accumulator_mask = 0x0000f000, offset_high_mask = 0x00000f00;

struct Pattern {
  std::uint32_t mask, value;

  [[nodiscard]] bool matches(std::uint32_t word) const { return (word & mask) == value; }
};

// DDI 0100I A3.1/A3.16, restricted to ARMv5TEJ. Named patterns keep the
// extension-space precedence visible and keep reserved fields out of ALU decode.
constexpr Pattern coprocessor_transfer{.mask = 0x0e000000, .value = 0x0c000000};
constexpr Pattern coprocessor_extension{.mask = 0x01a00000, .value = 0};
constexpr Pattern coprocessor_operation{.mask = 0x0f000000, .value = 0x0e000000};
constexpr Pattern branch{.mask = 0x0e000000, .value = 0x0a000000};
constexpr Pattern preload{.mask = 0x0d70f000, .value = 0x0550f000};
constexpr Pattern register_shift{.mask = 0x02000010, .value = 0x02000010};
constexpr Pattern extra_space{.mask = 0x90, .value = 0x90};
constexpr Pattern control_space{.mask = 0x01900000, .value = 0x01000000};
constexpr Pattern saturating{.mask = 0x0f900ff0, .value = 0x01000050};
constexpr Pattern breakpoint{.mask = 0x0ff000f0, .value = 0x01200070};
constexpr Pattern signed_multiply{.mask = 0x0f900090, .value = 0x01000080};
constexpr Pattern swap{.mask = 0x0fb00ff0, .value = 0x01000090};
constexpr Pattern multiply_space{.mask = 0x0f0000f0, .value = 0x00000090};
constexpr Pattern short_multiply{.mask = 0x0fc00000, .value = 0};
constexpr Pattern extra_post_writeback{.mask = 0x01200000, .value = 0x00200000};

[[noreturn]] void reject(std::string_view reason) {
  throw std::runtime_error(std::string(reason));
}

std::string_view coprocessor(std::uint32_t word, bool unconditional) {
  if (coprocessor_transfer.matches(word)) {
    if (coprocessor_extension.matches(word)) {
      if ((word & 0x00400000) == 0 || unconditional) {
        reject("undefined coprocessor extension for ARMv5TEJ");
      }
      return (word & 0x00100000) != 0 ? "MRRC" : "MCRR";
    }
    if ((word & 0x00100000) != 0) { return unconditional ? "LDC2" : "LDC"; }
    return unconditional ? "STC2" : "STC";
  }
  if ((word & 0x10) == 0) { return unconditional ? "CDP2" : "CDP"; }
  if ((word & 0x00100000) != 0) { return unconditional ? "MRC2" : "MRC"; }
  return unconditional ? "MCR2" : "MCR";
}

std::string_view miscellaneous(std::uint32_t word) {
  struct Rule {
    Pattern pattern;
    std::string_view name;
  };

  constexpr std::array rules{
      Rule{.pattern = {.mask = 0x0fbf0fff, .value = 0x010f0000}, .name = "MRS"},
      Rule{.pattern = {.mask = 0x0fb0fff0, .value = 0x0120f000}, .name = "MSR"},
      Rule{.pattern = {.mask = 0x0fb0f000, .value = 0x0320f000}, .name = "MSR"},
      Rule{.pattern = {.mask = 0x0ffffff0, .value = 0x012fff10}, .name = "BX"},
      Rule{.pattern = {.mask = 0x0ffffff0, .value = 0x012fff20}, .name = "BXJ"},
      Rule{.pattern = {.mask = 0x0ffffff0, .value = 0x012fff30}, .name = "BLX register"},
      Rule{.pattern = {.mask = 0x0fff0ff0, .value = 0x016f0f10}, .name = "CLZ"}};
  for (const auto &rule : rules) {
    if (rule.pattern.matches(word)) { return rule.name; }
  }
  if (saturating.matches(word)) {
    constexpr std::array names{"QADD", "QSUB", "QDADD", "QDSUB"};
    return names[(word >> opcode_shift) & 3];
  }
  if (breakpoint.matches(word)) {
    if ((word >> condition_shift) != 14) { reject("unpredictable BKPT condition"); }
    return "BKPT";
  }
  if (signed_multiply.matches(word)) {
    switch ((word >> opcode_shift) & 3) {
      case 0: return "SMLAxy";
      case 1:
        if ((word & 0x20) == 0) { return "SMLAWy"; }
        if ((word & accumulator_mask) != 0) { reject("unpredictable SMULWy reserved field"); }
        return "SMULWy";
      case 2: return "SMLALxy";
      default:
        if ((word & accumulator_mask) != 0) { reject("unpredictable SMULxy reserved field"); }
        return "SMULxy";
    }
  }
  reject("undefined or unpredictable control/DSP encoding for ARMv5TEJ");
}

std::string_view extra_transfer(std::uint32_t word) {
  if (swap.matches(word)) { return (word & 0x00400000) != 0 ? "SWPB" : "SWP"; }
  if (multiply_space.matches(word)) {
    if (short_multiply.matches(word)) {
      if ((word & 0x00200000) != 0) { return "MLA"; }
      if ((word & accumulator_mask) != 0) { reject("unpredictable MUL reserved field"); }
      return "MUL";
    }
    if ((word & 0x00800000) != 0) {
      constexpr std::array names{"UMULL", "UMLAL", "SMULL", "SMLAL"};
      return names[(word >> opcode_shift) & 3];
    }
    reject("undefined multiply extension for ARMv5TEJ");
  }
  const auto kind = (word >> 5) & 3;
  if (kind == 0) { reject("undefined or unpredictable swap/exclusive encoding for ARMv5TEJ"); }
  if ((word & 0x00400000) == 0 && (word & offset_high_mask) != 0) {
    reject("unpredictable extra-transfer register offset reserved field");
  }
  if (extra_post_writeback.matches(word)) {
    reject("unpredictable extra-transfer post-index writeback encoding");
  }
  if ((word & 0x00100000) != 0) {
    constexpr std::array names{"", "LDRH", "LDRSB", "LDRSH"};
    return names[kind];
  }
  constexpr std::array names{"", "STRH", "LDRD", "STRD"};
  return names[kind];
}

std::string_view unconditional_operation(std::uint32_t word) {
  if (branch.matches(word)) { return "BLX immediate"; }
  if (preload.matches(word)) {
    if (register_shift.matches(word)) { reject("undefined PLD register shift encoding"); }
    return "PLD";
  }
  if (coprocessor_transfer.matches(word) || coprocessor_operation.matches(word)) {
    return coprocessor(word, true);
  }
  reject("undefined unconditional encoding for ARMv5TEJ");
}

std::string_view operation(std::uint32_t word) {
  if ((word >> condition_shift) == 15) { return unconditional_operation(word); }
  switch ((word >> major_shift) & 7) {
    case 0:
      // Bits 7 and 4 distinguish multiply/extra transfers before the control
      // extension and ordinary data processing; this order is architectural.
      if (extra_space.matches(word)) { return extra_transfer(word); }
      if (control_space.matches(word)) { return miscellaneous(word); }
      break;
    case 1:
      if (control_space.matches(word)) { return miscellaneous(word); }
      break;
    case 2:
    case 3:
      if (register_shift.matches(word)) { reject("undefined media encoding for ARMv5TEJ"); }
      if ((word & 0x00100000) != 0) { return (word & 0x00400000) != 0 ? "LDRB" : "LDR"; }
      return (word & 0x00400000) != 0 ? "STRB" : "STR";
    case 4: return (word & 0x00100000) != 0 ? "LDM" : "STM";
    case 5: return (word & 0x01000000) != 0 ? "BL" : "B";
    case 6: return coprocessor(word, false);
    default:
      if ((word & 0x01000000) != 0) { return "SWI"; }
      return coprocessor(word, false);
  }
  constexpr std::array names{"AND",
                             "EOR",
                             "SUB",
                             "RSB",
                             "ADD",
                             "ADC",
                             "SBC",
                             "RSC",
                             "TST",
                             "TEQ",
                             "CMP",
                             "CMN",
                             "ORR",
                             "MOV",
                             "BIC",
                             "MVN"};
  return names[(word >> opcode_shift) & 15];
}

} // namespace

std::string_view decode_arm(std::uint32_t word) {
  return operation(word);
}

} // namespace armv5::detail
