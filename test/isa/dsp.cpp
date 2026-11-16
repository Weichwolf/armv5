// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
constexpr std::uint32_t negative = 0x80000000, zero = 0x40000000, qflag = 0x08000000,
                        initial_status = 0x38000013, half_mask = 0xffff;
constexpr unsigned upper_word = 32, upper_half = 16;
constexpr std::array edges{0U,
                           1U,
                           0xffffffffU,
                           0x80000000U,
                           0x7fffffffU,
                           0x80008000U,
                           0x7fff7fffU,
                           0x1234fedcU,
                           0x0000ffffU,
                           0xffff0000U,
                           0x40000000U,
                           0xc0000000U,
                           0x3fffffffU,
                           0xbfffffffU};

std::int64_t signed_word(std::uint32_t value) {
  return std::bit_cast<std::int32_t>(value);
}

std::int64_t half(std::uint32_t value, bool top) {
  const auto bits = (value >> (top ? upper_half : 0)) & half_mask;
  return std::int64_t(bits) - ((bits & 0x8000) != 0 ? 0x10000 : 0);
}

// Division and remainder define floor without relying on the host's signed shift.
std::int64_t scale_half(std::int64_t value) {
  constexpr std::int64_t divisor = 0x10000;
  return (value / divisor) - (value < 0 && value % divisor != 0 ? 1 : 0);
}

std::uint32_t clamp(std::int64_t value, bool &overflow) {
  constexpr auto low = std::numeric_limits<std::int32_t>::min();
  constexpr auto high = std::numeric_limits<std::int32_t>::max();
  overflow |= value < low || value > high;
  return std::uint32_t(std::clamp(value, std::int64_t(low), std::int64_t(high)));
}

struct Encoding {
  std::uint32_t word;
  std::uint64_t cycles;
};

template <class Oracle> void verify(Encoding instruction, Oracle oracle) {
  fixture::Program program;
  program.word(instruction.word);
  fixture::Machine machine(program);
  machine.prepare();
  for (const auto first : edges) {
    for (const auto second : edges) {
      for (const bool sticky : {false, true}) {
        machine.core->reset(program.address());
        auto input = machine.core->registers();
        for (unsigned index = 0; index < 15; ++index) { input.r[index] = first ^ (index * second); }
        input.r[1] = first;
        input.r[2] = second;
        input.cpsr = (initial_status & ~qflag) | (sticky ? qflag : 0);
        auto expected = input;
        oracle(expected);
        expected.r[15] += 4;
        machine.core->set_registers(input);
        const auto result = machine.run(1);
        fixture::require(
            machine.core->registers() == expected,
            "long/DSP multiply, CLZ or saturating arithmetic complete register output");
        fixture::require(result.progress.instructions == 1 &&
                             result.progress.base_cycles == instruction.cycles &&
                             machine.ram == std::vector<std::byte>(machine.ram.size()),
                         "DSP instruction retirement or unexpected RAM write");
      }
    }
  }
}

void long_expected(armv5::Registers &regs, bool signed_product, bool accumulate, bool flags) {
  std::uint64_t value = signed_product
                            ? std::uint64_t(signed_word(regs.r[1]) * signed_word(regs.r[2]))
                            : std::uint64_t(regs.r[1]) * regs.r[2];
  if (accumulate) { value += (std::uint64_t(regs.r[3]) << upper_word) | regs.r[0]; }
  regs.r[0] = std::uint32_t(value);
  regs.r[3] = std::uint32_t(value >> upper_word);
  if (flags) {
    regs.cpsr = (regs.cpsr & ~(negative | zero)) | (regs.r[3] & negative) | (value == 0 ? zero : 0);
  }
}

void long_products() {
  for (const bool signed_product : {false, true}) {
    for (const bool accumulate : {false, true}) {
      for (const bool flags : {false, true}) {
        // Distinct output pair R3:R0; both multiplicands remain live.
        constexpr std::uint32_t base = 0xe0800291;
        const auto word = base | (3U << 16) | (unsigned(signed_product) << 22) |
                          (unsigned(accumulate) << 21) | (unsigned(flags) << 20);
        verify({.word = word, .cycles = flags ? 5U : 3U}, [=](armv5::Registers &regs) {
          long_expected(regs, signed_product, accumulate, flags);
        });
      }
    }
  }
  // Output/input aliases must read the complete old operands before either write.
  constexpr std::uint32_t alias_product = 0xe0e01291; // SMLAL R1,R0,R1,R2
  verify({.word = alias_product, .cycles = 3}, [](armv5::Registers &regs) {
    const auto value = std::uint64_t(signed_word(regs.r[1]) * signed_word(regs.r[2])) +
                       ((std::uint64_t(regs.r[0]) << upper_word) | regs.r[1]);
    regs.r[1] = std::uint32_t(value);
    regs.r[0] = std::uint32_t(value >> upper_word);
  });
}

void half_expected(armv5::Registers &regs, bool top_first, bool top_second, unsigned kind) {
  const auto product = half(regs.r[1], top_first) * half(regs.r[2], top_second);
  if (kind == 2) {
    const auto value =
        ((std::uint64_t(regs.r[0]) << upper_word) | regs.r[3]) + std::uint64_t(product);
    regs.r[3] = std::uint32_t(value);
    regs.r[0] = std::uint32_t(value >> upper_word);
  } else {
    const auto value = product + (kind == 0 ? signed_word(regs.r[3]) : 0);
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
      regs.cpsr |= qflag;
    }
    regs.r[0] = std::uint32_t(value);
  }
}

void word_expected(armv5::Registers &regs, bool top, bool accumulate) {
  const auto value = scale_half(signed_word(regs.r[1]) * half(regs.r[2], top)) +
                     (accumulate ? signed_word(regs.r[3]) : 0);
  if (value < std::numeric_limits<std::int32_t>::min() ||
      value > std::numeric_limits<std::int32_t>::max()) {
    regs.cpsr |= qflag;
  }
  regs.r[0] = std::uint32_t(value);
}

void half_products() {
  for (const bool top_first : {false, true}) {
    for (const bool top_second : {false, true}) {
      for (const unsigned kind : {0U, 2U, 3U}) {
        constexpr std::uint32_t base = 0xe1000281;
        const auto word = base | (unsigned(top_first) << 5) | (unsigned(top_second) << 6) |
                          (kind << 21) | (kind == 3 ? 0 : 3U << 12);
        verify({.word = word, .cycles = kind == 2 ? 2U : 1U},
               [=](armv5::Registers &regs) { half_expected(regs, top_first, top_second, kind); });
      }
    }
  }
  for (const bool top : {false, true}) {
    for (const bool accumulate : {false, true}) {
      constexpr std::uint32_t base = 0xe1200281;
      const auto word = base | (unsigned(top) << 6) | (accumulate ? 3U << 12 : 0x20);
      verify({.word = word, .cycles = 1},
             [=](armv5::Registers &regs) { word_expected(regs, top, accumulate); });
    }
  }
}

void saturation_and_clz() {
  for (const bool doubled : {false, true}) {
    for (const bool subtract : {false, true}) {
      constexpr std::uint32_t base = 0xe1020051;
      const auto word = base | (unsigned(doubled) << 22) | (unsigned(subtract) << 21);
      verify({.word = word, .cycles = 1}, [=](armv5::Registers &regs) {
        bool overflow = false;
        auto second = signed_word(regs.r[2]);
        if (doubled) { second = signed_word(clamp(second * 2, overflow)); }
        regs.r[0] = clamp(signed_word(regs.r[1]) + (subtract ? -second : second), overflow);
        if (overflow) { regs.cpsr |= qflag; }
      });
    }
  }
  constexpr std::uint32_t clz = 0xe16f0f11;
  verify({.word = clz, .cycles = 1}, [](armv5::Registers &regs) {
    unsigned count = 0;
    for (auto mask = negative; mask != 0 && (regs.r[1] & mask) == 0; mask >>= 1) { ++count; }
    regs.r[0] = count;
  });
}
}

void dsp_instructions() {
  long_products();
  half_products();
  saturation_and_clz();
}
