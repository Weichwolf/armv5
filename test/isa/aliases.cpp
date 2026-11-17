// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/mapped.hpp"
#include "support/modes.hpp"

#include <cstdint>
#include <span>

namespace {
constexpr std::uint32_t entry = 0x1000;
constexpr std::uint32_t table = 0x4000;
constexpr std::uint32_t coarse = 0x8000;
constexpr std::uint32_t first = 0x100000;
constexpr std::uint32_t second = 0x300000;
constexpr std::uint32_t backing = 0x200000;
constexpr std::uint32_t initial = 0x12345678;
constexpr std::uint32_t replacement = 0x55667788;
constexpr std::uint32_t section_ap3 = 0xc02;
constexpr std::uint32_t page_ap3 = 0xff2;
constexpr std::uint32_t top_address = 0xfffffffc;
constexpr std::uint32_t working_size = 0x300000;
constexpr std::uint32_t wrapped_address = 0xffc;

void prepare(fixture::MappedMachine &machine, bool wrapping) {
  auto put = [&](std::uint32_t address, std::uint32_t value) {
    fixture::put(std::span(machine.ram).subspan(address - table, 4), value);
  };
  if (wrapping) {
    constexpr std::uint32_t final_page = 0x2ff000;
    constexpr std::uint32_t final_section_index = 4095;
    put(table, coarse | 1);
    put(coarse, final_page | page_ap3);
    put(coarse + 4, entry | page_ap3);
    put(table + (final_section_index * 4), backing | section_ap3);
  } else {
    constexpr unsigned section_shift = 20;
    put(table, section_ap3);
    put(table + ((first >> section_shift) * 4), backing | section_ap3);
    put(table + ((second >> section_shift) * 4), backing | section_ap3);
  }
  auto registers = machine.core->registers();
  registers.r[0] = replacement;
  registers.r[1] = wrapping ? top_address : 0;
  registers.system.mapping.control |= 1;
  registers.system.mapping.translation_table = table;
  registers.system.mapping.domains = 3;
  machine.mapping_ = registers.system.mapping;
  machine.core->set_registers(registers);
  machine.prepare();
}

void store_then_reload(bool wrapping) {
  fixture::Program code(entry);
  if (wrapping) {
    constexpr unsigned immediate_4096 = 0xa01;
    code.word(fixture::data(13, false, 2, 0, true, immediate_4096));
    code.word(fixture::data(4, false, 2, 1, false, 2));
  } else {
    constexpr unsigned immediate_first = 0x601;
    constexpr unsigned immediate_second = 0x603;
    code.word(fixture::data(13, false, 1, 0, true, immediate_first));
    code.word(fixture::data(13, false, 2, 0, true, immediate_second));
  }
  code.word(fixture::transfer(true, false, 3, 2, 0));
  code.word(fixture::transfer(false, false, 0, 1, 0));
  code.word(fixture::transfer(true, false, 4, 2, 0));
  fixture::MappedMachine machine(code, working_size, armv5::CompileOptions::MemoryMode::direct);
  const auto physical = wrapping ? 0x2ffffcU : backing;
  const auto offset = physical - table;
  fixture::put(std::span(machine.ram).subspan(offset, 4), initial);
  prepare(machine, wrapping);
  auto expected = machine.core->state();
  auto expected_ram = machine.ram;
  fixture::put(std::span(expected_ram).subspan(offset, 4), replacement);
  expected.registers.r[1] = wrapping ? top_address : first;
  expected.registers.r[2] = wrapping ? wrapped_address : second;
  expected.registers.r[3] = initial;
  expected.registers.r[4] = replacement;
  expected.registers.r[15] = code.pc();
  expected.progress = {.instructions = 5, .base_cycles = 5};
  (void)machine.run(5);
  fixture::require(machine.core->state() == expected && machine.ram == expected_ram,
                   "optimized Direct store/reload preserves real aliases and variable 32-bit wrap");
  machine.verify_storage();
}

void shifted_copy() {
  constexpr unsigned immediate_first = 0x601;
  constexpr unsigned immediate_second = 0x603;
  constexpr unsigned immediate_256 = 0xc01;
  constexpr unsigned words = 256;
  fixture::Program code(entry);
  code.word(fixture::data(13, false, 1, 0, true, immediate_first));
  code.word(fixture::data(13, false, 2, 0, true, immediate_second));
  code.word(fixture::data(4, false, 2, 2, true, 4));
  code.word(fixture::data(13, false, 3, 0, true, immediate_256));
  const auto loop = code.pc();
  code.word(fixture::transfer(true, false, 4, 1, 4, false));
  code.word(fixture::transfer(false, false, 4, 2, 4, false));
  code.word(fixture::data(2, true, 3, 3, true, 1));
  code.word(fixture::branch(code.pc(), {.address = loop}, 1));
  fixture::MappedMachine machine(code, working_size, armv5::CompileOptions::MemoryMode::direct);
  const auto offset = backing - table;
  for (unsigned index = 0; index <= words; ++index) {
    fixture::put(std::span(machine.ram).subspan(offset + (index * 4), 4), initial + index);
  }
  prepare(machine, false);
  auto expected = machine.core->state();
  auto expected_ram = machine.ram;
  for (unsigned index = 0; index <= words; ++index) {
    fixture::put(std::span(expected_ram).subspan(offset + (index * 4), 4), initial);
  }
  expected.registers.r[1] = first + (words * 4);
  expected.registers.r[2] = second + ((words + 1) * 4);
  expected.registers.r[4] = initial;
  expected.registers.r[15] = code.pc();
  constexpr std::uint32_t completed_status = 0x600000d3;
  expected.registers.cpsr = completed_status;
  expected.progress = {.instructions = 4 + (4 * words), .base_cycles = 4 + (6 * words) - 2};
  (void)machine.run(expected.progress.instructions);
  fixture::require(
      machine.core->state() == expected && machine.ram == expected_ram,
      "optimized shifted copy through distinct virtual aliases preserves forward dependence");
  machine.verify_storage();
}
} // namespace

void mapped_aliases() {
  store_then_reload(false);
  store_then_reload(true);
  shifted_copy();
}
