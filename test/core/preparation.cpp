// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "consumer.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
template <class Operation> void rejected(Operation operation, std::string_view reason) {
  try {
    operation();
  } catch (const std::runtime_error &error) {
    fixture::contains(error.what(), reason);
    return;
  }
  throw std::runtime_error("expected preparation/installation refusal");
}

void bounds_and_snapshot() {
  fixture::Program program;
  program.word(fixture::data(4, false, 0, 0, true, 1));
  program.word(fixture::data(4, false, 1, 1, true, 2));
  program.word(fixture::branch(program.pc(), {.address = program.address()}));
  fixture::Machine machine(program);
  machine.prepare();
  const auto initial = machine.core->save();
  for (unsigned budget = 0; budget < 64; ++budget) {
    machine.core->restore(initial);
    const auto result = machine.run(budget);
    const auto registers = machine.core->registers();
    const auto complete = budget / 3;
    const auto tail = budget % 3;
    fixture::require(result.progress.instructions == budget &&
                         registers.r[0] == complete + unsigned(tail >= 1) &&
                         registers.r[1] == 2 * (complete + unsigned(tail >= 2)),
                     "exact run budget inside a natural block");
    fixture::require(result.progress.base_cycles == complete * 5 + tail,
                     "per-block base accounting");
  }
  machine.core->restore(initial);
  const auto first = machine.run(1);
  const auto middle = machine.core->save();
  constexpr unsigned later_instructions = 17;
  (void)machine.run(later_instructions);
  machine.core->restore(middle);
  fixture::require(machine.core->state().progress == first.progress &&
                       machine.core->registers().r[1] == 0,
                   "snapshot restores an interior instruction boundary");
  const auto bounded = machine.core->run_until(4);
  fixture::require(bounded.progress == armv5::Progress{.instructions = 2, .base_cycles = 4},
                   "deadline stops at first complete boundary beyond bound");
  const auto unchanged = machine.core->state();
  (void)machine.core->run_until(4);
  fixture::require(machine.core->state() == unchanged, "past deadline changed state");
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  fixture::require(machine.core->state() == unchanged, "memory rebind changed CPU/time");
  const auto fault = machine.core->run(1);
  fixture::require(fault.fault.kind == armv5::Fault::Kind::unprepared &&
                       fault.progress.instructions == 0,
                   "memory rebind did not revoke binding");
  machine.core->restore(initial);
  const auto before = machine.core->state();
  (void)machine.core->install(machine.prepared(), 0);
  fixture::require(machine.core->state() == before, "install changed CPU/time");
  (void)machine.run(2);
}

void artifact_cache() {
  fixture::Program program;
  program.word(fixture::data(13, false, 0, 0, true, 7));
  program.word(fixture::branch(program.pc(), {.address = program.pc()}));
  fixture::Machine machine(program);
  const auto image = machine.image();
  const auto semantic = armv5::Core::lower(image);
  const auto loaded = armv5::Core::load(image, {}, semantic.artifact());
  fixture::require(std::ranges::equal(semantic.bitcode(), loaded.bitcode()) &&
                       std::ranges::equal(semantic.exports(), loaded.exports()),
                   "cache image did not reconstruct preparation");
  fixture::require(std::ranges::equal(semantic.artifact(), loaded.artifact()),
                   "artifact identity changed on load");
  auto corrupt = std::vector<std::byte>(semantic.artifact().begin(), semantic.artifact().end());
  corrupt.back() ^= std::byte{1};
  rejected([&] { (void)armv5::Core::load(image, {}, corrupt); }, "checksum mismatch");
  rejected(
      [&] {
        (void)armv5::Core::load(image, {.big_endian = true, .callbacks = {}}, semantic.artifact());
      },
      "digest mismatch");
  rejected([&] { (void)armv5::Core::load(image, {}, semantic.artifact().first(8)); }, "artifact");
  rejected([&] { (void)armv5::Core::bind(loaded, {}); }, "missing native export");
  machine.code[0] ^= std::byte{1};
  rejected([&] { (void)armv5::Core::load(machine.image(), {}, semantic.artifact()); },
           "digest mismatch");
  machine.code[0] ^= std::byte{1};
  machine.prepare();
  const auto before = machine.core->state();
  machine.code[0] ^= std::byte{1};
  rejected([&] { (void)machine.core->install(machine.prepared(), 0); }, "code bytes differ");
  fixture::require(machine.core->state() == before, "failed install changed state");
  fixture::require(machine.core->run(1).fault.kind == armv5::Fault::Kind::unprepared,
                   "failed install retained a binding");
}

struct Device {
  armv5::Core *core = nullptr;
  unsigned reads = 0;
  bool refuse = false, yield = false, reentry = false;

  struct Read {
    std::uint32_t address;
    unsigned width;
    bool operator==(const Read &) const = default;
  };

  std::vector<Read> addresses;

  static bool
  read(void *opaque, std::uint32_t address, unsigned width, std::uint32_t &value) noexcept {
    auto &self = *static_cast<Device *>(opaque);
    ++self.reads;
    self.addresses.push_back({.address = address, .width = width});
    if (width != 4) { return false; }
    if (self.core != nullptr) {
      const auto state = self.core->state();
      if (state.registers.r[1] != 1 || state.progress.instructions != 1) { return false; }
      try {
        (void)self.core->run(1);
      } catch (const std::logic_error &) { self.reentry = true; }
      if (self.yield) { self.core->request_yield(); }
    }
    value = address;
    return !self.refuse;
  }
};

void callbacks_and_faults() {
  fixture::Program program;
  program.word(fixture::data(4, false, 1, 1, true, 1));
  program.word(fixture::transfer(true, false, 2, 0, 0));
  program.word(fixture::data(4, false, 3, 3, true, 1));
  program.word(fixture::branch(program.pc(), {.address = program.pc()}));
  Device device;
  fixture::Machine machine(program, 0x10000, {.owner = &device, .read = &Device::read});
  device.core = machine.core.get();
  device.yield = true;
  device.addresses.reserve(16);
  machine.prepare();
  auto registers = machine.core->registers();
  constexpr std::uint32_t io_address = 0xf0000000;
  registers.r[0] = io_address;
  machine.registers(registers);
  const auto result = machine.core->run(3);
  fixture::require(
      result.execution == armv5::ExecutionState::yielded && result.progress.instructions == 2 &&
          machine.core->registers().r[2] == io_address && machine.core->registers().r[3] == 0,
      "callback yield must complete current instruction only");
  fixture::require(device.addresses ==
                       std::vector<Device::Read>{{.address = io_address, .width = 4}},
                   "callback address/width");
  fixture::require(device.reads == 1 && device.reentry,
                   "callback prefix inspection/reentry rejection");
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[3] == 1,
                   "yield continuation skipped next instruction");
  machine.core->reset(program.address());
  registers = machine.core->registers();
  registers.r[0] = io_address;
  machine.registers(registers);
  device.refuse = true;
  device.yield = false;
  const auto fault = machine.core->run(3);
  fixture::require(fault.fault.kind == armv5::Fault::Kind::memory &&
                       fault.fault.address == io_address && fault.progress.instructions == 1 &&
                       machine.core->registers().r[15] == program.address() + 4,
                   "callback fault exact prefix/context");
  const auto saved = machine.core->save();
  machine.core->reset();
  machine.core->restore(saved);
  fixture::require(machine.core->run(1).progress.instructions == 0 &&
                       machine.core->state().fault == fault.fault,
                   "fault snapshot or latched fault lost");
  fixture::Program end;
  end.word(fixture::data(13, false, 0, 0, true, 1));
  fixture::Machine last(end);
  last.prepare();
  (void)last.run(1);
  fixture::require(last.core->run(1).fault.kind == armv5::Fault::Kind::unprepared,
                   "undeclared fallthrough must fault on next admission");
}

void aliases_and_transitions() {
  fixture::Program first;
  first.word(fixture::transfer(false, false, 1, 0, 0));
  first.word(fixture::data(13, false, 2, 0, true, 1));
  first.word(fixture::branch(first.pc(), {.address = first.pc()}));
  auto old_code = first.bytes();
  auto new_code = first.bytes();
  const auto replacement = fixture::data(13, false, 2, 0, true, 9);
  fixture::put(std::span(new_code).subspan(4, 4), replacement);
  std::vector<std::byte> storage = old_code;
  std::array regions{armv5::Memory::Region{.address = 0x1000,
                                           .bytes = storage,
                                           .readable = true,
                                           .writable = true,
                                           .executable = true},
                     armv5::Memory::Region{.address = 0x8000,
                                           .bytes = storage,
                                           .readable = true,
                                           .writable = true,
                                           .executable = false}};
  const std::array old_spans{
      armv5::CodeSpan{.address = 0x1000, .bytes = old_code, .processor_modes = {}}};
  const std::array new_spans{
      armv5::CodeSpan{.address = 0x1000, .bytes = new_code, .processor_modes = {}}};
  const std::array<std::uint32_t, 1> old_entries{0x1000};
  const std::array<std::uint32_t, 1> new_entries{0x1004};
  const std::array variants{
      armv5::CodeVariant{.id = 0, .spans = old_spans, .entries = old_entries},
      armv5::CodeVariant{.id = 1, .spans = new_spans, .entries = new_entries}};
  const std::array layouts_data{armv5::RegionLayout{.address = 0x1000,
                                                    .size = storage.size(),
                                                    .readable = true,
                                                    .writable = true,
                                                    .executable = true},
                                armv5::RegionLayout{.address = 0x8000,
                                                    .size = storage.size(),
                                                    .readable = true,
                                                    .writable = true,
                                                    .executable = false}};
  const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = layouts_data},
                           armv5::VariantLayout{.variant = 1, .regions = layouts_data}};
  const std::array transitions{
      armv5::CodeTransition{.source_variant = 0, .destination_variant = 1, .address = 0x1004}};
  const armv5::CodeImage image{.identity = "authored-code-variant",
                               .variants = variants,
                               .layouts = layouts,
                               .transitions = transitions,
                               .interrupt_vectors = {}};
  const auto native = armv5::Core::compile(image);
  armv5::Core core({.regions = regions, .generation = 0});
  core.reset(0x1000);
  auto registers = core.registers();
  registers.r[0] = regions[1].address + 4;
  registers.r[1] = replacement;
  core.set_registers(registers);
  (void)core.install(native, 0);
  const auto before = core.save();
  const auto result = core.run(2);
  fixture::require(result.execution == armv5::ExecutionState::transfer &&
                       result.progress.instructions == 1 && storage == new_code &&
                       core.registers().r[2] == 0,
                   "alias write must revoke before changed successor");
  const auto pending = core.save();
  const auto state = core.state();
  (void)core.install(native, 1);
  fixture::require(core.state() == state, "variant installation consumed pending transfer");
  fixture::require(core.run(1).execution == armv5::ExecutionState::ready &&
                       core.registers().r[2] == 9,
                   "declared variant continuation");
  core.restore(before);
  fixture::require(!core.state().transition_pending, "restore retained later transfer residue");
  core.restore(pending);
  fixture::require(core.state() == state, "restore lost pending transfer or progress");
  fixture::require(core.run(1).progress.instructions == 1 && core.registers().r[2] == 9,
                   "restored pending continuation");
}
}

int main() {
  try {
    bounds_and_snapshot();
    artifact_cache();
    callbacks_and_faults();
    aliases_and_transitions();
    consumer_binding();
    interrupts();
    call_returns();
    counted_loops();
    system_lifecycle();
    tcm_lifecycle();
    control_lifecycle();
    mmu_lifecycle();
    std::cout << "Installed image lifecycle, cache, callbacks and alias transitions passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
