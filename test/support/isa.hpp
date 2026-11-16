// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "armv5/core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fixture {

inline constexpr unsigned condition_shift = 28, immediate_bit = 25, opcode_shift = 21,
                          flags_bit = 20, transfer_bit = 26, preindex_bit = 24, up_bit = 23,
                          byte_bit = 22;
inline constexpr unsigned byte_mask = 255;

inline void contains(std::string_view actual, std::string_view expected) {
  if (actual.find(expected) != std::string_view::npos) { return; }
  std::string message(expected);
  message.append(" / got: ").append(actual);
  throw std::runtime_error(message);
}

inline void require(bool condition, std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

// Independently authored encoding fields from DDI 0100I A3/A5. No production
// decoder or semantic helper is included. Inputs describe synthetic programs.
inline std::uint32_t data(unsigned opcode,
                          bool flags,
                          unsigned destination,
                          unsigned source,
                          bool immediate,
                          unsigned operand) {
  return (14U << condition_shift) | (unsigned(immediate) << immediate_bit) |
         (opcode << opcode_shift) | (unsigned(flags) << flags_bit) | (source << 16) |
         (destination << 12) | operand;
}

inline std::uint32_t transfer(bool load, bool byte, unsigned destination, unsigned base) {
  return (14U << condition_shift) | (1U << transfer_bit) | (1U << preindex_bit) | (1U << up_bit) |
         (unsigned(byte) << byte_bit) | (unsigned(load) << flags_bit) | (base << 16) |
         (destination << 12) | 4U;
}

class Program {
public:
  explicit Program(std::uint32_t address = 0x1000) : address_(address) {}

  void word(std::uint32_t instruction, bool big_endian = false) {
    for (unsigned index = 0; index < 4; ++index) {
      bytes_.push_back(
          std::byte((instruction >> ((big_endian ? 3 - index : index) * 8)) & byte_mask));
    }
  }

  // A1 tests the public preparation diagnostic. A2 replaces this with installed
  // image execution for semantic expectations; no private decoder access exists.
  [[nodiscard]] std::string diagnostic(bool big_endian = false) const {
    const std::array spans{armv5::CodeSpan{.address = address_, .bytes = bytes_}};
    const std::array entries{address_};
    const std::array variants{armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries}};
    const armv5::CodeImage image{.identity = "authored-arm-fixture",
                                 .variants = variants,
                                 .layouts = {},
                                 .transitions = {},
                                 .interrupt_vectors = {}};
    try {
      (void)armv5::Core::lower(image, {.big_endian = big_endian, .callbacks = {}});
    } catch (const std::runtime_error &error) {
      const std::string message = error.what();
      require(message.starts_with("authored-arm-fixture at 0x"), "missing guest fault context");
      return message;
    }
    throw std::runtime_error("A1 must refuse executable preparation");
  }

private:
  std::uint32_t address_;
  std::vector<std::byte> bytes_;
};

inline void decoded(std::uint32_t word, std::string_view operation) {
  for (const bool big_endian : {false, true}) {
    Program program;
    program.word(word, big_endian);
    std::string expected("decoded ARM ");
    expected.append(operation).append("; LLVM lowering is not implemented");
    const auto actual = program.diagnostic(big_endian);
    contains(actual, expected);
  }
}

inline void rejected(std::uint32_t word, std::string_view reason) {
  Program program;
  program.word(word);
  const auto actual = program.diagnostic();
  contains(actual, reason);
  require(actual.find("decoded ARM") == std::string::npos, "invalid instruction reached lowering");
}

} // namespace fixture
