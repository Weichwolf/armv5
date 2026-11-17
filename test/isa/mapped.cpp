// SPDX-License-Identifier: GPL-3.0-only
#include "support/mapped.hpp"

#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {
struct Form {
  std::uint32_t word;
  unsigned width;
  bool reading, writing, signed_value;
};

void access(const Form &form, bool big_endian, unsigned offset) {
  constexpr std::uint32_t entry = 0x1000;
  constexpr std::uint32_t input = 0x89abcdef;
  constexpr std::uint32_t output = 0x87654321;
  constexpr std::uint32_t untouched = 0x55555555;
  constexpr std::uint32_t flags = 0xf8000000;
  fixture::Program code(entry, big_endian);
  code.word(form.word);
  fixture::MappedMachine machine(code, 64, armv5::CompileOptions::MemoryMode::direct);
  fixture::put(std::span(machine.ram).first(4), input, big_endian);
  auto registers = machine.core->registers();
  registers.r[0] = untouched;
  registers.r[1] = fixture::AddressSpace::data_address + offset;
  registers.r[2] = output;
  registers.cpsr |= flags;
  if (big_endian) { registers.system.mapping.control |= 0x80; }
  machine.options_.big_endian = big_endian;
  machine.core->set_registers(registers);
  machine.prepare();
  auto expected = machine.core->state();
  auto expected_ram = machine.ram;
  const auto address = form.width == 4 ? 0 : offset;
  std::uint32_t loaded = 0;
  for (unsigned index = 0; index < form.width; ++index) {
    const auto shift = (big_endian ? form.width - 1 - index : index) * 8;
    loaded |= std::to_integer<std::uint32_t>(expected_ram[address + index]) << shift;
    if (form.writing) {
      expected_ram[address + index] = std::byte((output >> shift) & fixture::byte_mask);
    }
  }
  if (form.signed_value) {
    const auto bits = form.width * 8;
    if ((loaded & (1U << (bits - 1))) != 0) { loaded |= ~std::uint32_t{0} << bits; }
  }
  if (form.width == 4) { loaded = std::rotr(loaded, int(offset * 8)); }
  if (form.reading) { expected.registers.r[0] = loaded; }
  expected.registers.r[15] = entry + 4;
  expected.progress = {.instructions = 1, .base_cycles = form.reading && form.writing ? 2U : 1U};
  const auto result = machine.run(1);
  fixture::require(
      machine.core->state() == expected && result.progress == expected.progress &&
          machine.ram == expected_ram,
      "Direct byte/half/word/sign/SWP endian and legacy rotation preserve complete output");
  machine.verify_storage();
}

void blocks(bool big_endian, bool thumb) {
  constexpr std::uint32_t entry = 0x1000;
  constexpr std::uint32_t input = 0x12345678;
  constexpr std::uint32_t output = 0xabcdef01;
  constexpr std::uint32_t store_pair = 0xe8a10005;
  constexpr std::uint32_t load_pair = 0xe9310018;
  constexpr std::uint32_t store_double = 0xe1c500f0;
  constexpr std::uint32_t load_double = 0xe1c560d0;
  constexpr std::uint16_t thumb_store = 0xc105;
  constexpr std::uint16_t thumb_load = 0xcd18;
  constexpr std::uint32_t thumb_bit = 0x20;
  constexpr unsigned double_offset = 16;
  fixture::Program code(entry, big_endian);
  if (thumb) {
    code.half(thumb_store);
    code.half(thumb_load);
  } else {
    for (const auto word : {store_pair, load_pair, store_double, load_double}) { code.word(word); }
  }
  fixture::MappedMachine machine(code, 64, armv5::CompileOptions::MemoryMode::direct);
  auto expected = machine.core->state();
  auto &registers = expected.registers;
  registers.r[0] = input;
  registers.r[1] = fixture::AddressSpace::data_address;
  registers.r[2] = output;
  registers.r[5] = registers.r[1] + (thumb ? 0 : double_offset);
  if (big_endian) { registers.system.mapping.control |= 0x80; }
  if (thumb) {
    registers.cpsr |= thumb_bit;
    machine.options_.instruction_set = armv5::CompileOptions::InstructionSet::thumb;
    machine.spans_.front().instruction_set = armv5::CodeSpan::InstructionSet::thumb;
  }
  machine.options_.big_endian = big_endian;
  machine.core->set_registers(registers);
  machine.prepare();
  auto expected_ram = machine.ram;
  fixture::put(std::span(expected_ram).first(4), input, big_endian);
  fixture::put(std::span(expected_ram).subspan(4, 4), output, big_endian);
  if (thumb) {
    registers.r[1] += 8;
    registers.r[5] += 8;
  } else {
    fixture::put(std::span(expected_ram).subspan(double_offset, 4), input, big_endian);
    fixture::put(std::span(expected_ram).subspan(double_offset + 4, 4), registers.r[1], big_endian);
    registers.r[6] = input;
    registers.r[7] = registers.r[1];
  }
  registers.r[3] = input;
  registers.r[4] = output;
  registers.r[15] = code.pc();
  expected.progress = {.instructions = thumb ? 2U : 4U, .base_cycles = thumb ? 4U : 8U};
  (void)machine.run(expected.progress.instructions);
  fixture::require(machine.core->state() == expected && machine.ram == expected_ram,
                   "Direct ARM/Thumb block and doubleword transfers preserve full state/RAM");
  machine.verify_storage();
}
} // namespace

void mapped_accesses() {
  constexpr std::uint32_t load_half = 0xe1d100b0;
  constexpr std::uint32_t store_half_r2 = 0xe1c120b0;
  constexpr std::uint32_t load_signed_byte = 0xe1d100d0;
  constexpr std::uint32_t load_signed_half = 0xe1d100f0;
  constexpr std::uint32_t swap_r0_r2 = 0xe1010092;
  constexpr std::uint32_t swap_byte_r0_r2 = 0xe1410092;
  const std::array forms{
      Form{.word = fixture::transfer(true, false, 0, 1, 0),
           .width = 4,
           .reading = true,
           .writing = false,
           .signed_value = false},
      Form{.word = fixture::transfer(false, false, 2, 1, 0),
           .width = 4,
           .reading = false,
           .writing = true,
           .signed_value = false},
      Form{.word = fixture::transfer(true, true, 0, 1, 0),
           .width = 1,
           .reading = true,
           .writing = false,
           .signed_value = false},
      Form{.word = fixture::transfer(false, true, 2, 1, 0),
           .width = 1,
           .reading = false,
           .writing = true,
           .signed_value = false},
      Form{.word = load_half, .width = 2, .reading = true, .writing = false, .signed_value = false},
      Form{.word = store_half_r2,
           .width = 2,
           .reading = false,
           .writing = true,
           .signed_value = false},
      Form{.word = load_signed_byte,
           .width = 1,
           .reading = true,
           .writing = false,
           .signed_value = true},
      Form{.word = load_signed_half,
           .width = 2,
           .reading = true,
           .writing = false,
           .signed_value = true},
      Form{.word = swap_r0_r2, .width = 4, .reading = true, .writing = true, .signed_value = false},
      Form{.word = swap_byte_r0_r2,
           .width = 1,
           .reading = true,
           .writing = true,
           .signed_value = false}};
  for (const auto &form : forms) {
    for (const bool big_endian : {false, true}) {
      for (unsigned offset = 0; offset < 4; offset += form.width == 2 ? 2 : 1) {
        access(form, big_endian, offset);
      }
    }
  }
  for (const bool big_endian : {false, true}) {
    for (const bool thumb : {false, true}) { blocks(big_endian, thumb); }
  }
}
