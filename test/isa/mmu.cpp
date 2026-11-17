// SPDX-License-Identifier: GPL-3.0-only
#include "support/mmu.hpp"

#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {
constexpr std::uint32_t load_post_word = 0xe4902004;
constexpr std::uint32_t return_abort_word = 0xe25ef004;
constexpr std::uint32_t load_word = 0xe5902000;
constexpr std::uint32_t load_multiple_word = 0xe8b0003c;
constexpr std::uint32_t move_zero_three_word = 0xe3a00003;
constexpr std::uint32_t next_pc = 0x1004;
constexpr std::uint32_t abort_link = 0x1008;
constexpr std::uint32_t code_section_descriptor = 0x00100c12;
constexpr std::uint32_t data_section_descriptor = 0x00200c32;
constexpr std::uint32_t data_section_no_ap = 0x00200032;
constexpr std::uint32_t fine_table_descriptor = 0x8033;
constexpr std::uint32_t tiny_page_descriptor = 0x00200033;
constexpr std::uint32_t privileged_data_descriptor = 0x00200432;
constexpr std::uint32_t translation_page_fault = 0x17;
constexpr std::uint32_t tail_first = 1016;
constexpr std::uint32_t tail_second = 1020;
constexpr std::uint32_t fcse_data_index = 72;
constexpr std::uint32_t old_value = 42, value = 0x11223344, flags = 0x98000000,
                        initial_data_status = 0x55, initial_instruction_status = 0xaa,
                        initial_far = 0x87654321;
constexpr std::array modes{fixture::user_mode,
                           fixture::fiq_mode,
                           fixture::irq_mode,
                           fixture::supervisor_mode,
                           fixture::abort_mode,
                           fixture::undefined_mode,
                           fixture::system_mode};

void sections(bool big, bool fixed) {
  fixture::Program code(0x1000, big);
  fixture::Program handler(0x10, big);
  code.word(load_post_word); // LDR R2,[R0],#4.
  handler.word(return_abort_word);
  fixture::MmuMachine machine({{.program = code, .modes = {modes.begin(), modes.end()}},
                               {.program = handler, .modes = {fixture::abort_mode}}},
                              {0x10},
                              big);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), value, big);
  machine.prepare(fixed);
  for (const auto mode : modes) {
    machine.core->reset(code.address());
    auto registers = machine.core->registers();
    registers.cpsr = flags | mode;
    registers.r[0] = fixture::MmuMachine::data_virtual;
    registers.system.mapping = machine.mapping();
    registers.system.data_fault_status = initial_data_status;
    registers.system.instruction_fault_status = initial_instruction_status;
    registers.system.fault_address = initial_far;
    machine.core->set_registers(registers);
    const auto before = machine.core->state();
    (void)machine.core->install(machine.prepared(), 0);
    fixture::require(machine.core->state() == before, "translated install has no state effects");
    const auto result = machine.core->run(1);
    registers.r[0] += 4;
    registers.r[2] = value;
    registers.r[15] = code.pc();
    fixture::require(
        result.execution == armv5::ExecutionState::ready &&
            result.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
            machine.core->registers() == registers,
        "section translation preserves CPU state, original writeback and fault registers");
  }
}

struct PermissionInstruction {
  std::uint32_t word;
  bool writing, swap;
};

struct PermissionCase {
  unsigned domain, protection, permission;
  std::uint32_t mode;
};

void permission_case(fixture::MmuMachine &machine,
                     PermissionInstruction operation,
                     PermissionCase input) {
  const auto [domain, protection, permission, mode] = input;
  constexpr std::array<unsigned, 4> ap_masks{0, 12, 13, 15};
  constexpr std::array<unsigned, 4> zero_masks{0, 4, 5, 0};
  const auto mask = permission == 0 ? zero_masks[protection] : ap_masks[permission];
  auto bit = operation.writing ? 2U : 1U;
  if (mode != fixture::user_mode) { bit *= 4; }
  const bool allowed = domain == 3 || (domain == 1 && (mask & bit) != 0);
  const bool unpredictable = domain == 1 && permission == 0 && protection == 3;
  machine.first(32, data_section_no_ap | (permission << 10));
  std::ranges::fill(machine.data, std::byte{0});
  fixture::put(std::span(machine.data).subspan(0x4000, 4), value, machine.big);
  machine.core->reset(0x1000);
  auto registers = machine.core->registers();
  registers.cpsr = flags | mode;
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[1] = initial_far;
  registers.r[2] = initial_far;
  registers.system.mapping = machine.mapping();
  registers.system.data_fault_status = initial_data_status;
  registers.system.instruction_fault_status = initial_instruction_status;
  registers.system.fault_address = initial_far;
  machine.core->set_registers(registers);
  (void)machine.core->install(machine.prepared(), 0);
  auto expected = machine.core->state();
  auto ram = machine.data;
  if (allowed) {
    if (operation.writing) {
      fixture::put(std::span(ram).subspan(0x4000, 4), initial_far, machine.big);
    }
    if (!operation.writing || operation.swap) { expected.registers.r[2] = value; }
    if (!operation.swap) { expected.registers.r[0] += 4; }
    expected.registers.r[15] = next_pc;
    expected.progress = {.instructions = 1, .base_cycles = operation.swap ? 2U : 1U};
  } else if (!unpredictable) {
    constexpr std::uint32_t domain_fault = 0x19;
    constexpr std::uint32_t permission_fault = 0x1d;
    constexpr std::uint32_t abort_status = 0x98000097;
    expected.registers.cpsr = abort_status;
    expected.registers.spsr = registers.cpsr;
    expected.registers.r[14] = abort_link;
    expected.registers.r[15] = 0x10;
    expected.registers.system.data_fault_status = domain == 1 ? permission_fault : domain_fault;
    expected.registers.system.fault_address = registers.r[0];
    expected.banks.abort[1] = abort_link;
    expected.banks.spsr[3] = registers.cpsr;
    expected.progress = {.instructions = 0, .base_cycles = 3};
  }
  const auto result = machine.core->run_until(1);
  if (unpredictable) {
    expected.execution = armv5::ExecutionState::fault;
    expected.fault = result.fault;
    fixture::require(result.fault.kind == armv5::Fault::Kind::unsupported &&
                         result.fault.address == registers.r[0],
                     "AP00/S=R=1 explicit refusal");
  }
  fixture::require(machine.core->state() == expected && machine.data == ram,
                   "domain/AP/S/R read/write/SWP privilege truth table, complete state and memory");
}

void permissions(bool big, bool fixed) {
  constexpr std::array instructions{
      PermissionInstruction{.word = load_post_word, .writing = false, .swap = false},
      PermissionInstruction{.word = 0xe4802004, .writing = true, .swap = false},
      PermissionInstruction{.word = 0xe1002091, .writing = true, .swap = true}};
  for (const auto operation : instructions) {
    for (unsigned domain = 0; domain < 4; ++domain) {
      for (unsigned protection = 0; protection < 4; ++protection) {
        fixture::Program code(0x1000, big);
        fixture::Program handler(0x10, big);
        code.word(operation.word);
        handler.word(return_abort_word);
        fixture::MmuMachine machine(
            {{.program = code, .modes = {fixture::user_mode, fixture::supervisor_mode}},
             {.program = handler, .modes = {fixture::abort_mode}}},
            {0x10},
            big);
        machine.context.control |= protection << 8;
        machine.context.domains = 3 | (domain << 2);
        machine.prepare(fixed);
        for (unsigned permission = 0; permission < 4; ++permission) {
          for (const auto mode : {fixture::user_mode, fixture::supervisor_mode}) {
            permission_case(machine,
                            operation,
                            {.domain = domain,
                             .protection = protection,
                             .permission = permission,
                             .mode = mode});
          }
        }
      }
    }
  }
}

void pages(bool big, bool fixed) {
  struct Case {
    std::uint32_t first, second;
    unsigned offset;
  };

  constexpr std::array cases{Case{.first = 0x00008031, .second = 0x00208ff2, .offset = 0x8000},
                             Case{.first = 0x00008031, .second = 0x00200ff1, .offset = 0x4000},
                             Case{.first = 0x00008033, .second = 0x00208ff2, .offset = 0x8000},
                             Case{.first = 0x00008033, .second = 0x00200ff1, .offset = 0x4000},
                             Case{.first = 0x00008033, .second = 0x0020c033, .offset = 0xc000}};
  fixture::Program code(0x1000, big);
  code.word(load_word);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}}, {}, big);
  machine.prepare(fixed);
  for (const auto test : cases) {
    machine.first(32, test.first);
    machine.page(test.second);
    std::ranges::fill(machine.data, std::byte{0});
    fixture::put(std::span(machine.data).subspan(test.offset, 4), value, big);
    machine.core->reset(0x1000);
    auto expected = machine.core->registers();
    expected.system.mapping = machine.mapping();
    expected.r[0] = fixture::MmuMachine::data_virtual;
    machine.core->set_registers(expected);
    (void)machine.core->install(machine.prepared(), 0);
    const auto ram = machine.data;
    expected.r[2] = value;
    expected.r[15] = code.pc();
    const auto result = machine.core->run(1);
    fixture::require(result.execution == armv5::ExecutionState::ready &&
                         machine.core->registers() == expected && machine.data == ram &&
                         result.progress == armv5::Progress{.instructions = 1, .base_cycles = 1},
                     "coarse/fine small/large/tiny page physical translation");
  }
}

void fcse(bool big) {
  constexpr std::uint32_t pid = 0x04000000;
  constexpr std::uint32_t address = 0x00804000;
  constexpr std::uint32_t mva = 0x04804000;
  fixture::Program code(0x1000, big);
  fixture::Program handler(0x10, big);
  code.word(load_word);
  handler.word(return_abort_word);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}},
                               {.program = handler, .modes = {fixture::abort_mode}}},
                              {0x10},
                              big);
  machine.context.process_id = pid;
  machine.context.control |= 2;
  machine.first(64, code_section_descriptor);              // Modified low instruction address.
  machine.first(fcse_data_index, data_section_descriptor); // Modified low data address.
  fixture::put(std::span(machine.data).subspan(0x4000, 4), value, big);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[0] = address;
  machine.core->set_registers(registers);
  fixture::require(machine.core->run(1).execution == armv5::ExecutionState::ready &&
                       machine.core->registers().r[2] == value,
                   "FCSE modifies instruction and data low addresses");
  constexpr std::uint32_t high_address = 0x08004000;
  machine.first(128, data_section_descriptor);
  registers.r[0] = high_address;
  registers.r[15] = code.address();
  machine.core->set_registers(registers);
  fixture::require(machine.core->run(1).execution == armv5::ExecutionState::ready &&
                       machine.core->registers().r[2] == value,
                   "FCSE leaves addresses above 32MB unchanged");
  machine.core->reset(code.address());
  machine.first(fcse_data_index, 0);
  registers.r[0] = address + 1;
  registers.r[15] = 0x1000;
  machine.core->set_registers(registers);
  (void)machine.core->install(machine.prepared(), 0);
  const auto result = machine.core->run_until(machine.core->state().progress.base_cycles + 1);
  fixture::require(
      result.progress == armv5::Progress{.instructions = 0, .base_cycles = 3} &&
          machine.core->registers().system.data_fault_status == 1 &&
          machine.core->registers().system.fault_address == mva + 1 &&
          machine.core->registers().r[0] == address + 1 && machine.core->registers().r[15] == 0x10,
      "FCSE alignment FAR is modified while original guest base and abort PC remain virtual");
}

void fault_classes(bool big) {
  struct Case {
    std::uint32_t first, second, domains, status;
    bool missing_first = false, missing_second = false, user = false;
  };

  constexpr std::array cases{
      Case{.first = 0, .second = 0, .domains = 7, .status = 5},
      Case{.first = 0x8031, .second = 0, .domains = 7, .status = translation_page_fault},
      Case{.first = fine_table_descriptor, .second = 0x0020c033, .domains = 3, .status = 0x1b},
      Case{.first = 0x8031, .second = 0x00208552, .domains = 7, .status = 0x1f, .user = true},
      Case{.first = data_section_descriptor,
           .second = 0,
           .domains = 7,
           .status = 12,
           .missing_first = true},
      Case{.first = 0x8031,
           .second = 0x00208ff2,
           .domains = 7,
           .status = 0x1e,
           .missing_second = true}};
  for (const auto test : cases) {
    fixture::Program code(0x1000, big);
    fixture::Program handler(0x10, big);
    code.word(load_post_word);
    handler.word(return_abort_word);
    const auto mode = test.user ? fixture::user_mode : fixture::supervisor_mode;
    fixture::MmuMachine machine(
        {{.program = code, .modes = {mode}}, {.program = handler, .modes = {fixture::abort_mode}}},
        {0x10},
        big);
    machine.first(32, test.first);
    machine.page(test.second);
    machine.context.domains = test.domains;
    for (auto &region : machine.regions) {
      if (test.missing_first && region.address == fixture::MmuMachine::table_address) {
        region.bytes = region.bytes.first(4);
      }
      if (test.missing_second && region.address == fixture::MmuMachine::page_address) {
        region.bytes = region.bytes.first(1);
      }
    }
    machine.core->set_memory({.regions = machine.regions, .generation = 1});
    machine.prepare(false);
    for (unsigned byte_offset = 0; byte_offset < 4; ++byte_offset) {
      machine.core->reset(code.address());
      auto registers = machine.core->registers();
      registers.cpsr = flags | mode;
      registers.r[0] = fixture::MmuMachine::data_virtual + byte_offset;
      registers.system.mapping = machine.mapping();
      registers.r[2] = initial_far;
      registers.system.instruction_fault_status = initial_instruction_status;
      machine.core->set_registers(registers);
      (void)machine.core->install(machine.prepared(), 0);
      auto expected = machine.core->state();
      constexpr std::uint32_t abort_status = 0x98000097;
      expected.registers.cpsr = abort_status;
      expected.registers.spsr = registers.cpsr;
      expected.registers.r[14] = abort_link;
      expected.registers.r[15] = 0x10;
      expected.banks.abort[1] = abort_link;
      expected.banks.spsr[3] = registers.cpsr;
      expected.registers.system.data_fault_status = test.status;
      expected.registers.system.fault_address = registers.r[0];
      expected.progress = {.instructions = 0, .base_cycles = 3};
      const auto ram = machine.data;
      const auto result = machine.core->run_until(1);
      fixture::require(machine.core->state() == expected && machine.data == ram &&
                           result.progress == expected.progress,
                       "first/second-level translation, domain, permission and page-walk external "
                       "aborts have distinct exact status/prefixes");
    }
  }
}

struct SwapAccess {
  bool byte, readable, writable;
  std::uint32_t mode;
  unsigned offset;
};

void uncached_swap_case(fixture::MmuMachine &machine, SwapAccess access) {
  const auto [byte, readable, writable, mode, offset] = access;
  fixture::put(std::span(machine.data).subspan(0x4000, 4), value, machine.big);
  machine.core->reset(0x1000);
  auto registers = machine.core->registers();
  registers.cpsr = flags | mode;
  registers.r[0] = fixture::MmuMachine::data_virtual + offset;
  registers.r[1] = initial_far;
  registers.r[2] = old_value;
  registers.system.mapping = machine.mapping();
  registers.system.instruction_fault_status = initial_instruction_status;
  machine.core->set_registers(registers);
  (void)machine.core->install(machine.prepared(), 0);
  auto expected = machine.core->state();
  auto ram = machine.data;
  if (writable) {
    if (byte) {
      ram[0x4000 + offset] = std::byte(initial_far & fixture::byte_mask);
    } else {
      fixture::put(std::span(ram).subspan(0x4000, 4), initial_far, machine.big);
    }
  }
  if (readable && writable) {
    const auto shift = (machine.big ? 3 - offset : offset) * 8;
    expected.registers.r[2] = byte ? (value >> shift) & fixture::byte_mask
                                   : std::rotr(value, static_cast<int>(offset * 8));
    expected.registers.r[15] = next_pc;
    expected.progress = {.instructions = 1, .base_cycles = 2};
  } else {
    constexpr std::uint32_t abort_status = 0x98000097;
    expected.registers.cpsr = abort_status;
    expected.registers.spsr = registers.cpsr;
    expected.registers.r[14] = abort_link;
    expected.registers.r[15] = 0x10;
    expected.banks.abort[1] = abort_link;
    expected.banks.spsr[3] = registers.cpsr;
    expected.registers.system.data_fault_status = 8;
    expected.registers.system.fault_address = registers.r[0];
    expected.progress = {.instructions = 0, .base_cycles = 3};
  }
  const auto result = machine.core->run_until(1);
  fixture::require(machine.core->state() == expected && machine.data == ram &&
                       result.progress == expected.progress,
                   "NCNB/NCB SWP attempts the write after a physical read abort, preserving "
                   "destination, MVA fault status and exact retirement");
}

void uncached_swap_permissions(fixture::MmuMachine &machine, bool byte, bool fixed) {
  for (const bool readable : {false, true}) {
    for (const bool writable : {false, true}) {
      auto &region = *std::ranges::find(
          machine.regions, fixture::MmuMachine::data_physical, &armv5::Memory::Region::address);
      region.readable = readable;
      region.writable = writable;
      auto &layout = *std::ranges::find(
          machine.layout, fixture::MmuMachine::data_physical, &armv5::RegionLayout::address);
      layout.readable = readable;
      layout.writable = writable;
      machine.core->set_memory({.regions = machine.regions, .generation = 1});
      machine.prepare(fixed);
      for (const auto mode : modes) {
        for (unsigned offset = 0; offset < 4; ++offset) {
          uncached_swap_case(machine,
                             {.byte = byte,
                              .readable = readable,
                              .writable = writable,
                              .mode = mode,
                              .offset = offset});
        }
      }
    }
  }
}

void uncached_swaps(bool big, bool fixed) {
  constexpr std::uint32_t swap_word = 0xe1002091;
  constexpr std::uint32_t swap_byte = 0xe1402091;
  for (const bool byte : {false, true}) {
    for (const bool buffered : {false, true}) {
      fixture::Program code(0x1000, big);
      fixture::Program handler(0x10, big);
      code.word(byte ? swap_byte : swap_word); // SWP{B} R2,R1,[R0].
      handler.word(return_abort_word);
      fixture::MmuMachine machine({{.program = code, .modes = {modes.begin(), modes.end()}},
                                   {.program = handler, .modes = {fixture::abort_mode}}},
                                  {0x10},
                                  big);
      machine.context.control |= 4; // Global C cannot cache an NCNB/NCB descriptor.
      machine.first(32, data_section_descriptor | (buffered ? 4U : 0U));
      uncached_swap_permissions(machine, byte, fixed);
    }
  }
}

void multiple_fault(bool page_fault) {
  fixture::Program code;
  fixture::Program handler(0x10);
  code.word(load_multiple_word);
  handler.word(return_abort_word);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}},
                               {.program = handler, .modes = {fixture::abort_mode}}},
                              {0x10});
  if (page_fault) {
    machine.first(32, fine_table_descriptor);
    machine.page(0);
    fixture::put(std::span(machine.pages).first(4), tiny_page_descriptor);
  } else {
    for (auto &region : machine.regions) {
      if (region.address == fixture::MmuMachine::data_physical) {
        region.bytes = region.bytes.first(1024);
      }
    }
    machine.core->set_memory({.regions = machine.regions, .generation = 1});
  }
  fixture::put(std::span(machine.data).subspan(tail_first, 4), old_value);
  fixture::put(std::span(machine.data).subspan(tail_second, 4), value);
  machine.prepare(false);
  auto registers = machine.core->registers();
  constexpr std::uint32_t first = 0x020003f8;
  constexpr std::uint32_t last = 0x02000404;
  constexpr std::uint32_t failed = 0x02000400;
  registers.r[0] = first;
  registers.r[4] = initial_far;
  machine.core->set_registers(registers);
  const auto result = machine.core->run_until(1);
  fixture::require(
      result.execution == armv5::ExecutionState::ready &&
          result.progress == armv5::Progress{.instructions = 0, .base_cycles = 3} &&
          machine.core->registers().r[0] == first && machine.core->registers().r[2] == old_value &&
          machine.core->registers().r[3] == value &&
          machine.core->registers().r[4] == initial_far &&
          machine.core->registers().system.data_fault_status ==
              (page_fault ? translation_page_fault : 8U) &&
          machine.core->registers().system.fault_address == (page_fault ? failed : last),
      "multiple MMU abort keeps completed loads and restores base; translation FAR differs from "
      "external burst FAR");
}

void forced_user() {
  constexpr std::array instructions{0xe4b02004U, 0xe4a02004U}; // LDRT/STRT R2,[R0],#4.
  for (const auto instruction : instructions) {
    fixture::Program code;
    fixture::Program handler(0x10);
    code.word(instruction);
    handler.word(return_abort_word);
    fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}},
                                 {.program = handler, .modes = {fixture::abort_mode}}},
                                {0x10});
    machine.first(32, privileged_data_descriptor); // AP=1: privileged only.
    machine.prepare();
    auto registers = machine.core->registers();
    registers.r[0] = fixture::MmuMachine::data_virtual;
    machine.core->set_registers(registers);
    const auto ram = machine.data;
    const auto result = machine.core->run_until(1);
    constexpr std::uint32_t permission_fault = 0x1d;
    fixture::require(result.progress == armv5::Progress{.instructions = 0, .base_cycles = 3} &&
                         machine.core->registers().system.data_fault_status == permission_fault &&
                         machine.data == ram && machine.core->registers().r[0] == registers.r[0],
                     "LDRT/STRT use User permissions from Supervisor mode");
  }
}

void prefetch_faults(bool big) {
  struct Case {
    std::uint32_t first, domains, status;
    bool user = false, missing_second = false;
  };

  constexpr std::array cases{
      Case{.first = 0, .domains = 7, .status = 5},
      Case{.first = 0x8031, .domains = 7, .status = translation_page_fault},
      Case{.first = code_section_descriptor, .domains = 4, .status = 9},
      Case{.first = 0x00100df2, .domains = 4, .status = 0xf9},
      Case{.first = 0x00100412, .domains = 5, .status = 13, .user = true},
      Case{.first = 0x8031, .domains = 7, .status = 0x1e, .missing_second = true}};
  for (const auto test : cases) {
    fixture::Program code(0x1000, big);
    fixture::Program handler(12, big);
    code.word(move_zero_three_word);
    handler.word(return_abort_word);
    const auto mode = test.user ? fixture::user_mode : fixture::supervisor_mode;
    fixture::MmuMachine machine(
        {{.program = code, .modes = {mode}}, {.program = handler, .modes = {fixture::abort_mode}}},
        {12},
        big);
    machine.context.domains = test.domains;
    if (test.missing_second) {
      for (auto &region : machine.regions) {
        if (region.address == fixture::MmuMachine::page_address) {
          region.bytes = region.bytes.first(1);
        }
      }
      machine.core->set_memory({.regions = machine.regions, .generation = 1});
    }
    machine.prepare(false);
    machine.first(0, test.first);
    auto registers = machine.core->registers();
    registers.cpsr = flags | mode;
    registers.r[0] = initial_far;
    registers.system.data_fault_status = initial_data_status;
    registers.system.fault_address = initial_far;
    machine.core->set_registers(registers);
    auto expected = machine.core->state();
    constexpr std::uint32_t abort_status = 0x98000097;
    expected.registers.cpsr = abort_status;
    expected.registers.spsr = registers.cpsr;
    expected.registers.r[15] = 12;
    expected.registers.r[14] = next_pc;
    expected.banks.abort[1] = next_pc;
    expected.banks.spsr[3] = registers.cpsr;
    expected.registers.system.instruction_fault_status = test.status;
    expected.progress = {.instructions = 0, .base_cycles = 3};
    const auto result = machine.core->run_until(1);
    fixture::require(machine.core->state() == expected && result.progress == expected.progress,
                     "instruction faults precede execution and update only IFSR, with virtual "
                     "PC/LR and exact exception state");
  }
}

void unknown_fetch() {
  constexpr std::uint32_t target = 0x01001000;
  constexpr std::array descriptors{0U, 0x00400c12U, code_section_descriptor};
  constexpr std::array statuses{5U, 8U, 0U};
  for (unsigned index = 0; index < descriptors.size(); ++index) {
    fixture::Program code;
    fixture::Program handler(12);
    code.word(fixture::branch(code.pc(), {.address = target}));
    handler.word(return_abort_word);
    fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}},
                                 {.program = handler, .modes = {fixture::abort_mode}}},
                                {12});
    machine.first(16, descriptors[index]);
    machine.prepare();
    const auto result = machine.core->run_until(4);
    if (statuses[index] == 0) {
      fixture::require(result.execution == armv5::ExecutionState::fault &&
                           result.fault.kind == armv5::Fault::Kind::unprepared &&
                           result.fault.address == target &&
                           result.progress == armv5::Progress{.instructions = 1, .base_cycles = 3},
                       "a valid translated alias still needs an explicit guest code declaration");
    } else {
      fixture::require(
          result.execution == armv5::ExecutionState::ready &&
              result.progress == armv5::Progress{.instructions = 1, .base_cycles = 6} &&
              machine.core->registers().r[15] == 12 &&
              machine.core->registers().r[14] == target + 4 &&
              machine.core->registers().system.instruction_fault_status == statuses[index],
          "translation/physical instruction fault precedes an undeclared destination diagnostic");
    }
  }
}

void translated_tcm(bool denied) {
  fixture::Program code;
  fixture::Program handler(0x10);
  code.word(fixture::transfer(true, false, 2, 0, 0));
  constexpr std::uint32_t abort_return = return_abort_word;
  constexpr std::uint32_t itcm_region = 0x0010100d;
  constexpr std::uint32_t dtcm_region = 0x0020400d;
  constexpr std::uint32_t privileged_data = privileged_data_descriptor;
  constexpr std::uint32_t permission_fault = 0x1d;
  handler.word(abort_return);
  const auto mode = denied ? fixture::user_mode : fixture::supervisor_mode;
  fixture::MmuMachine machine(
      {{.program = code, .modes = {mode}}, {.program = handler, .modes = {fixture::abort_mode}}},
      {0x10});
  std::vector<std::byte> itcm(4096);
  std::vector<std::byte> dtcm(4096);
  std::ranges::copy(code.bytes(), itcm.begin());
  fixture::put(std::span(dtcm).first(4), value);
  std::array<std::byte, 4> poison{};
  machine.regions[0].bytes = poison;
  machine.regions.push_back({.address = 0,
                             .bytes = itcm,
                             .readable = true,
                             .writable = true,
                             .executable = true,
                             .space = armv5::Memory::Space::itcm});
  machine.regions.push_back({.address = 0,
                             .bytes = dtcm,
                             .readable = true,
                             .writable = true,
                             .executable = false,
                             .space = armv5::Memory::Space::dtcm});
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  machine.context.instruction_tcm = itcm_region;
  machine.context.data_tcm = dtcm_region;
  if (denied) { machine.first(32, privileged_data); }
  machine.prepare(false);
  auto registers = machine.core->registers();
  registers.cpsr = mode;
  registers.r[0] = fixture::MmuMachine::data_virtual;
  machine.core->set_registers(registers);
  const auto old_itcm = itcm;
  const auto old_dtcm = dtcm;
  const auto result = machine.core->run_until(1);
  if (denied) {
    fixture::require(result.progress == armv5::Progress{.instructions = 0, .base_cycles = 3} &&
                         machine.core->registers().system.data_fault_status == permission_fault &&
                         machine.core->registers().system.fault_address == registers.r[0] &&
                         machine.core->registers().r[15] == 0x10,
                     "TCM data still obeys MMU permissions before physical access");
  } else {
    fixture::require(
        result.execution == armv5::ExecutionState::ready &&
            result.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
            machine.core->registers().r[2] == value && machine.core->registers().r[15] == code.pc(),
        "MMU-translated physical addresses select ITCM instruction bytes and DTCM data");
  }
  fixture::require(itcm == old_itcm && dtcm == old_dtcm,
                   "translated TCM reads preserve consumer storage");
}

} // namespace

void mmu_instructions() {
  multiple_fault(false);
  multiple_fault(true);
  forced_user();
  unknown_fetch();
  translated_tcm(false);
  translated_tcm(true);
  for (const bool big : {false, true}) {
    for (const bool fixed : {false, true}) {
      sections(big, fixed);
      pages(big, fixed);
      permissions(big, fixed);
      uncached_swaps(big, fixed);
    }
    fcse(big);
    fault_classes(big);
    prefetch_faults(big);
  }
}
