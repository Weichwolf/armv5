// SPDX-License-Identifier: GPL-3.0-only
#include "support/mapped.hpp"

#include "armv5/core.hpp"
#include "consumer.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

void mapped_memory() {
  constexpr std::uint32_t entry = 0x1000;
  constexpr std::uint32_t vector = 0x10;
  constexpr std::uint32_t link = entry + 8;
  constexpr std::uint32_t mode_mask = 31;
  constexpr std::uint32_t first = 0x12345678;
  constexpr std::uint32_t last = 0x89abcdef;
  constexpr std::uint32_t untouched = 0x55;
  fixture::Program code(entry);
  fixture::Program handler(vector);
  code.word(fixture::transfer(true, false, 0, 1, 0));
  code.word(fixture::transfer(false, false, 2, 1, 0));
  handler.word(fixture::data(13, false, 0, 0, false, 0));
  fixture::MappedMachine machine({{.program = code, .modes = {fixture::supervisor_mode}},
                                  {.program = handler, .modes = {fixture::abort_mode}}},
                                 {vector},
                                 true,
                                 4096);
  fixture::put(machine.space_.data().first(4), first);
  fixture::put(machine.space_.data().last(4), last);
  constexpr std::uint32_t first_address = fixture::AddressSpace::data_address;
  constexpr std::uint32_t last_address = 0xfffffffc;
  for (const auto address : {0U, first_address - 4, first_address, last_address}) {
    machine.core->reset(entry);
    auto initial = machine.core->registers();
    initial.r[0] = untouched;
    initial.r[1] = address;
    machine.core->set_registers(initial);
    auto expected = machine.core->state();
    const bool present = address >= first_address;
    expected.progress = {.instructions = present ? 1U : 0U, .base_cycles = present ? 1U : 3U};
    if (present) {
      expected.registers.r[0] = address == first_address ? first : last;
      expected.registers.r[15] = entry + 4;
    } else {
      expected.registers.cpsr = (initial.cpsr & ~mode_mask) | fixture::abort_mode;
      expected.registers.spsr = initial.cpsr;
      expected.registers.r[14] = link;
      expected.registers.r[15] = vector;
      expected.registers.system.data_fault_status = 8;
      expected.registers.system.fault_address = address;
      expected.banks.abort[1] = link;
      expected.banks.spsr[3] = initial.cpsr;
    }
    const auto result = machine.core->run_until(1);
    fixture::require(
        machine.core->state() == expected && result.progress == expected.progress,
        "full 32-bit reservation preserves exact low holes, first and final RAM words");
  }
  fixture::put(machine.space_.data().last(4), 0);
  machine.verify_storage();
  machine.core->reset(entry);
  auto registers = machine.core->registers();
  registers.r[1] = last_address;
  registers.r[2] = 1;
  machine.core->set_registers(registers);
  (void)machine.run(2);
  bool detected = false;
  try {
    machine.verify_storage();
  } catch (const std::runtime_error &error) {
    detected =
        std::string_view(error.what()) == "benchmark storage beyond working buffer remains zero";
  }
  fixture::require(detected, "full-space output oracle detects a native write beyond working RAM");
  fixture::put(machine.space_.data().last(4), 0);
  machine.verify_storage();
}
