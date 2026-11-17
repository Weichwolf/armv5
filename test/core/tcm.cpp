// SPDX-License-Identifier: GPL-3.0-only
#include "support/tcm.hpp"

#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using Machine = fixture::TcmMachine;
using Mapping = armv5::Registers::System::Mapping;
constexpr std::uint32_t entry = 0x1000, move_r1 = 0xe3a01005, mcr_dtc = 0xee090f11,
                        instruction_fields = 0x0fffffff, raw_table = 0x1234ffff,
                        aligned_table = 0x1234c000, misaligned_dtc = 0x9001,
                        overlapping_dtc = 0x4001, absent_dtc = 0x8001;
enum class Field : std::uint8_t { control, table, domains, process, instruction, data };

struct WriteCase {
  Field field;
  std::uint32_t instruction, input, expected;
};

Mapping changed(Mapping result, WriteCase test) {
  switch (test.field) {
    case Field::control: result.control = test.expected; break;
    case Field::table: result.translation_table = test.expected; break;
    case Field::domains: result.domains = test.expected; break;
    case Field::process: result.process_id = test.expected; break;
    case Field::instruction: result.instruction_tcm = test.expected; break;
    case Field::data: result.data_tcm = test.expected; break;
  }
  return result;
}

void transition(WriteCase test, bool cycle_bound) {
  fixture::Program source;
  source.word(test.instruction);
  source.word(move_r1);
  Machine machine(source);
  const auto old = machine.core->registers().system.mapping;
  const auto next = changed(old, test);
  fixture::Program destination(entry + 4, (next.control & 0x80) != 0);
  destination.word(move_r1);
  std::ranges::copy(destination.bytes(), machine.code.begin() + 4);
  const std::array old_spans{armv5::CodeSpan{
      .address = entry, .bytes = std::span(source.bytes()).first(4), .processor_modes = {}}};
  const std::array new_spans{
      armv5::CodeSpan{.address = entry + 4, .bytes = destination.bytes(), .processor_modes = {}}};
  const std::array old_entries{entry};
  const std::array new_entries{entry + 4};
  const std::array variants{
      armv5::CodeVariant{.id = 0, .spans = old_spans, .entries = old_entries, .mapping = old},
      armv5::CodeVariant{.id = 1, .spans = new_spans, .entries = new_entries, .mapping = next}};
  const std::array transfers{
      armv5::CodeTransition{.source_variant = 0, .destination_variant = 1, .address = entry + 4}};
  const armv5::CodeImage image{.identity = "authored-cp15-context-transfer",
                               .variants = variants,
                               .layouts = {},
                               .transitions = transfers,
                               .interrupt_vectors = {}};
  const auto native = machine.compile(image, Machine::Layout::all);
  auto registers = machine.core->registers();
  registers.r[0] = test.input;
  machine.core->set_registers(registers);
  (void)machine.core->install(native, 0);
  auto expected = machine.core->state();
  expected.registers.system.mapping = next;
  expected.registers.r[15] = entry + 4;
  expected.progress = {.instructions = 1, .base_cycles = 1};
  expected.execution = armv5::ExecutionState::transfer;
  expected.transition_pending = true;
  expected.transition = transfers[0];
  const auto result = cycle_bound ? machine.core->run_until(1) : machine.core->run(8);
  fixture::require(machine.core->state() == expected && result.progress == expected.progress,
                   "MCR canonicalizes exactly one instruction and revokes through the declared "
                   "context transition");
  const auto pending = machine.core->save();
  const auto waiting = machine.core->run(1);
  fixture::require(waiting.execution == armv5::ExecutionState::transfer &&
                       waiting.progress == armv5::Progress{} && machine.core->state() == expected,
                   "mapping transition waits for the matching installed context without progress");
  (void)machine.core->install(native, 1);
  fixture::require(
      machine.core->state() == expected,
      "context installation does not change guest state/time or consume the transition");
  (void)machine.core->run(1);
  auto after = expected;
  after.registers.r[1] = 5;
  after.registers.r[15] = entry + 8;
  after.execution = armv5::ExecutionState::ready;
  after.transition_pending = false;
  after.progress = {.instructions = 2, .base_cycles = 2};
  fixture::require(machine.core->state() == after,
                   "new context executes the destination with the correct byte order");
  machine.core->restore(pending);
  fixture::require(machine.core->state() == expected,
                   "pending CP15 transition is part of snapshot state");
  bool refused = false;
  try {
    (void)machine.core->install(native, 0);
  } catch (const std::runtime_error &error) {
    fixture::contains(error.what(), "CP15 mapping differs");
    refused = true;
  }
  fixture::require(refused && machine.core->state() == expected,
                   "old context cannot be installed after a mapping write");
  (void)machine.core->install(native, 1);
  (void)machine.core->run(1);
  fixture::require(machine.core->state() == after,
                   "restored mapping transition has no residue from later execution");
}

void ordinary_write(bool change, bool predicate) {
  fixture::Program code;
  constexpr std::uint32_t mcr_table = 0xee020f10;
  constexpr std::uint32_t mcr_control = 0xee010f10;
  auto word = change ? mcr_table : mcr_control;
  if (predicate) { word &= instruction_fields; }
  code.word(word);
  code.word(move_r1);
  Machine machine(code);
  const std::array spans{
      armv5::CodeSpan{.address = entry, .bytes = code.bytes(), .processor_modes = {}}};
  const std::array entries{entry};
  const std::array variants{armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries}};
  const armv5::CodeImage image{.identity = "authored-cp15-write-boundary",
                               .variants = variants,
                               .layouts = {},
                               .transitions = {},
                               .interrupt_vectors = {}};
  const auto native = machine.compile(image, Machine::Layout::all);
  auto expected = machine.core->registers();
  expected.r[0] = change ? raw_table : 0;
  machine.core->set_registers(expected);
  (void)machine.core->install(native, 0);
  const auto result = machine.core->run(2);
  const bool revoked = change && !predicate;
  expected.r[15] = entry + (revoked ? 4 : 8);
  if (revoked) {
    expected.system.mapping.translation_table = aligned_table;
  } else {
    expected.r[1] = 5;
  }
  fixture::require(machine.core->registers() == expected &&
                       result.execution == (revoked ? armv5::ExecutionState::fault
                                                    : armv5::ExecutionState::ready) &&
                       result.progress == armv5::Progress{.instructions = revoked ? 1U : 2U,
                                                          .base_cycles = revoked ? 1U : 2U},
                   "unchanged/predicated mapping writes continue; an undeclared change stops at "
                   "its exact boundary");
  if (revoked) {
    fixture::require(result.fault.kind == armv5::Fault::Kind::changed_code &&
                         result.fault.address == entry,
                     "mapping refusal identifies the MCR address");
    fixture::contains(result.fault.reason, "CP15");
  }
}

void unpredictable(std::uint32_t input, bool overlap, bool absent, bool predicate) {
  fixture::Program code;
  code.word(predicate ? mcr_dtc & instruction_fields : mcr_dtc);
  code.word(move_r1);
  Machine machine(code);
  if (absent) {
    std::erase_if(machine.regions, [](const armv5::Memory::Region &region) {
      return region.space == armv5::Memory::Space::dtcm;
    });
    machine.core->set_memory({.regions = machine.regions, .generation = 1});
  }
  auto registers = machine.core->registers();
  if (overlap) { registers.system.mapping.instruction_tcm = Machine::instruction_base | 13U; }
  registers.r[0] = input;
  machine.core->set_registers(registers);
  const std::array spans{
      armv5::CodeSpan{.address = entry, .bytes = code.bytes(), .processor_modes = {}}};
  const std::array entries{entry};
  const std::array variants{armv5::CodeVariant{
      .id = 0, .spans = spans, .entries = entries, .mapping = registers.system.mapping}};
  const armv5::CodeImage image{.identity = "authored-unpredictable-tcm-write",
                               .variants = variants,
                               .layouts = {},
                               .transitions = {},
                               .interrupt_vectors = {}};
  const auto native = machine.compile(image, Machine::Layout::all);
  (void)machine.core->install(native, 0);
  auto expected = machine.core->state();
  const auto result = machine.core->run(2);
  if (predicate) {
    expected.registers.r[1] = 5;
    expected.registers.r[15] = entry + 8;
    expected.progress = {.instructions = 2, .base_cycles = 2};
  } else {
    expected.execution = armv5::ExecutionState::fault;
    expected.fault = result.fault;
    fixture::require(result.fault.kind == armv5::Fault::Kind::unsupported,
                     "unpredictable TCM write is refused before retirement");
  }
  fixture::require(machine.core->state() == expected,
                   "bad TCM enable/alignment/overlap writes have no partial effects, false "
                   "predicates skip validation");
}

void fetch_boundary(bool beginning) {
  fixture::Program main;
  main.word(move_r1);
  Machine machine(main);
  constexpr std::uint32_t itcm_size = 4096;
  const auto start =
      beginning ? Machine::instruction_base - 4 : Machine::instruction_base + itcm_size - 4;
  fixture::Program selected(start);
  selected.word(fixture::data(13, false, 0, 0, true, 1));
  selected.word(fixture::data(13, false, 1, 0, true, 2));
  const auto poison = fixture::data(13, false, beginning ? 1 : 0, 0, true, 9);
  if (beginning) {
    machine.regions[1].address = start;
    fixture::put(std::span(machine.bus_instruction).first(4),
                 fixture::data(13, false, 0, 0, true, 1));
    fixture::put(std::span(machine.bus_instruction).subspan(4, 4), poison);
    fixture::put(std::span(machine.instruction).first(4), fixture::data(13, false, 1, 0, true, 2));
  } else {
    machine.bus_instruction.resize(itcm_size + 4);
    machine.regions[1].bytes = machine.bus_instruction;
    fixture::put(std::span(machine.bus_instruction).subspan(itcm_size - 4, 4), poison);
    fixture::put(std::span(machine.bus_instruction).subspan(itcm_size, 4),
                 fixture::data(13, false, 1, 0, true, 2));
    fixture::put(std::span(machine.instruction).last(4), fixture::data(13, false, 0, 0, true, 1));
  }
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  auto registers = machine.core->registers();
  registers.system.mapping.instruction_tcm = Machine::instruction_base | 13U;
  registers.r[15] = start;
  machine.core->set_registers(registers);
  const std::array spans{
      armv5::CodeSpan{.address = start, .bytes = selected.bytes(), .processor_modes = {}}};
  const std::array entries{start};
  const std::array variants{armv5::CodeVariant{
      .id = 0, .spans = spans, .entries = entries, .mapping = registers.system.mapping}};
  const armv5::CodeImage image{.identity = "authored-tcm-fetch-boundary",
                               .variants = variants,
                               .layouts = {},
                               .transitions = {},
                               .interrupt_vectors = {}};
  const auto native = machine.compile(image, Machine::Layout::all);
  (void)machine.core->install(native, 0);
  const auto result = machine.core->run(2);
  registers.r[0] = 1;
  registers.r[1] = 2;
  registers.r[15] = start + 8;
  fixture::require(machine.core->registers() == registers &&
                       result.execution == armv5::ExecutionState::ready &&
                       result.progress == armv5::Progress{.instructions = 2, .base_cycles = 2},
                   "install splits executable declarations at ITCM overlay boundaries");
  if (beginning) {
    fixture::put(std::span(machine.instruction).first(4), poison);
    machine.core->set_memory({.regions = machine.regions, .generation = 2});
    bool refused = false;
    try {
      (void)machine.core->install(native, 0);
    } catch (const std::runtime_error &error) {
      fixture::contains(error.what(), "installed code bytes differ");
      refused = true;
    }
    fixture::require(refused,
                     "bus bytes cannot hide changed ITCM code inside a crossing declaration");
  }
}

void retained_storage(bool relocate) {
  fixture::Program code;
  code.word(fixture::transfer(false, false, 2, 3, 0));
  code.word(mcr_dtc);
  code.word(fixture::transfer(true, false, 4, 3, 0));
  code.word(fixture::data(4, false, 0, 0, true, 1));
  code.word(mcr_dtc);
  if (relocate) {
    constexpr unsigned encoded_8192 = 0xa02;
    code.word(fixture::data(4, false, 3, 3, true, encoded_8192));
  }
  code.word(fixture::transfer(true, false, 5, 3, 0));
  Machine machine(code);
  constexpr std::uint32_t retained = 42;
  constexpr std::uint32_t bus_word = 0x33333333;
  constexpr std::uint32_t new_base = 0xa000;
  fixture::put(std::span(machine.bus_data).first(4), bus_word);
  auto active = machine.core->registers().system.mapping;
  active.data_tcm = Machine::data_base | 16U | 1U;
  auto disabled = active;
  disabled.data_tcm = (relocate ? new_base : Machine::data_base) | 16U;
  auto reenabled = disabled;
  reenabled.data_tcm |= 1;
  const std::array first{armv5::CodeSpan{
      .address = entry, .bytes = std::span(code.bytes()).first(8), .processor_modes = {}}};
  const std::array middle{armv5::CodeSpan{.address = entry + 8,
                                          .bytes = std::span(code.bytes()).subspan(8, 12),
                                          .processor_modes = {}}};
  const std::array last{armv5::CodeSpan{
      .address = entry + 20, .bytes = std::span(code.bytes()).subspan(20), .processor_modes = {}}};
  const std::array first_entries{entry};
  const std::array middle_entries{entry + 8};
  const std::array last_entries{entry + 20};
  const std::array variants{
      armv5::CodeVariant{.id = 0, .spans = first, .entries = first_entries, .mapping = active},
      armv5::CodeVariant{.id = 1, .spans = middle, .entries = middle_entries, .mapping = disabled},
      armv5::CodeVariant{.id = 2, .spans = last, .entries = last_entries, .mapping = reenabled}};
  const std::array transfers{
      armv5::CodeTransition{.source_variant = 0, .destination_variant = 1, .address = entry + 8},
      armv5::CodeTransition{.source_variant = 1, .destination_variant = 2, .address = entry + 20}};
  const armv5::CodeImage image{.identity = "authored-tcm-disable-relocate-reenable",
                               .variants = variants,
                               .layouts = {},
                               .transitions = transfers,
                               .interrupt_vectors = {}};
  const auto native = machine.compile(image, Machine::Layout::all);
  auto registers = machine.core->registers();
  registers.system.mapping = active;
  registers.r[0] = relocate ? new_base : Machine::data_base;
  registers.r[2] = retained;
  registers.r[3] = Machine::data_base;
  machine.core->set_registers(registers);
  (void)machine.core->install(native, 0);
  const auto first_stop = machine.core->run(16);
  fixture::require(first_stop.execution == armv5::ExecutionState::transfer &&
                       first_stop.progress == armv5::Progress{.instructions = 2, .base_cycles = 2},
                   "TCM disable completes after the preceding store");
  (void)machine.core->install(native, 1);
  const auto second_stop = machine.core->run(16);
  fixture::require(second_stop.execution == armv5::ExecutionState::transfer &&
                       second_stop.progress ==
                           armv5::Progress{.instructions = 3, .base_cycles = 3} &&
                       machine.core->registers().r[4] == bus_word,
                   "disabled TCM exposes bus storage until re-enabled");
  (void)machine.core->install(native, 2);
  const auto result = machine.core->run(relocate ? 2 : 1);
  fixture::require(
      result.execution == armv5::ExecutionState::ready &&
          machine.core->registers().r[5] == retained &&
          machine.core->registers().r[3] == (relocate ? new_base : Machine::data_base) &&
          machine.core->state().progress == armv5::Progress{.instructions = relocate ? 7U : 6U,
                                                            .base_cycles = relocate ? 7U : 6U} &&
          fixture::get(std::span(machine.data).first(4)) == retained &&
          fixture::get(std::span(machine.bus_data).first(4)) == bus_word,
      "TCM disable/base relocation/re-enable retains consumer bytes and rebuilds direct bindings");
}

void context_selection() {
  fixture::Program code;
  code.word(mcr_dtc);
  code.word(move_r1);
  Machine machine(code);
  const auto initial = machine.core->registers().system.mapping;
  auto first = initial;
  first.data_tcm = Machine::data_base | 16U | 1U;
  constexpr std::uint32_t alternative_base = 0xa000;
  constexpr std::uint32_t undeclared_base = 0xc000;
  auto second = initial;
  second.data_tcm = alternative_base | 16U | 1U;
  const std::array source{armv5::CodeSpan{
      .address = entry, .bytes = std::span(code.bytes()).first(4), .processor_modes = {}}};
  const std::array destination{armv5::CodeSpan{
      .address = entry + 4, .bytes = std::span(code.bytes()).last(4), .processor_modes = {}}};
  const std::array source_entries{entry};
  const std::array destination_entries{entry + 4};
  std::array variants{
      armv5::CodeVariant{.id = 0, .spans = source, .entries = source_entries, .mapping = initial},
      armv5::CodeVariant{
          .id = 1, .spans = destination, .entries = destination_entries, .mapping = first},
      armv5::CodeVariant{
          .id = 2, .spans = destination, .entries = destination_entries, .mapping = second}};
  const std::array transfers{
      armv5::CodeTransition{.source_variant = 0, .destination_variant = 2, .address = entry + 4},
      armv5::CodeTransition{.source_variant = 0, .destination_variant = 1, .address = entry + 4}};
  const armv5::CodeImage image{.identity = "authored-runtime-cp15-context-selection",
                               .variants = variants,
                               .layouts = {},
                               .transitions = transfers,
                               .interrupt_vectors = {}};
  const auto native = machine.compile(image, Machine::Layout::all);
  for (const auto id : {1U, 2U}) {
    machine.core->reset(entry);
    auto registers = machine.core->registers();
    registers.r[0] = (id == 1 ? Machine::data_base : alternative_base) | 1U;
    machine.core->set_registers(registers);
    (void)machine.core->install(native, 0);
    const auto stopped = machine.core->run(8);
    fixture::require(stopped.execution == armv5::ExecutionState::transfer &&
                         stopped.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
                         machine.core->state().transition.destination_variant == id,
                     "mapping state selects among explicitly declared alternatives at one PC");
    (void)machine.core->install(native, id);
    (void)machine.core->run(1);
    fixture::require(machine.core->registers().r[1] == 5,
                     "selected context enters its prepared destination");
  }
  machine.core->reset(entry);
  auto registers = machine.core->registers();
  registers.r[0] = undeclared_base | 1U;
  machine.core->set_registers(registers);
  (void)machine.core->install(native, 0);
  const auto missing = machine.core->run(8);
  fixture::require(missing.execution == armv5::ExecutionState::fault &&
                       missing.fault.kind == armv5::Fault::Kind::changed_code &&
                       missing.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
                       !machine.core->state().transition_pending,
                   "undeclared CP15 context faults instead of choosing an unrelated destination");
  auto duplicate = first;
  duplicate.instruction_tcm = 0; // Disabled size bits are not a context discriminator.
  variants[2].mapping = duplicate;
  bool ambiguous = false;
  try {
    (void)armv5::Core::lower(image);
  } catch (const std::runtime_error &error) {
    fixture::contains(error.what(), "ambiguous code transition");
    ambiguous = true;
  }
  fixture::require(
      ambiguous,
      "equivalent destination contexts remain ambiguous even with different disabled size bits");
}
} // namespace

void tcm_lifecycle() {
  context_selection();
  retained_storage(false);
  retained_storage(true);
  fetch_boundary(false);
  fetch_boundary(true);
  constexpr std::array cases{
      WriteCase{
          .field = Field::control, .instruction = 0xee010f10, .input = 0x80, .expected = 0x500f8},
      WriteCase{.field = Field::table,
                .instruction = 0xee020f10,
                .input = 0x1234ffff,
                .expected = 0x1234c000},
      WriteCase{.field = Field::domains,
                .instruction = 0xee030f10,
                .input = 0xa5a5a5a5,
                .expected = 0xa5a5a5a5},
      WriteCase{.field = Field::process,
                .instruction = 0xee0d0f10,
                .input = 0xbfffffff,
                .expected = 0xbe000000},
      WriteCase{.field = Field::instruction,
                .instruction = 0xee090f31,
                .input = 0x4fff,
                .expected = 0x400d},
      WriteCase{.field = Field::data, .instruction = mcr_dtc, .input = 0x8fff, .expected = 0x8011}};
  for (const auto &test : cases) {
    transition(test, false);
    transition(test, true);
  }
  ordinary_write(false, false);
  ordinary_write(true, false);
  ordinary_write(true, true);
  unpredictable(misaligned_dtc, false, false, false);
  unpredictable(overlapping_dtc, true, false, false);
  unpredictable(absent_dtc, false, true, false);
  unpredictable(misaligned_dtc, false, false, true);
}
