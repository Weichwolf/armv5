// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/mmu.hpp"
#include "support/modes.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace {
constexpr std::array all_modes{fixture::user_mode,
                               fixture::fiq_mode,
                               fixture::irq_mode,
                               fixture::supervisor_mode,
                               fixture::abort_mode,
                               fixture::undefined_mode,
                               fixture::system_mode};

void lockdown_registers(bool big) {
  constexpr std::uint32_t read_d = 0xee191f10;
  constexpr std::uint32_t read_i = 0xee192f30;
  constexpr std::uint32_t write_d = 0xee091f10;
  constexpr std::uint32_t write_i = 0xee092f30;
  constexpr std::uint32_t read_back_d = 0xee193f10;
  constexpr std::uint32_t read_back_i = 0xee195f30;
  constexpr std::uint32_t all_bits = 0xffffffff;
  constexpr std::uint32_t input = 0x12345678;
  constexpr std::uint32_t expected_d = 0xffff;
  constexpr std::uint32_t expected_i = 0xfff8;
  constexpr std::uint32_t reset_lockdown = 0xfff0;
  fixture::Program code(0x1000, big);
  code.word(read_d);
  code.word(read_i);
  code.word(fixture::data(12, false, 1, 1, true, 15));
  code.word(fixture::data(12, false, 2, 2, true, 8));
  for (const auto word : {write_d, write_i, read_back_d, read_back_i}) { code.word(word); }
  fixture::MmuMachine machine(
      {{.program = code, .modes = {all_modes.begin() + 1, all_modes.end()}}}, {}, big);
  machine.prepare();
  constexpr std::uint32_t preserved_flags = 0x98000000;
  for (const auto mode : std::span(all_modes).subspan(1)) {
    machine.core->reset(code.address());
    auto expected = machine.core->registers();
    expected.system.mapping = machine.mapping();
    expected.cpsr = preserved_flags | mode;
    expected.r[0] = all_bits;
    expected.r[4] = input;
    machine.core->set_registers(expected);
    (void)machine.core->install(machine.prepared(), 0);
    (void)machine.core->run(2);
    expected.r[1] = reset_lockdown;
    expected.r[2] = reset_lockdown;
    expected.r[15] = code.address() + 8;
    fixture::require(machine.core->registers() == expected,
                     "MRC reads the required-one fields and unlocked ways from reset");
    (void)machine.core->run(6);
    expected.r[1] = expected_d;
    expected.r[2] = expected_i;
    expected.r[3] = expected_d;
    expected.r[5] = expected_i;
    expected.r[15] = code.pc();
    expected.system.data_cache_lockdown = expected_d;
    expected.system.instruction_cache_lockdown = expected_i;
    fixture::require(
        machine.core->registers() == expected,
        "legal cache-lockdown read-modify-write preserves SBO fields in every privileged mode");
    machine.core->reset(code.address());
    fixture::require(machine.core->registers().system.data_cache_lockdown == reset_lockdown &&
                         machine.core->registers().system.instruction_cache_lockdown ==
                             reset_lockdown,
                     "reset unlocks all cache ways and retains the required-one register fields");
  }
}

void maintenance(bool big, bool fixed) {
  // Every documented data maintenance operation, plus instruction prefetch.
  constexpr std::array words{0xee070f16U,
                             0xee070f36U,
                             0xee070f56U,
                             0xee070f3aU,
                             0xee070f5aU,
                             0xee070f3eU,
                             0xee070f5eU,
                             0xee070f9aU,
                             0xee070f3dU,
                             0xee173f7aU,
                             0xee17ff7eU};
  constexpr std::uint32_t initial_flags = 0x98000000;
  constexpr std::uint32_t clean_flags = 0x48000000;
  constexpr unsigned pc_read_extra_cycles = 3; // ARM9E-S DDI 0165B table 8-2.
  constexpr std::uint32_t caches = 0x5004;
  constexpr std::uint32_t unmapped = 0xffffffff;
  fixture::Program code(0x1000, big);
  for (const auto word : words) { code.word(word); }
  fixture::MmuMachine machine(
      {{.program = code, .modes = {all_modes.begin() + 1, all_modes.end()}}}, {}, big);
  machine.context.control |= caches;
  machine.prepare(fixed);
  for (const auto mode : std::span(all_modes).subspan(1)) {
    machine.core->reset(code.address());
    auto expected = machine.core->registers();
    expected.cpsr = initial_flags | mode;
    expected.system.mapping = machine.mapping();
    expected.r[0] = unmapped; // Maintenance never touches this undeclared address.
    machine.core->set_registers(expected);
    (void)machine.core->install(machine.prepared(), 0);
    const auto ram = machine.data;
    const auto result = machine.core->run(words.size());
    expected.r[3] = 0x40000000;
    expected.r[15] = code.pc();
    expected.cpsr = clean_flags | mode;
    fixture::require(machine.core->registers() == expected && machine.data == ram &&
                         result.progress ==
                             armv5::Progress{.instructions = words.size(),
                                             .base_cycles = words.size() + pc_read_extra_cycles},
                     "data cache maintenance/prefetch has no stored content; test-clean reports "
                     "clean, MRC PC changes only NZCV and all controls remain readable");
  }
}

void storage(bool big, bool fixed, unsigned attributes) {
  constexpr std::uint32_t load_second = 0xe5902000;
  constexpr std::uint32_t load_third = 0xe5903000;
  constexpr std::uint32_t caches = 0x5004;
  constexpr std::uint32_t initial = 0x11223344;
  constexpr std::uint32_t replacement = 0x55667788;
  constexpr std::uint32_t after_snapshot = 0x99aabbcc;
  constexpr std::uint32_t descriptor = 0x00200c32;
  fixture::Program code(0x1000, big);
  code.word(fixture::transfer(false, false, 1, 0, 0));
  code.word(load_second);
  code.word(load_third);
  fixture::MmuMachine machine(
      {{.program = code, .modes = {all_modes.begin(), all_modes.end()}}}, {}, big);
  machine.context.control |= caches;
  machine.first(32, descriptor | (attributes << 2));
  machine.prepare(fixed);
  for (const auto mode : all_modes) {
    machine.core->reset(code.address());
    auto expected = machine.core->registers();
    expected.system.mapping = machine.mapping();
    expected.cpsr = mode;
    expected.r[0] = fixture::MmuMachine::data_virtual;
    expected.r[1] = initial;
    machine.core->set_registers(expected);
    (void)machine.core->install(machine.prepared(), 0);
    (void)machine.core->run(1);
    fixture::require(fixture::get(std::span(machine.data).subspan(0x4000, 4), big) == initial,
                     "C/I/RR and page C/B never retain private data writes");
    fixture::put(std::span(machine.data).subspan(0x4000, 4), replacement, big);
    (void)machine.core->run(1);
    const auto saved = machine.core->save();
    fixture::put(std::span(machine.data).subspan(0x4000, 4), after_snapshot, big);
    machine.core->restore(saved);
    (void)machine.core->run(1);
    expected.r[2] = replacement;
    expected.r[3] = after_snapshot;
    expected.r[15] = code.pc();
    fixture::require(machine.core->registers() == expected &&
                         machine.core->state().progress ==
                             armv5::Progress{.instructions = 3, .base_cycles = 3},
                     "reads use consumer storage; snapshots restore CPU/TLB state without "
                     "capturing cache bytes or reverting external memory");
  }
}
} // namespace

void cache_instructions() {
  for (const bool big : {false, true}) {
    lockdown_registers(big);
    for (const bool fixed : {false, true}) {
      maintenance(big, fixed);
      for (unsigned attributes = 0; attributes < 4; ++attributes) {
        storage(big, fixed, attributes);
      }
    }
  }
}
