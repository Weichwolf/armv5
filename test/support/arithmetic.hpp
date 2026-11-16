// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <bit>
#include <cstdint>
#include <limits>
#include <utility>

namespace fixture {
inline constexpr std::uint32_t n_flag = 0x80000000, z_flag = 0x40000000, c_flag = 0x20000000,
                               v_flag = 0x10000000, q_flag = 0x08000000, reset_status = 0xd3;

struct Arithmetic {
  std::uint32_t result, flags;
};

inline Arithmetic pack_expected(std::uint32_t value, bool carry, bool overflow) {
  return {.result = value,
          .flags = (value & n_flag) | (value == 0 ? z_flag : 0) | (carry ? c_flag : 0) |
                   (overflow ? v_flag : 0) | q_flag | reset_status};
}

inline Arithmetic
expected_arithmetic(unsigned opcode, std::pair<std::uint32_t, std::uint32_t> operands, bool carry) {
  const auto [a, b] = operands;
  const auto signed_a = std::int64_t(std::bit_cast<std::int32_t>(a));
  const auto signed_b = std::int64_t(std::bit_cast<std::int32_t>(b));
  std::int64_t result = 0;
  bool carry_out = false;
  if (opcode == 4 || opcode == 5 || opcode == 11) {
    const unsigned extra = opcode == 5 ? unsigned(carry) : 0;
    result = signed_a + signed_b + extra;
    carry_out = std::uint64_t(a) + b + extra > std::numeric_limits<std::uint32_t>::max();
  } else {
    const bool reversed = opcode == 3 || opcode == 7;
    const unsigned borrow = (opcode == 6 || opcode == 7) && !carry ? 1 : 0;
    result = reversed ? signed_b - signed_a - borrow : signed_a - signed_b - borrow;
    carry_out = reversed ? std::uint64_t(b) >= std::uint64_t(a) + borrow
                         : std::uint64_t(a) >= std::uint64_t(b) + borrow;
  }
  return pack_expected(static_cast<std::uint32_t>(result),
                       carry_out,
                       result < std::numeric_limits<std::int32_t>::min() ||
                           result > std::numeric_limits<std::int32_t>::max());
}

inline Arithmetic
expected(unsigned opcode, std::pair<std::uint32_t, std::uint32_t> operands, bool carry) {
  const auto [a, b] = operands;
  if ((opcode >= 2 && opcode <= 7) || opcode == 10 || opcode == 11) {
    return expected_arithmetic(opcode, operands, carry);
  }
  std::uint32_t value = 0;
  switch (opcode) {
    case 0:
    case 8: value = a & b; break;
    case 1:
    case 9: value = a ^ b; break;
    case 12: value = a | b; break;
    case 13: value = b; break;
    case 14: value = a & ~b; break;
    default: value = ~b; break;
  }
  return pack_expected(value, carry, true);
}

} // namespace fixture
