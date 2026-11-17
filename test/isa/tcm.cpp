// SPDX-License-Identifier: GPL-3.0-only
#include "support/tcm.hpp"

#include "armv5/core.hpp"
#include "support/isa.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace {
using Machine = fixture::TcmMachine;
using Layout = Machine::Layout;
constexpr std::uint32_t entry = 0x1000, bus_i = 0x11111111, tcm_i = 0x22222222, bus_d = 0x33333333,
                        tcm_d = 0x44444444, stored_i = 0x55667788, stored_d = 0x99aabbcc,
                        top_itcm = 0xfffff000;

struct AccessCase {
  bool instruction, data, big;
  Layout layout;
  std::uint32_t instruction_base = Machine::instruction_base, data_base = Machine::data_base;
};

void access(AccessCase test) {
  fixture::Program code(entry, test.big);
  code.word(fixture::transfer(true, false, 0, 4, 0));
  code.word(fixture::transfer(true, false, 1, 5, 0));
  code.word(fixture::transfer(false, false, 2, 4, 4));
  code.word(fixture::transfer(false, false, 3, 5, 4));
  constexpr std::uint32_t swap_r6_r2_r5 = 0xe1056092;
  code.word(swap_r6_r2_r5);
  Machine machine(code);
  fixture::put(std::span(machine.bus_instruction).first(4), bus_i, test.big);
  fixture::put(std::span(machine.instruction).first(4), tcm_i, test.big);
  fixture::put(std::span(machine.bus_data).first(4), bus_d, test.big);
  fixture::put(std::span(machine.data).first(4), tcm_d, test.big);
  armv5::Registers::System::Mapping mapping;
  mapping.control |= test.big ? 0x80 : 0;
  mapping.instruction_tcm = test.instruction_base | 12U | unsigned(test.instruction);
  mapping.data_tcm = test.data_base | 16U | unsigned(test.data);
  const std::array spans{
      armv5::CodeSpan{.address = entry, .bytes = code.bytes(), .processor_modes = {}}};
  const std::array entries{entry};
  const std::array variants{
      armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries, .mapping = mapping}};
  const armv5::CodeImage image{.identity = "authored-tcm-accesses",
                               .variants = variants,
                               .layouts = {},
                               .transitions = {},
                               .interrupt_vectors = {}};
  const auto native = machine.compile(image, test.layout);
  auto expected = machine.core->registers();
  expected.system.mapping = mapping;
  expected.r[2] = stored_i;
  expected.r[3] = stored_d;
  expected.r[4] = test.instruction_base;
  expected.r[5] = test.data_base;
  machine.core->set_registers(expected);
  (void)machine.core->install(native, 0);
  auto readonly_write = expected;
  constexpr std::uint32_t size_bits = 0x3c;
  readonly_write.system.mapping.instruction_tcm &= ~size_bits;
  readonly_write.system.mapping.data_tcm &= ~size_bits;
  machine.core->set_registers(readonly_write);
  auto expected_bus_i = machine.bus_instruction;
  auto expected_bus_d = machine.bus_data;
  auto expected_i = machine.instruction;
  auto expected_d = machine.data;
  auto &selected_i = test.instruction ? expected_i : expected_bus_i;
  auto &selected_d = test.data ? expected_d : expected_bus_d;
  fixture::put(std::span(selected_i).subspan(4, 4), stored_i, test.big);
  fixture::put(std::span(selected_d).first(4), stored_i, test.big);
  fixture::put(std::span(selected_d).subspan(4, 4), stored_d, test.big);
  expected.r[0] = test.instruction ? tcm_i : bus_i;
  expected.r[1] = test.data ? tcm_d : bus_d;
  expected.r[6] = expected.r[1];
  expected.r[15] = code.pc();
  const auto result = machine.core->run(5);
  fixture::require(
      machine.core->registers() == expected && result.execution == armv5::ExecutionState::ready &&
          result.progress == armv5::Progress{.instructions = 5, .base_cycles = 6} &&
          machine.bus_instruction == expected_bus_i && machine.bus_data == expected_bus_d &&
          machine.instruction == expected_i && machine.data == expected_d,
      "TCM/bus routing, shadowing, byte order and atomic exchange");
}

void instruction_fetch(bool instruction_side, bool modify, bool alias) {
  fixture::Program code;
  code.word(fixture::transfer(false, false, 0, 1, 0));
  constexpr std::uint32_t bx_r2 = 0xe12fff12;
  code.word(bx_r2);
  Machine machine(code);
  const auto base = instruction_side ? Machine::instruction_base : Machine::data_base;
  const auto target = base + 0x40;
  fixture::Program destination(target);
  destination.word(fixture::data(13, false, 0, 0, true, 5));
  auto &backing = instruction_side ? machine.instruction : machine.data;
  auto &bus = instruction_side ? machine.bus_instruction : machine.bus_data;
  std::ranges::copy(destination.bytes(), backing.begin() + 0x40);
  std::ranges::copy(destination.bytes(), bus.begin() + 0x40);
  const auto original_bus = bus;
  constexpr std::uint32_t alias_base = 0xc000;
  if (alias) {
    machine.regions.push_back({.address = alias_base,
                               .bytes = backing,
                               .readable = true,
                               .writable = true,
                               .executable = false});
    machine.core->set_memory({.regions = machine.regions, .generation = 1});
  }
  auto mapping = machine.core->registers().system.mapping;
  if (instruction_side) {
    mapping.instruction_tcm = base | 13U;
  } else {
    mapping.data_tcm = base | 16U | 1U;
  }
  const std::array spans{
      armv5::CodeSpan{.address = entry, .bytes = code.bytes(), .processor_modes = {}},
      armv5::CodeSpan{.address = target, .bytes = destination.bytes(), .processor_modes = {}}};
  const std::array entries{entry};
  const std::array variants{
      armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries, .mapping = mapping}};
  const armv5::CodeImage image{.identity = "authored-tcm-code-view",
                               .variants = variants,
                               .layouts = {},
                               .transitions = {},
                               .interrupt_vectors = {}};
  const auto native = machine.compile(image, Layout::all);
  auto registers = machine.core->registers();
  registers.system.mapping = mapping;
  const auto replacement = fixture::data(13, false, 0, 0, true, modify ? 7 : 5);
  registers.r[0] = replacement;
  registers.r[1] = alias ? alias_base + 0x40 : target;
  registers.r[2] = target;
  machine.core->set_registers(registers);
  (void)machine.core->install(native, 0);
  const auto result = machine.core->run(3);
  const bool revoked = instruction_side && modify;
  fixture::require(
      result.execution == (revoked ? armv5::ExecutionState::fault : armv5::ExecutionState::ready) &&
          result.progress == armv5::Progress{.instructions = revoked ? 1U : 3U,
                                             .base_cycles = revoked ? 1U : 5U} &&
          machine.core->registers().r[15] == (revoked ? entry + 4 : target + 4) &&
          bus == original_bus && fixture::get(std::span(backing).subspan(0x40, 4)) == replacement,
      "ITCM code writes revoke; DTCM data writes do not affect bus instruction fetch");
  if (revoked) {
    fixture::require(
        result.fault.kind == armv5::Fault::Kind::changed_code &&
            result.fault.address == registers.r[1],
        "TCM executable alias changes preserve store address and complete instruction boundary");
  } else {
    fixture::require(machine.core->registers().r[0] == 5, "DTCM is never an instruction source");
  }
}

void geometries(Layout layout) {
  struct Geometry {
    std::uint32_t bytes, bits;
  };

  constexpr std::array sizes{Geometry{.bytes = 4096, .bits = 12},
                             Geometry{.bytes = 8192, .bits = 16},
                             Geometry{.bytes = 16384, .bits = 20},
                             Geometry{.bytes = 32768, .bits = 24},
                             Geometry{.bytes = 65536, .bits = 28},
                             Geometry{.bytes = 131072, .bits = 32},
                             Geometry{.bytes = 262144, .bits = 36},
                             Geometry{.bytes = 524288, .bits = 40},
                             Geometry{.bytes = 1048576, .bits = 44}};
  constexpr std::uint32_t instruction_base = 0x200000;
  constexpr std::uint32_t data_base = 0x400000;
  for (const auto &geometry : sizes) {
    fixture::Program code;
    code.word(fixture::transfer(true, false, 0, 2, 0));
    code.word(fixture::transfer(true, false, 1, 3, 0));
    Machine machine(code);
    machine.instruction.resize(geometry.bytes);
    machine.data.resize(geometry.bytes);
    machine.regions[3].bytes = machine.instruction;
    machine.regions[4].bytes = machine.data;
    machine.core->set_memory({.regions = machine.regions, .generation = 1});
    fixture::put(std::span(machine.instruction).last(4), tcm_i);
    fixture::put(std::span(machine.data).last(4), tcm_d);
    auto expected = machine.core->registers();
    fixture::require(expected.system.mapping.instruction_tcm == geometry.bits &&
                         expected.system.mapping.data_tcm == geometry.bits,
                     "all nine TCM geometry encodings are read-only profile state");
    expected.system.mapping.instruction_tcm = instruction_base | geometry.bits | 1;
    expected.system.mapping.data_tcm = data_base | geometry.bits | 1;
    expected.r[2] = instruction_base + geometry.bytes - 4;
    expected.r[3] = data_base + geometry.bytes - 4;
    machine.core->set_registers(expected);
    const std::array spans{
        armv5::CodeSpan{.address = entry, .bytes = code.bytes(), .processor_modes = {}}};
    const std::array entries{entry};
    const std::array variants{armv5::CodeVariant{
        .id = 0, .spans = spans, .entries = entries, .mapping = expected.system.mapping}};
    const armv5::CodeImage image{.identity = "authored-all-tcm-geometries",
                                 .variants = variants,
                                 .layouts = {},
                                 .transitions = {},
                                 .interrupt_vectors = {}};
    const auto native = machine.compile(image, layout);
    (void)machine.core->install(native, 0);
    const auto result = machine.core->run(2);
    expected.r[0] = tcm_i;
    expected.r[1] = tcm_d;
    expected.r[15] = code.pc();
    fixture::require(machine.core->registers() == expected &&
                         result.execution == armv5::ExecutionState::ready &&
                         result.progress == armv5::Progress{.instructions = 2, .base_cycles = 2},
                     "every legal TCM size routes its last word through direct and dynamic paths");
  }
}
} // namespace

void tcm_accesses() {
  geometries(Layout::all);
  geometries(Layout::dynamic);
  for (const bool instruction : {false, true}) {
    for (const bool data : {false, true}) {
      for (const bool big : {false, true}) {
        for (const auto layout : {Layout::dynamic, Layout::bus, Layout::all}) {
          access({.instruction = instruction, .data = data, .big = big, .layout = layout});
        }
      }
    }
  }
  for (const auto layout : {Layout::dynamic, Layout::bus, Layout::all}) {
    access({.instruction = true,
            .data = true,
            .big = false,
            .layout = layout,
            .instruction_base = top_itcm,
            .data_base = 0});
  }
  for (const bool instruction : {false, true}) {
    instruction_fetch(instruction, false, false);
    instruction_fetch(instruction, true, false);
    instruction_fetch(instruction, true, true);
  }
}
