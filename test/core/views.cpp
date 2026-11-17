// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "consumer.hpp"
#include "support/faults.hpp"
#include "support/isa.hpp"
#include "support/mapped.hpp"
#include "support/modes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#if defined(__linux__) && defined(__x86_64__)
namespace {
constexpr std::uint32_t entry = 0x1000;
constexpr std::uint32_t abort_vector = 0x10;
constexpr std::uint32_t table = 0x4000;
constexpr std::uint32_t second_table = 0x8000;
constexpr std::uint32_t virtual_data = 0x108000;
constexpr std::uint32_t physical_data = 0x208000;
constexpr std::uint32_t input = 0x12345678;
constexpr std::uint32_t output = 0x87654321;
constexpr unsigned section_shift = 20;
constexpr std::uint32_t repeated_ap = 0x55;
constexpr std::uint32_t section_offset = (1U << section_shift) - 1;
constexpr std::uint32_t section_mask = ~section_offset;
constexpr std::uint32_t mode_mask = 31;
constexpr std::size_t working_size = 0x220000;
enum class Page : std::uint8_t {
  section,
  coarse_large,
  coarse_small,
  fine_large,
  fine_small,
  tiny
};

struct Case {
  Page page;
  std::uint32_t domain, permission;
  bool user, writing, big_endian;
  std::uint32_t process_id = 0, protection = 0;
  bool high_alias = false, forced_user = false, mixed = false;
  bool lockdown = false, enable_lockdown = false;
  unsigned quarter = 0;
};

constexpr std::array<std::uint32_t, 4> subpage_permissions{0, 3, 1, 2};

std::uint32_t guest_address(const Case &test) {
  return virtual_data + (test.quarter * 1024) + (test.high_alias ? test.process_id : 0);
}

void tables(fixture::MappedMachine &machine, const Case &test) {
  auto put = [&](std::uint32_t address, std::uint32_t value) {
    fixture::put(std::span(machine.ram).subspan(address - table, 4), value, test.big_endian);
  };
  // The code has its own Manager domain; data independently tests domain zero.
  put(table + ((test.process_id >> section_shift) * 4), (1U << 5) | 2);
  const auto first = table + (((test.process_id | virtual_data) >> section_shift) * 4);
  if (test.page == Page::section) {
    put(first, (physical_data & section_mask) | (test.permission << 10) | 2);
    return;
  }
  const bool fine =
      test.page == Page::fine_large || test.page == Page::fine_small || test.page == Page::tiny;
  const bool large = test.page == Page::coarse_large || test.page == Page::fine_large;
  put(first, second_table | (fine ? 3U : 1U));
  auto size = large ? 65536U : 4096U;
  if (test.page == Page::tiny) { size = 1024; }
  auto kind = large ? 1U : 2U;
  if (test.page == Page::tiny) { kind = 3; }
  auto permission = test.page == Page::tiny ? test.permission : test.permission * repeated_ap;
  if (test.mixed) {
    permission = 0;
    for (unsigned quarter = 0; quarter < subpage_permissions.size(); ++quarter) {
      permission |= subpage_permissions[quarter] << (quarter * 2);
    }
  }
  const auto descriptor = (physical_data & ~(size - 1)) | (permission << 4) | kind;
  const auto base = virtual_data & ~(size - 1);
  const auto stride = fine ? 1024U : 4096U;
  for (std::uint32_t address = base; address - base < size; address += stride) {
    put(second_table + (((address & section_offset) / stride) * 4), descriptor);
  }
}

bool allowed(const Case &test) {
  if (test.domain == 3) { return true; }
  if (test.domain != 1) { return false; }
  const auto permission = test.mixed ? subpage_permissions[test.quarter] : test.permission;
  const bool user = test.user || test.forced_user;
  if (permission == 0) {
    return !test.writing && (test.protection == 2 || (test.protection == 1 && !user));
  }
  if (permission == 3 || !user) { return true; }
  return permission == 2 && !test.writing;
}

void abort_state(armv5::State &state, const Case &test) {
  const auto initial_status = state.registers.cpsr;
  state.registers.cpsr = (initial_status & ~mode_mask) | fixture::abort_mode;
  state.registers.spsr = initial_status;
  state.registers.r[14] = entry + 8;
  state.registers.r[15] = abort_vector;
  state.banks.abort[1] = entry + 8;
  state.banks.spsr[3] = initial_status;
  state.progress = {.instructions = 0, .base_cycles = 3};
  const bool section = test.page == Page::section;
  auto status = section ? 9U : 11U;
  if (test.domain == 1) { status += 4; }
  state.registers.system.data_fault_status = status;
  state.registers.system.fault_address = test.process_id | guest_address(test);
}

void complete(armv5::State &expected, std::span<std::byte> ram, const Case &test, unsigned work) {
  expected.registers.r[15] = entry + (work * 4);
  expected.progress = {.instructions = work, .base_cycles = work};
  if (test.lockdown || test.enable_lockdown) {
    constexpr std::uint32_t two_locked_fills = (2U << 26) | 1;
    expected.registers.system.tlb_lockdown = two_locked_fills;
  }
  if (test.writing) {
    fixture::put(
        ram.subspan(physical_data + (test.quarter * 1024) - table, 4), output, test.big_endian);
  } else {
    expected.registers.r[0] = input;
  }
}

void run(const Case &test) {
  const auto mode = test.user ? fixture::user_mode : fixture::supervisor_mode;
  fixture::Program code(entry, test.big_endian);
  fixture::Program handler(abort_vector, test.big_endian);
  if (test.enable_lockdown) {
    constexpr std::uint32_t write_lockdown_r3 = 0xee0a3f10;
    code.word(write_lockdown_r3);
  }
  code.word(fixture::transfer(
      !test.writing, false, test.writing ? 2 : 0, 1, 0, !test.forced_user, true, test.forced_user));
  handler.word(fixture::data(13, false, 0, 0, false, 0));
  fixture::MappedMachine machine(
      {{.program = code, .modes = {mode}}, {.program = handler, .modes = {fixture::abort_mode}}},
      {abort_vector},
      true,
      working_size,
      armv5::CompileOptions::MemoryMode::direct,
      fixture::MappedMachine::Preparation::deferred);
  tables(machine, test);
  const auto data_offset = physical_data + (test.quarter * 1024) - table;
  fixture::put(std::span(machine.ram).subspan(data_offset, 4), input, test.big_endian);
  auto expected_ram = machine.ram;
  auto registers = machine.core->registers();
  registers.cpsr = (registers.cpsr & ~mode_mask) | mode;
  registers.r[1] = guest_address(test);
  registers.r[2] = output;
  registers.r[3] = test.enable_lockdown ? 1 : 0;
  registers.system.mapping.control |= 1U | (test.big_endian ? 0x80U : 0U);
  registers.system.mapping.control |= test.protection << 8;
  registers.system.mapping.translation_table = table;
  registers.system.mapping.domains = (3U << 2) | test.domain;
  registers.system.mapping.process_id = test.process_id;
  registers.system.tlb_lockdown = test.lockdown ? 1 : 0;
  machine.mapping_ = registers.system.mapping;
  machine.core->set_registers(registers);
  machine.prepare();
  auto expected = machine.core->state();
  const bool permitted = allowed(test);
  const auto work = test.enable_lockdown ? 2U : 1U;
  const auto result = machine.core->run_until(work);
  const auto label = "page=" + std::to_string(static_cast<unsigned>(test.page)) +
                     " domain=" + std::to_string(test.domain) +
                     " AP=" + std::to_string(test.permission) +
                     (test.user ? " user" : " privileged") + (test.writing ? " write" : " read");
  if (!permitted && test.page != Page::tiny && !test.forced_user && !test.mixed) {
    fixture::require(result.execution == armv5::ExecutionState::fault &&
                         result.fault.kind == armv5::Fault::Kind::memory &&
                         result.fault.address == guest_address(test),
                     "Direct page permission faults: " + label);
    fixture::contains(result.fault.reason,
                      test.writing ? "write at guest PC 0x1000" : "read at guest PC 0x1000");
  } else {
    if (permitted) {
      complete(expected, expected_ram, test, work);
    } else {
      abort_state(expected, test);
    }
    fixture::require(result.execution == armv5::ExecutionState::ready,
                     "exact fallback must retain a resumable guest boundary: " + label + " / " +
                         result.fault.reason);
    fixture::require(machine.core->state() == expected && result.progress == expected.progress,
                     "mapped page permissions and exact tiny fallback: " + label);
  }
  fixture::require(machine.ram == expected_ram, "complete mapped physical RAM: " + label);
  machine.verify_storage();
}

void permission_cases(Page page) {
  for (std::uint32_t domain = 0; domain < 4; ++domain) {
    for (std::uint32_t permission = 0; permission < 4; ++permission) {
      for (const bool user : {false, true}) {
        for (const bool writing : {false, true}) {
          run({.page = page,
               .domain = domain,
               .permission = permission,
               .user = user,
               .writing = writing,
               .big_endian = permission % 2 != 0});
        }
      }
    }
  }
}

void translated_cases(Page page) {
  for (const bool writing : {false, true}) {
    for (const bool high_alias : {false, true}) {
      run({.page = page,
           .domain = 1,
           .permission = 3,
           .user = false,
           .writing = writing,
           .big_endian = true,
           .process_id = 0x04000000,
           .high_alias = high_alias});
    }
    for (std::uint32_t permission = 0; permission < 4; ++permission) {
      run({.page = page,
           .domain = 1,
           .permission = permission,
           .user = false,
           .writing = writing,
           .big_endian = false,
           .forced_user = true});
    }
    for (std::uint32_t protection = 1; protection < 3; ++protection) {
      for (const bool user : {false, true}) {
        run({.page = page,
             .domain = 1,
             .permission = 0,
             .user = user,
             .writing = writing,
             .big_endian = false,
             .protection = protection});
      }
    }
  }
}

void subpage_cases(Page page) {
  for (unsigned quarter = 0; quarter < 4; ++quarter) {
    for (const bool user : {false, true}) {
      for (const bool writing : {false, true}) {
        run({.page = page,
             .domain = 1,
             .permission = 0,
             .user = user,
             .writing = writing,
             .big_endian = true,
             .mixed = true,
             .quarter = quarter});
      }
    }
  }
}
} // namespace
#endif

void mapped_views() {
#if defined(__linux__) && defined(__x86_64__)
  const fixture::FaultHandler handler;
  for (const auto page : {Page::section,
                          Page::coarse_large,
                          Page::coarse_small,
                          Page::fine_large,
                          Page::fine_small,
                          Page::tiny}) {
    permission_cases(page);
  }
  for (const auto page : {Page::section, Page::coarse_small, Page::tiny}) {
    translated_cases(page);
  }
  for (const auto page : {Page::coarse_small, Page::fine_small}) { subpage_cases(page); }
  run({.page = Page::section,
       .domain = 1,
       .permission = 3,
       .user = false,
       .writing = false,
       .big_endian = false,
       .lockdown = true});
  run({.page = Page::section,
       .domain = 1,
       .permission = 3,
       .user = false,
       .writing = false,
       .big_endian = false,
       .enable_lockdown = true});
#endif
}
