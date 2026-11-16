// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
constexpr std::uint32_t n_flag = 0x80000000, z_flag = 0x40000000, c_flag = 0x20000000,
                        v_flag = 0x10000000, q_flag = 0x08000000, reset_status = 0xd3;

struct Arithmetic {
  std::uint32_t result, flags;
};

Arithmetic pack_expected(std::uint32_t value, bool carry, bool overflow) {
  return {.result = value,
          .flags = (value & n_flag) | (value == 0 ? z_flag : 0) | (carry ? c_flag : 0) |
                   (overflow ? v_flag : 0) | q_flag | reset_status};
}

Arithmetic
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

Arithmetic expected(unsigned opcode, std::pair<std::uint32_t, std::uint32_t> operands, bool carry) {
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

void arithmetic() {
  constexpr std::array<std::uint32_t, 7> values{
      0, 1, 2, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff};
  for (unsigned opcode = 0; opcode < 16; ++opcode) {
    const bool test = opcode >= 8 && opcode <= 11;
    fixture::Program program;
    program.word(fixture::data(opcode, true, test ? 0 : 2, 0, false, 1));
    program.word(fixture::branch(program.pc(), {.address = program.pc()}));
    fixture::Machine machine(program);
    machine.prepare();
    for (const auto a : values) {
      for (const auto b : values) {
        for (const bool carry : {false, true}) {
          machine.core->reset(program.address());
          auto registers = machine.core->registers();
          registers.r[0] = a;
          registers.r[1] = b;
          registers.cpsr |= q_flag | v_flag | (carry ? c_flag : 0);
          machine.registers(registers);
          const auto retired = machine.run(1);
          const auto answer = expected(opcode, {a, b}, carry);
          if (!test) { registers.r[2] = answer.result; }
          registers.cpsr = answer.flags;
          registers.r[15] += 4;
          fixture::require(machine.core->registers() == registers,
                           "ARM ALU result/flags/untouched register mismatch, opcode " +
                               std::to_string(opcode));
          fixture::require(retired.progress.instructions == 1 && retired.progress.base_cycles == 1,
                           "ARM ALU progress");
        }
      }
    }
  }
}

void shifts() {
  struct Case {
    unsigned count;
    std::array<std::uint32_t, 4> results;
    std::array<bool, 4> carry;
  };

  constexpr std::array cases{
      Case{.count = 0,
           .results = {0x80000001, 0x80000001, 0x80000001, 0x80000001},
           .carry = {false, false, false, false}},
      Case{.count = 1,
           .results = {2, 0x40000000, 0xc0000000, 0xc0000000},
           .carry = {true, true, true, true}},
      Case{.count = 31,
           .results = {0x80000000, 1, 0xffffffff, 3},
           .carry = {false, false, false, false}},
      Case{.count = 32,
           .results = {0, 0, 0xffffffff, 0x80000001},
           .carry = {true, true, true, true}},
      Case{.count = 33,
           .results = {0, 0, 0xffffffff, 0xc0000000},
           .carry = {false, false, true, true}},
      Case{.count = 255, .results = {0, 0, 0xffffffff, 3}, .carry = {false, false, true, false}},
      Case{.count = 256,
           .results = {0x80000001, 0x80000001, 0x80000001, 0x80000001},
           .carry = {false, false, false, false}}};
  constexpr std::uint32_t input = 0x80000001;
  for (unsigned type = 0; type < 4; ++type) {
    fixture::Program program;
    program.word(fixture::data(13, true, 2, 0, false, (1U << 8) | (type << 5) | 16U));
    program.word(fixture::branch(program.pc(), {.address = program.pc()}));
    fixture::Machine machine(program);
    machine.prepare();
    for (const auto &row : cases) {
      for (const bool incoming : {false, true}) {
        machine.core->reset(program.address());
        auto registers = machine.core->registers();
        registers.r[0] = input;
        registers.r[1] = row.count;
        registers.cpsr |= v_flag | q_flag | (incoming ? c_flag : 0);
        machine.registers(registers);
        const auto retired = machine.run(1);
        const auto actual = machine.core->registers();
        const bool carry = (row.count & fixture::byte_mask) == 0 ? incoming : row.carry[type];
        const auto status = reset_status | q_flag | v_flag | (row.results[type] & n_flag) |
                            (row.results[type] == 0 ? z_flag : 0) | (carry ? c_flag : 0);
        fixture::require(actual.r[2] == row.results[type] && actual.cpsr == status,
                         "register shift result/carry boundary");
        fixture::require(retired.progress.base_cycles == 2, "register-shift base cost");
      }
    }
  }
  fixture::Program program;
  constexpr unsigned rrx = 0x60;
  constexpr unsigned rotated_immediate = 0x102;
  program.word(fixture::data(13, true, 2, 0, false, rrx)); // RRX r2,r0
  program.word(
      fixture::data(13, true, 3, 0, true, rotated_immediate)); // rotated immediate 0x80000000
  program.word(fixture::branch(program.pc(), {.address = program.pc()}));
  fixture::Machine machine(program);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[0] = 2;
  registers.cpsr |= c_flag;
  machine.registers(registers);
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[2] == (n_flag | 1) &&
                       (machine.core->registers().cpsr & c_flag) == 0,
                   "RRX uses old carry");
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[3] == n_flag &&
                       (machine.core->registers().cpsr & c_flag) != 0,
                   "immediate rotation carry");
}

void conditions() {
  constexpr std::array<unsigned, 16> truth{0x56aa,
                                           0x6a6a,
                                           0x55a6,
                                           0x6966,
                                           0x66a9,
                                           0x6a69,
                                           0x66a5,
                                           0x6a65,
                                           0x6a9a,
                                           0x565a,
                                           0x6996,
                                           0x5556,
                                           0x6a99,
                                           0x6659,
                                           0x6a95,
                                           0x6655};
  fixture::Program program;
  for (unsigned condition = 0; condition < 15; ++condition) {
    program.word(fixture::data(13, false, 0, 0, true, 1, condition));
    program.word(fixture::branch(program.pc(), {.address = program.pc()}));
  }
  fixture::Machine machine(program);
  machine.entries.clear();
  for (unsigned condition = 0; condition < 15; ++condition) {
    machine.entries.push_back(program.address() + (condition * 8));
  }
  machine.prepare();
  for (unsigned flags = 0; flags < truth.size(); ++flags) {
    for (unsigned condition = 0; condition < 15; ++condition) {
      machine.core->reset();
      auto registers = machine.core->registers();
      registers.cpsr |= flags << fixture::condition_shift;
      registers.r[15] = machine.entries[condition];
      machine.registers(registers);
      (void)machine.run(1);
      registers.r[0] = (truth[flags] >> condition) & 1;
      registers.r[15] += 4;
      fixture::require(machine.core->registers() == registers,
                       "condition truth table or failed-predicate effect");
    }
  }
}

void single_transfers() {
  constexpr std::uint32_t big_byte = 0xc3;
  constexpr std::uint32_t little_byte = 0xb2;
  constexpr std::uint32_t input = 0xa1b2c3d4;
  constexpr std::uint32_t rotated = 0xd4a1b2c3;
  for (const bool big : {false, true}) {
    for (const bool fixed : {false, true}) {
      fixture::Program program(0x1000, big);
      program.word(fixture::transfer(true, false, 2, 0, 1));
      program.word(fixture::transfer(false, false, 2, 0, 5));
      program.word(fixture::transfer(true, true, 3, 0, 2));
      program.word(fixture::transfer(false, true, 3, 0, 8));
      program.word(fixture::transfer(true, false, 4, 0, 4, false));
      program.word(fixture::transfer(false, false, 4, 0, 4, true, false, true));
      program.word(fixture::branch(program.pc(), {.address = program.pc()}));
      fixture::Machine machine(program);
      fixture::put(std::span(machine.ram).first(4), input, big);
      auto expected_ram = machine.ram;
      fixture::put(std::span(expected_ram).subspan(4, 4), rotated, big);
      expected_ram[8] = big ? std::byte{big_byte} : std::byte{little_byte};
      machine.prepare({.big_endian = big, .callbacks = {}}, fixed);
      auto registers = machine.core->registers();
      registers.r[0] = 0x4000;
      machine.registers(registers);
      const auto result = machine.run(6);
      fixture::require(result.progress.instructions == 6 && result.progress.base_cycles == 6,
                       "single transfer progress");
      const auto actual = machine.core->registers();
      fixture::require(actual.r[0] == 0x4000 && actual.r[2] == rotated &&
                           actual.r[3] == (big ? big_byte : little_byte) && actual.r[4] == input,
                       "legacy alignment/endian/writeback");
      fixture::require(machine.ram == expected_ram, "complete single-transfer memory output");
    }
  }
}

void extra_and_multiple() {
  constexpr std::uint32_t stack_pointer = 0x4080;
  constexpr unsigned tail_offset = 20;
  constexpr std::byte sign_prefix{0xff};
  constexpr std::byte sign_suffix{0xfe};
  constexpr std::array extra{0xe1d020f0U, 0xe1d030d2U, 0xe1c020b4U, 0xe1c040d8U, 0xe1c041f0U};
  for (const bool big : {false, true}) {
    fixture::Program program(0x1000, big);
    for (const auto word : extra) { program.word(word); }
    program.word(fixture::branch(program.pc(), {.address = program.pc()}));
    fixture::Machine machine(program);
    machine.ram[0] = std::byte{0x80};
    machine.ram[1] = sign_prefix;
    machine.ram[2] = std::byte{1};
    machine.ram[3] = sign_suffix;
    constexpr std::uint32_t first = 0x12345678;
    constexpr std::uint32_t second = 0xabcdef01;
    fixture::put(std::span(machine.ram).subspan(8, 4), first, big);
    fixture::put(std::span(machine.ram).subspan(12, 4), second, big);
    auto expected_ram = machine.ram;
    expected_ram[4] = expected_ram[0];
    expected_ram[5] = expected_ram[1];
    fixture::put(std::span(expected_ram).subspan(16, 4), first, big);
    fixture::put(std::span(expected_ram).subspan(tail_offset, 4), second, big);
    machine.prepare({.big_endian = big, .callbacks = {}});
    auto registers = machine.core->registers();
    registers.r[0] = 0x4000;
    machine.registers(registers);
    (void)machine.run(5);
    constexpr std::uint32_t big_signed = 0xffff80ff;
    constexpr std::uint32_t little_signed = 0xffffff80;
    const auto actual = machine.core->registers();
    fixture::require(actual.r[2] == (big ? big_signed : little_signed) && actual.r[3] == 1 &&
                         actual.r[4] == first && actual.r[5] == second,
                     "extra transfer signed/pair results");
    fixture::require(machine.ram == expected_ram, "complete extra-transfer output");
  }
  fixture::Program program;
  constexpr std::uint32_t push = 0xe92d400f;
  constexpr std::uint32_t pop = 0xe8bd40f0;
  program.word(push);
  program.word(pop);
  program.word(fixture::branch(program.pc(), {.address = program.pc()}));
  fixture::Machine machine(program);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[0] = 1;
  registers.r[1] = 2;
  registers.r[2] = 3;
  registers.r[3] = 4;
  registers.r[14] = 8;
  registers.r[13] = stack_pointer;
  machine.registers(registers);
  (void)machine.run(2);
  for (unsigned index = 0; index < 4; ++index) { registers.r[index + 4] = registers.r[index]; }
  registers.r[15] += 8;
  fixture::require(machine.core->registers() == registers, "STMDB/LDMIA stack and writeback");
}

void status_and_predicated_memory() {
  constexpr std::array words{0xe128f000U, 0xe10f1000U, 0xe328f20fU, 0xe10f2000U};
  fixture::Program program;
  for (const auto word : words) { program.word(word); }
  program.word(fixture::branch(program.pc(), {.address = program.pc()}));
  fixture::Machine machine(program);
  machine.prepare();
  auto registers = machine.core->registers();
  constexpr std::uint32_t input = 0x980000ff;
  constexpr std::uint32_t first = 0x980000d3;
  constexpr std::uint32_t second = 0xf00000d3;
  registers.r[0] = input;
  machine.registers(registers);
  const auto result = machine.run(4);
  fixture::require(machine.core->registers().r[1] == first &&
                       machine.core->registers().r[2] == second &&
                       machine.core->registers().cpsr == second && result.progress.base_cycles == 6,
                   "CPSR flag MSR/MRS and preserved control bits");
  fixture::Program conditional;
  constexpr std::uint32_t condition_mask = 0x0fffffff;
  conditional.word(fixture::transfer(false, false, 1, 0, 0) & condition_mask);
  conditional.word(fixture::transfer(true, false, 2, 0, 0) & condition_mask);
  conditional.word(fixture::branch(conditional.pc(), {.address = conditional.pc()}));
  fixture::Machine skipped(conditional);
  skipped.prepare();
  registers = skipped.core->registers();
  constexpr std::uint32_t unmapped = 0xf0000000;
  registers.r[0] = unmapped;
  registers.r[2] = 7;
  skipped.registers(registers);
  (void)skipped.run(2);
  fixture::require(skipped.core->registers().r[2] == 7 &&
                       skipped.core->registers().r[0] == unmapped,
                   "failed memory predicate performed an access/writeback");
}

void swaps() {
  constexpr std::uint32_t word_swap = 0xe1002091;
  constexpr std::uint32_t byte_swap = 0xe1402091;
  constexpr std::uint32_t old = 0xa1b2c3d4;
  constexpr std::uint32_t replacement = 0x12345678;
  constexpr std::uint32_t rotated = 0xd4a1b2c3;
  constexpr std::byte replacement_byte{0x78};
  for (const bool big : {false, true}) {
    for (const bool byte : {false, true}) {
      fixture::Program program(0x1000, big);
      program.word(byte ? byte_swap : word_swap);
      program.word(fixture::branch(program.pc(), {.address = program.pc()}));
      fixture::Machine machine(program);
      fixture::put(std::span(machine.ram).first(4), old, big);
      auto expected_ram = machine.ram;
      const auto old_byte = std::to_integer<std::uint32_t>(expected_ram[1]);
      if (byte) {
        expected_ram[1] = replacement_byte;
      } else {
        fixture::put(std::span(expected_ram).first(4), replacement, big);
      }
      machine.prepare({.big_endian = big, .callbacks = {}});
      auto registers = machine.core->registers();
      registers.r[0] = 0x4000 + 1;
      registers.r[1] = replacement;
      machine.registers(registers);
      (void)machine.run(1);
      fixture::require(machine.core->registers().r[2] == (byte ? old_byte : rotated) &&
                           machine.ram == expected_ram,
                       "atomic swap width/endian/legacy alignment");
    }
  }
}

void preparation_refusals() {
  struct Case {
    std::uint32_t word;
    std::string_view reason;
  };

  constexpr std::array cases{
      Case{.word = 0xe6bf0f31, .reason = "undefined media"},
      Case{.word = 0xe1900f9f, .reason = "swap/exclusive"},
      Case{.word = 0xe0400291, .reason = "undefined multiply"},
      Case{.word = 0xf1080080, .reason = "undefined unconditional"},
      Case{.word = 0xfc410f02, .reason = "undefined coprocessor extension"},
      Case{.word = 0xe10f0100, .reason = "control/DSP encoding"},
      Case{.word = 0xe0810293, .reason = "UMULL: instruction is not lowered"},
      Case{.word = 0xe18200d0, .reason = "doubleword index/destination overlap"},
      Case{.word = 0xe1e000f8, .reason = "writeback overlap/PC"},
      Case{.word = 0xee100f10, .reason = "MRC: instruction is not lowered"}};
  for (const auto &[word, reason] : cases) {
    fixture::Program program;
    program.word(word);
    fixture::Machine machine(program);
    bool refused = false;
    try {
      (void)armv5::Core::lower(machine.image());
    } catch (const std::runtime_error &error) {
      fixture::contains(error.what(), "authored-arm-fixture at 0x00001000");
      fixture::contains(error.what(), reason);
      refused = true;
    }
    fixture::require(refused, "unsupported/invalid preparation accepted");
  }
}

}

int main() {
  try {
    arithmetic();
    shifts();
    conditions();
    single_transfers();
    extra_and_multiple();
    status_and_predicated_memory();
    swaps();
    preparation_refusals();
    processor_modes();
    std::cout << "Installed ARM arithmetic, predication, shifts and transfers passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
