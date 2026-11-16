// SPDX-License-Identifier: GPL-3.0-only
#include "image.hpp"
#include "lower.hpp"

#include <array>
#include <cstdint>

namespace armv5::detail {
namespace {
constexpr std::uint32_t always = 0xe0000000, immediate = 0x02000000, set_flags = 0x00100000,
                        memory_base = 0xe5800000, extra_base = 0xe1800090;
constexpr std::uint32_t branch_encoding = 0x0a000000, undefined_encoding = 0xe7f000f0,
                        software_interrupt_encoding = 0xef000000;
constexpr std::uint16_t add_sub_start = 0x1800, high_register_start = 0x4400,
                        literal_start = 0x4800, immediate_memory_start = 0x6000,
                        stack_memory_start = 0x9000, address_start = 0xa000,
                        miscellaneous_start = 0xb000, multiple_start = 0xc000,
                        conditional_start = 0xd000, branch_start = 0xe000,
                        exchange_suffix_start = 0xe800, stack_adjust_mask = 0xff00,
                        push_pop_mask = 0xf600, push_pop_encoding = 0xb400,
                        breakpoint_encoding = 0xbe00, high_register_bits = 0xc0,
                        stack_adjust_offset = 0x7f;
constexpr unsigned thumb_immediate_mask = 0x7ff;

std::uint32_t data(unsigned opcode,
                   bool flags,
                   unsigned destination,
                   unsigned left,
                   unsigned right,
                   bool constant = false) {
  return always | (opcode << opcode_shift) | (flags ? set_flags : 0) | (destination << 12) |
         (left << 16) | right | (constant ? immediate : 0);
}

std::uint32_t single(bool reading,
                     bool byte,
                     unsigned destination,
                     unsigned base,
                     unsigned offset,
                     bool register_offset = false) {
  return memory_base | (reading ? set_flags : 0) | (byte ? 0x00400000 : 0) | 0x01000000 |
         (destination << 12) | (base << 16) | offset | (register_offset ? immediate : 0);
}

std::uint32_t extra(bool reading,
                    unsigned kind,
                    unsigned destination,
                    unsigned base,
                    unsigned offset,
                    bool constant) {
  return extra_base | 0x01000000 | (reading ? set_flags : 0) | (kind << 5) | (destination << 12) |
         (base << 16) |
         (constant ? 0x00400000 | ((offset & high_offset_mask) << 4) | (offset & 15) : offset);
}

std::uint32_t low_alu(std::uint16_t word) {
  const auto opcode = (word >> 6) & 15;
  const auto destination = word & 7;
  const auto source = (word >> 3) & 7;
  if (opcode == 2 || opcode == 3 || opcode == 4 || opcode == 7) {
    const auto kind = opcode == 7 ? 3 : opcode - 2;
    return data(13, true, destination, 0, destination | 0x10 | (kind << 5) | (source << 8));
  }
  if (opcode == 9) { return data(3, true, destination, source, 0, true); }
  if (opcode == 13) {
    constexpr std::uint32_t muls = 0xe0100090;
    return muls | (destination << 16) | destination | (source << 8);
  }
  const bool test = opcode >= 8 && opcode <= 11;
  const auto first = opcode == 15 ? 0U : destination;
  return data(opcode, true, test ? 0 : destination, first, source);
}

std::uint32_t register_memory(std::uint16_t word) {
  const auto kind = (word >> 9) & 7;
  const auto destination = word & 7;
  const auto base = (word >> 3) & 7;
  const auto offset = (word >> 6) & 7;
  if (kind == 0 || kind == 2 || kind == 4 || kind == 6) {
    return single(kind >= 4, (kind & 2) != 0, destination, base, offset, true);
  }
  unsigned form = 1;
  if (kind == 3) { form = 2; }
  if (kind == 7) { form = 3; }
  return extra(kind != 1, form, destination, base, offset, false);
}
} // namespace

namespace {
struct ThumbDecoder {
  const Image &image;
  std::uint32_t pc;
  std::uint16_t word;
  unsigned destination, source, small, immediate8, extended_destination;

  [[nodiscard]] Instruction normalized(std::uint32_t arm, bool aligned_pc = false) const {
    auto result = classify_arm(image, pc, arm);
    result.size = 2;
    result.aligned_pc = aligned_pc;
    return result;
  }

  [[nodiscard]] Instruction branch(unsigned condition, std::uint32_t offset) const {
    return {.pc = pc,
            .word = (condition << condition_shift) | branch_encoding,
            .operation = Operation::branch,
            .cycles = 3,
            .terminates = true,
            .size = 2,
            .target = pc + 4 + offset};
  }

  [[noreturn]] void invalid() const {
    refuse(image.identity, pc, "undefined Thumb extension outside ARMv5TEJ");
  }

  [[nodiscard]] Instruction low() const {
    if (word < add_sub_start) {
      return normalized(
          data(13, true, destination, 0, source | (small << 7) | ((word >> 11) << 5)));
    }
    if (word < 0x2000) {
      return normalized(data((word & 0x200) != 0 ? 2 : 4,
                             true,
                             destination,
                             source,
                             (word >> 6) & 7,
                             (word & 0x400) != 0));
    }
    if (word < 0x4000) {
      constexpr std::array opcodes{13U, 10U, 4U, 2U};
      const auto kind = (word >> 11) & 3;
      return normalized(data(opcodes[kind],
                             true,
                             kind == 1 ? 0 : extended_destination,
                             kind == 0 ? 0 : extended_destination,
                             immediate8,
                             true));
    }
    invalid();
  }

  [[nodiscard]] Instruction registers() const {
    if (word < high_register_start) { return normalized(low_alu(word)); }
    if (word < literal_start) {
      const auto kind = (word >> 8) & 3;
      const auto high_destination = destination | ((word >> 4) & 8);
      const auto high_source = (word >> 3) & 15;
      if (kind == 3) {
        if ((word & 7) != 0 || ((word & 0x80) != 0 && high_source == 15)) {
          refuse(image.identity, pc, "unpredictable Thumb BX/BLX fields/PC");
        }
        constexpr std::uint32_t bx = 0xe12fff10;
        return normalized(bx | high_source | ((word & 0x80) != 0 ? 0x20 : 0));
      }
      if ((word & high_register_bits) == 0) {
        refuse(image.identity, pc, "unpredictable low-register high-operation form");
      }
      if (kind == 1 && high_destination == 15) {
        refuse(image.identity, pc, "unpredictable Thumb CMP first operand PC");
      }
      constexpr std::array opcodes{4U, 10U, 13U};
      return normalized(data(opcodes[kind],
                             kind == 1,
                             kind == 1 ? 0 : high_destination,
                             kind == 2 ? 0 : high_destination,
                             high_source));
    }
    return normalized(single(true, false, extended_destination, 15, immediate8 * 4), true);
    invalid();
  }

  [[nodiscard]] Instruction memory() const {
    if (word < immediate_memory_start) { return normalized(register_memory(word)); }
    if (word < 0x8000) {
      const bool byte = (word & 0x1000) != 0;
      return normalized(
          single((word & 0x800) != 0, byte, destination, source, small * (byte ? 1 : 4)));
    }
    if (word < stack_memory_start) {
      return normalized(extra((word & 0x800) != 0, 1, destination, source, small * 2, true));
    }
    if (word < address_start) {
      return normalized(
          single((word & 0x800) != 0, false, extended_destination, 13, immediate8 * 4));
    }
    if (word < miscellaneous_start) {
      const auto base = (word & 0x800) != 0 ? 13U : 15U;
      constexpr unsigned times_four = 15U << 8;
      return normalized(data(4, false, extended_destination, base, immediate8 | times_four, true),
                        true);
    }
    invalid();
  }

  [[nodiscard]] Instruction stack() const {
    if ((word & stack_adjust_mask) == miscellaneous_start) {
      constexpr unsigned times_four = 15U << 8;
      return normalized(data((word & 0x80) != 0 ? 2 : 4,
                             false,
                             13,
                             13,
                             (word & stack_adjust_offset) | times_four,
                             true));
    }
    if ((word & push_pop_mask) == push_pop_encoding) {
      const bool pop = (word & 0x800) != 0;
      const auto extra_register = pop ? 0x8000U : 0x4000U;
      const auto list = immediate8 | ((word & 0x100) != 0 ? extra_register : 0);
      constexpr std::uint32_t push = 0xe92d0000;
      constexpr std::uint32_t pop_encoding = 0xe8bd0000;
      return normalized((pop ? pop_encoding : push) | list);
    }
    if ((word & stack_adjust_mask) == breakpoint_encoding) {
      constexpr std::uint32_t breakpoint = 0xe1200070;
      return normalized(breakpoint | ((immediate8 & high_offset_mask) << 4) | (immediate8 & 15));
    }
    invalid();
  }

  [[nodiscard]] Instruction control() const {
    if (word >= multiple_start && word < conditional_start) {
      const bool reading = (word & 0x800) != 0;
      const bool writeback = !reading || (immediate8 & (1U << extended_destination)) == 0;
      constexpr std::uint32_t multiple = 0xe8800000;
      return normalized(multiple | (reading ? set_flags : 0) | (writeback ? 0x00200000 : 0) |
                        (extended_destination << 16) | immediate8);
    }
    if (word >= conditional_start && word < branch_start) {
      const auto condition = (word >> 8) & 15;
      if (condition < 14) { return branch(condition, ((immediate8 << 1) ^ 0x100) - 0x100); }
      if (condition == 15) { return normalized(software_interrupt_encoding | immediate8); }
      return normalized(undefined_encoding);
    }
    if (word >= branch_start && word < exchange_suffix_start) {
      const auto offset = (word & thumb_immediate_mask) << 1;
      return branch(14, (offset ^ 0x800) - 0x800);
    }
    if (word >= exchange_suffix_start) {
      const bool prefix = (word & 0xf800) == 0xf000;
      const bool exchange = (word & 0xf800) == exchange_suffix_start;
      if (exchange && (word & 1) != 0) { return normalized(undefined_encoding); }
      return {.pc = pc,
              .word = always | word,
              .operation = prefix ? Operation::thumb_link_prefix : Operation::thumb_link_suffix,
              .cycles = prefix ? 1U : 3U,
              .terminates = !prefix,
              .size = 2,
              .exchange = exchange,
              .continuation_only = !prefix};
    }
    invalid();
  }

  [[nodiscard]] Instruction decode() const {
    switch (word >> 12) {
      case 0:
      case 1:
      case 2:
      case 3: return low();
      case 4: return registers();
      case 5:
      case 6:
      case 7:
      case 8:
      case 9:
      case 10: return memory();
      case 11: return stack();
      default: return control();
    }
  }
};
} // namespace

Instruction Lowering::thumb_instruction(std::uint32_t pc, std::uint16_t word) {
  return ThumbDecoder{.image = image_,
                      .pc = pc,
                      .word = word,
                      .destination = unsigned(word & 7),
                      .source = unsigned((word >> 3) & 7),
                      .small = unsigned((word >> 6) & shift_count_mask),
                      .immediate8 = unsigned(word & immediate_mask),
                      .extended_destination = unsigned((word >> 8) & 7)}
      .decode();
}
} // namespace armv5::detail
