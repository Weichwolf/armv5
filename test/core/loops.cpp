// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {
constexpr std::uint32_t entry = 0x1000, source = 0x4000, destination = 0x4100,
                        callback_source = 0x20000, initial_flags = 0x980000d3,
                        signed_min = 0x80000000, source_seed = 0x12345678, non_flags = 0x0fffffff;
constexpr std::array<std::uint32_t, 8> words{
    1, 0xffffffff, signed_min, signed_min, source_seed, 3, 5, 7};
constexpr unsigned budget_limit = 20, cycle_limit = 30;
constexpr auto unbounded = std::numeric_limits<std::uint64_t>::max();

struct RunCase {
  std::uint32_t count;
  std::uint64_t budget = unbounded, deadline = unbounded;
  std::uint32_t input_base = source;
};

fixture::Program program() {
  fixture::Program code;
  code.word(fixture::transfer(true, false, 3, 0, 4, false));
  code.word(fixture::data(4, false, 4, 4, false, 3));
  code.word(fixture::transfer(false, false, 4, 1, 4, false));
  code.word(fixture::data(2, true, 2, 2, true, 1));
  code.word(fixture::branch(code.pc(), {.address = entry}, 1));
  code.word(fixture::data(13, false, 5, 0, true, 3));
  code.word(fixture::branch(code.pc(), {.address = code.pc()}));
  return code;
}

// Independent expectations for this authored seven-word program. No production
// decoder, arithmetic helper or alternate executor contributes to the oracle.
void retire(armv5::State &state, std::span<std::byte> ram, std::uint32_t input_base) {
  auto &reg = state.registers;
  const auto pc = reg.r[15];
  unsigned cycles = 1;
  reg.r[15] += 4;
  switch ((pc - entry) / 4) {
    case 0:
      reg.r[3] = words[(reg.r[0] - input_base) / 4];
      reg.r[0] += 4;
      break;
    case 1: reg.r[4] += reg.r[3]; break;
    case 2:
      fixture::put(ram.subspan(reg.r[1] - source, 4), reg.r[4]);
      reg.r[1] += 4;
      break;
    case 3: {
      const auto old = reg.r[2]--;
      reg.cpsr = (reg.cpsr & non_flags) | (reg.r[2] & signed_min);
      if (reg.r[2] == 0) { reg.cpsr |= 0x40000000; }
      if (old != 0) { reg.cpsr |= 0x20000000; }
      if (old == signed_min) { reg.cpsr |= 0x10000000; }
      break;
    }
    case 4:
      if (reg.r[2] != 0) {
        reg.r[15] = entry;
        cycles = 3;
      }
      break;
    case 5: reg.r[5] = 3; break;
    default:
      fixture::require(pc == entry + (6 * 4), "authored loop oracle PC");
      reg.r[15] = pc;
      cycles = 3;
      break;
  }
  ++state.progress.instructions;
  state.progress.base_cycles += cycles;
  state.execution = armv5::ExecutionState::ready;
}

void initialize(fixture::Modes &machine, RunCase test) {
  machine.core->reset(entry);
  std::ranges::fill(machine.ram, std::byte{0});
  for (std::size_t index = 0; index < words.size(); ++index) {
    fixture::put(std::span(machine.ram).subspan(index * 4, 4), words[index]);
  }
  auto registers = machine.core->registers();
  registers.r[0] = test.input_base;
  registers.r[1] = destination;
  registers.r[2] = test.count;
  registers.cpsr = initial_flags;
  machine.core->set_registers(registers);
}

void verify_bound(fixture::Modes &machine, RunCase test) {
  initialize(machine, test);
  auto expected = machine.core->state();
  auto ram = machine.ram;
  while (expected.progress.instructions < test.budget &&
         expected.progress.base_cycles < test.deadline) {
    retire(expected, ram, source);
  }
  const auto result = test.deadline == unbounded ? machine.core->run(test.budget)
                                                 : machine.core->run_until(test.deadline);
  fixture::require(machine.core->state() == expected && machine.ram == ram &&
                       result.progress == expected.progress,
                   "counted loop exact bounds, flags, counter wrap and memory prefix");
  const auto saved = machine.core->save();
  const auto saved_ram = ram;
  for (unsigned next = 0; next < 3; ++next) { retire(expected, ram, source); }
  (void)machine.core->run(3);
  fixture::require(machine.core->state() == expected && machine.ram == ram, "counted loop resume");
  machine.core->restore(saved);
  std::ranges::copy(saved_ram, machine.ram.begin());
  (void)machine.core->run(3);
  fixture::require(machine.core->state() == expected && machine.ram == ram,
                   "counted loop snapshot replay");
}

struct Callback {
  armv5::Core *cpu = nullptr;
  bool refuse;
  std::vector<armv5::State> prefixes;

  static bool
  read(void *owner, std::uint32_t address, unsigned width, std::uint32_t &value) noexcept {
    auto &self = *static_cast<Callback *>(owner);
    if (width != 4 || address < callback_source || address - callback_source >= words.size() * 4) {
      return false;
    }
    self.prefixes.push_back(self.cpu->state());
    const auto index = (address - callback_source) / 4;
    value = words[index];
    if (index == 1) {
      self.cpu->request_yield();
      return !self.refuse;
    }
    return true;
  }
};

void callbacks(bool refuse) {
  Callback callback{.refuse = refuse, .prefixes = {}};
  fixture::Modes machine({{.program = program(), .modes = {fixture::supervisor_mode}}},
                         {},
                         true,
                         0x10000,
                         {.owner = &callback, .read = &Callback::read});
  callback.cpu = machine.core.get();
  initialize(machine, {.count = 3, .input_base = callback_source});
  auto expected = machine.core->state();
  const auto first_prefix = expected;
  auto ram = machine.ram;
  for (unsigned next = 0; next < 5; ++next) { retire(expected, ram, callback_source); }
  const auto second_prefix = expected;
  if (!refuse) { retire(expected, ram, callback_source); }
  const auto result = machine.core->run(15);
  fixture::require(callback.prefixes == std::vector{first_prefix, second_prefix},
                   "loop callbacks observe exact preceding turns and SUBS flags");
  expected.execution = refuse ? armv5::ExecutionState::fault : armv5::ExecutionState::yielded;
  if (refuse) {
    fixture::require(result.fault.kind == armv5::Fault::Kind::memory &&
                         result.fault.address == callback_source + 4,
                     "callback refusal outranks requested yield and does not retire LDR");
    expected.fault = result.fault;
  }
  fixture::require(machine.core->state() == expected && machine.ram == ram,
                   "loop callback stop retains exact architectural prefix");
  if (!refuse) {
    const auto saved = machine.core->save();
    const auto saved_ram = ram;
    retire(expected, ram, callback_source);
    retire(expected, ram, callback_source);
    (void)machine.core->run(2);
    fixture::require(machine.core->state() == expected && machine.ram == ram,
                     "yielded loop resume");
    machine.core->restore(saved);
    std::ranges::copy(saved_ram, machine.ram.begin());
    (void)machine.core->run(2);
    fixture::require(machine.core->state() == expected && machine.ram == ram,
                     "yielded loop snapshot replay");
  }
}

void code_alias() {
  const auto code = program();
  std::vector<std::byte> storage(code.bytes().size() + 4);
  std::ranges::copy(code.bytes(), storage.begin() + 4);
  std::array<std::byte, 32> ram{};
  fixture::put(std::span(ram).first(4), words[0]);
  fixture::put(std::span(ram).subspan(4, 4), words[1]);
  const auto original_ram = ram;
  constexpr std::uint32_t alias_address = 0x8000;
  const std::array regions{
      armv5::Memory::Region{.address = entry,
                            .bytes = std::span(storage).subspan(4),
                            .readable = true,
                            .writable = false,
                            .executable = true},
      armv5::Memory::Region{
          .address = source, .bytes = ram, .readable = true, .writable = true, .executable = false},
      armv5::Memory::Region{.address = alias_address,
                            .bytes = storage,
                            .readable = true,
                            .writable = true,
                            .executable = false}};
  std::vector<armv5::RegionLayout> layout;
  layout.reserve(regions.size());
  for (const auto &region : regions) {
    layout.push_back({.address = region.address,
                      .size = region.bytes.size(),
                      .readable = region.readable,
                      .writable = region.writable,
                      .executable = region.executable});
  }
  const std::array spans{
      armv5::CodeSpan{.address = entry, .bytes = code.bytes(), .processor_modes = {}}};
  const std::array entries{entry};
  const std::array variants{armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries}};
  const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = layout}};
  const armv5::CodeImage image{.identity = "authored-loop-code-alias",
                               .variants = variants,
                               .layouts = layouts,
                               .transitions = {},
                               .interrupt_vectors = {}};
  const auto native = armv5::Core::compile(image);
  armv5::Core core({.regions = regions, .generation = 0});
  core.reset(entry);
  auto registers = core.registers();
  registers.r[0] = source;
  registers.r[1] = alias_address;
  registers.r[2] = 3;
  registers.cpsr = initial_flags;
  core.set_registers(registers);
  (void)core.install(native, 0);
  auto expected = core.state();
  auto expected_storage = storage;
  fixture::put(std::span(expected_storage).first(4), 1);
  fixture::put(std::span(expected_storage).subspan(4, 4), 0);
  expected.registers.r[0] += 8;
  expected.registers.r[1] += 8;
  expected.registers.r[2] = 2;
  expected.registers.r[3] = words[1];
  expected.registers.r[15] = entry + 12;
  constexpr std::uint32_t preceding_subs_status = 0x280000d3;
  expected.registers.cpsr = preceding_subs_status;
  expected.progress = {.instructions = 8, .base_cycles = 10};
  expected.execution = armv5::ExecutionState::fault;
  const auto result = core.run(15);
  fixture::require(result.fault.kind == armv5::Fault::Kind::changed_code &&
                       result.fault.address == alias_address + 4,
                   "code alias revokes a counted loop at its exact store");
  expected.fault = result.fault;
  fixture::require(core.state() == expected && storage == expected_storage && ram == original_ram,
                   "code change after an earlier turn preserves exact state and memory effects");
}
} // namespace

void counted_loops() {
  for (const bool fixed : {false, true}) {
    fixture::Modes machine(
        {{.program = program(), .modes = {fixture::supervisor_mode}}}, {}, fixed);
    for (const auto count : {0U, 1U, 2U, 3U, signed_min, signed_min + 1, 0xffffffffU}) {
      for (unsigned budget = 0; budget <= budget_limit; ++budget) {
        verify_bound(machine, {.count = count, .budget = budget});
      }
      for (unsigned deadline = 0; deadline <= cycle_limit; ++deadline) {
        verify_bound(machine, {.count = count, .deadline = deadline});
      }
    }
  }
  callbacks(false);
  callbacks(true);
  code_alias();
}
