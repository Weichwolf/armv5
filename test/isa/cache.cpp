// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/mmu.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {
constexpr std::uint32_t poison_byte = 0xa5;
constexpr std::uint32_t maintenance_entry = 0x1100;
constexpr std::uint32_t second_word = 0x4004;
constexpr std::uint32_t third_word = 0x4008;
constexpr std::uint32_t fourth_word = 0x400c;
constexpr std::uint32_t second_half = 0x4010;
constexpr std::uint32_t second_half_next = 0x4014;
constexpr std::uint32_t high_byte = 0x99;
constexpr std::uint32_t byte_value = 0xcc;
constexpr std::uint32_t half_value = 0xbbcc;
constexpr std::uint32_t remaining_cycles = 21;
constexpr std::uint32_t mode_mask = 0x1f;
constexpr std::uint32_t state_mode_mask = 0x3f;
constexpr std::uint32_t initial = 0x11223344, replacement = 0x99aabbcc, cacheable = 0x00200c3e,
                        load_first = 0xe5901000, store_word = 0xe5802000, load_second = 0xe5903000,
                        clean_mva = 0xee070f3a, invalidate_mva = 0xee070f36,
                        test_clean = 0xee174f7a, load_last = 0xe5905000, clean_bit = 0x40000000;

void writeback(bool big) {
  fixture::Program code(0x1000, big);
  for (const auto word :
       {load_first, store_word, load_second, clean_mva, test_clean, invalidate_mva, load_last}) {
    code.word(word);
  }
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}}, {}, big);
  machine.context.control |= 4;
  machine.first(32, cacheable);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), initial, big);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  machine.core->set_registers(registers);
  const auto first = machine.core->run(1);
  fixture::require(first.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
                       machine.core->registers().r[1] == initial,
                   "DCache initial read fill");
  std::ranges::fill(std::span(machine.data).subspan(0x4000, 32), std::byte{poison_byte});
  const auto backing = machine.data;
  (void)machine.core->run(1);
  fixture::require(machine.data == backing, "write-back hit keeps external storage unchanged");
  const auto dirty = machine.core->save();
  const auto state = machine.core->state();
  (void)machine.core->install(machine.prepared(), 0);
  fixture::require(machine.core->state() == state, "cache reinstall preserves state");
  const auto cleaned = machine.core->run(2);
  auto expected = backing;
  std::ranges::fill(std::span(expected).subspan(0x4000, 16), std::byte{0});
  fixture::put(std::span(expected).subspan(0x4000, 4), replacement, big);
  fixture::require(cleaned.progress == armv5::Progress{.instructions = 2, .base_cycles = 2} &&
                       machine.core->registers().r[3] == replacement && machine.data == expected,
                   "cached read sees dirty value; clean writes only the dirty half-line");
  (void)machine.core->run(3);
  fixture::require(machine.core->registers().r[4] == clean_bit &&
                       machine.core->registers().r[5] == replacement,
                   "test-clean reports clean; invalidation refills actual external bytes");
  machine.core->restore(dirty);
  std::ranges::copy(backing, machine.data.begin());
  fixture::require(machine.core->state() == state, "cache snapshot restores exact guest state");
  (void)machine.core->run(2);
  fixture::require(machine.core->registers().r[3] == replacement && machine.data == expected,
                   "snapshot restores dirty bytes and original physical writeback tags");
}

constexpr std::array all_modes{fixture::user_mode,
                               fixture::fiq_mode,
                               fixture::irq_mode,
                               fixture::supervisor_mode,
                               fixture::abort_mode,
                               fixture::undefined_mode,
                               fixture::system_mode};

void mixed_transfers(bool big, bool fixed, bool back) {
  constexpr std::array words{0xe5901000U,
                             0xe5802004U,
                             0xe5c02001U,
                             0xe1c020b2U,
                             0xe5d04001U,
                             0xe1d050b2U,
                             0xe1d060d1U,
                             0xe1d070f2U,
                             0xe1003092U,
                             0xe1c080d0U,
                             0xe8a0000cU,
                             0xe8b00c00U,
                             0xe1c020f0U,
                             0xe590c000U,
                             0xe1401092U,
                             0xe590d001U};
  constexpr std::uint32_t high_word = 0x80000001;
  constexpr std::uint32_t all_bits = 0xffffffff;
  constexpr std::uint32_t swap_little = 0xbbcccc44;
  constexpr std::uint32_t swap_big = 0x11ccbbcc;
  constexpr std::uint32_t rotated_little = 0xcc99aabb;
  constexpr std::uint32_t rotated_big = 0xccccaabb;
  constexpr std::uint32_t final_big = 0xccaabbcc;
  constexpr std::uint32_t signed_byte = 0xffffffcc;
  constexpr std::uint32_t signed_half = 0xffffbbcc;
  constexpr std::uint32_t saved_flags = 0x98000000;
  constexpr std::uint32_t saved_far = 0x87654321;
  constexpr std::uint32_t saved_status = 0x55;
  fixture::Program code(0x1000, big);
  for (const auto word : words) { code.word(word); }
  fixture::Program maintenance(maintenance_entry, big);
  maintenance.word(clean_mva);
  fixture::MmuMachine machine({{.program = code, .modes = {all_modes.begin(), all_modes.end()}},
                               {.program = maintenance, .modes = {fixture::supervisor_mode}}},
                              {},
                              big);
  machine.context.control |= 4;
  constexpr std::uint32_t through = 0x00200c3a;
  machine.first(32, back ? cacheable : through);
  machine.prepare(fixed);
  for (const auto mode : all_modes) {
    std::ranges::fill(machine.data, std::byte{0});
    fixture::put(std::span(machine.data).subspan(0x4000, 4), initial, big);
    fixture::put(std::span(machine.data).subspan(third_word, 4), high_word, big);
    fixture::put(std::span(machine.data).subspan(fourth_word, 4), all_bits, big);
    machine.core->reset(code.address());
    auto expected = machine.core->registers();
    expected.system.mapping = machine.mapping();
    expected.system.data_fault_status = saved_status;
    expected.system.fault_address = saved_far;
    expected.cpsr = saved_flags | mode;
    expected.r[0] = fixture::MmuMachine::data_virtual;
    expected.r[2] = replacement;
    machine.core->set_registers(expected);
    (void)machine.core->install(machine.prepared(), 0);
    (void)machine.core->run(1);
    std::ranges::fill(std::span(machine.data).subspan(0x4000, 32), std::byte{poison_byte});
    const auto backing = machine.data;
    const auto result = machine.core->run(words.size() - 1);
    expected.r[0] += 16;
    expected.r[1] = big ? high_byte : byte_value;
    expected.r[3] = big ? swap_big : swap_little;
    expected.r[4] = byte_value;
    expected.r[5] = half_value;
    expected.r[6] = signed_byte;
    expected.r[7] = signed_half;
    expected.r[8] = replacement;
    expected.r[9] = replacement;
    expected.r[10] = high_word;
    expected.r[11] = all_bits;
    expected.r[12] = replacement;
    expected.r[13] = big ? rotated_big : rotated_little;
    expected.r[15] = code.pc();
    auto ram = backing;
    if (back) { std::ranges::fill(std::span(ram).subspan(0x4000, 32), std::byte{0}); }
    fixture::put(std::span(ram).subspan(0x4000, 4), replacement, big);
    fixture::put(std::span(ram).subspan(second_word, 4), expected.r[3], big);
    if (back) {
      fixture::put(std::span(ram).subspan(third_word, 4), high_word, big);
      fixture::put(std::span(ram).subspan(fourth_word, 4), all_bits, big);
    }
    fixture::put(std::span(ram).subspan(second_half, 4), big ? final_big : replacement, big);
    fixture::put(std::span(ram).subspan(second_half_next, 4), expected.r[3], big);
    fixture::require(
        result.execution == armv5::ExecutionState::ready &&
            result.progress ==
                armv5::Progress{.instructions = 15, .base_cycles = remaining_cycles} &&
            machine.core->registers() == expected && machine.data == (back ? backing : ram),
        "cached ARM byte/half/signed/word/unaligned/double/multiple/SWP transfers in every mode");
    auto clean_state = expected;
    clean_state.cpsr = saved_flags | fixture::supervisor_mode;
    clean_state.r[0] = fixture::MmuMachine::data_virtual;
    clean_state.r[15] = maintenance.address();
    machine.core->set_registers(clean_state);
    (void)machine.core->run(1);
    clean_state.r[15] = maintenance.pc();
    fixture::require(
        machine.data == ram && machine.core->registers() == clean_state,
        "clean writes both dirty half-lines with exact byte lanes and no register side effects");
  }
}

struct Policy {
  unsigned attributes;
  bool mmu, enabled, big, fixed;
};

void policies(Policy policy) {
  constexpr std::uint32_t invalidate_all = 0xee070f16;
  constexpr std::uint32_t fourth_load = 0xe5904000;
  constexpr std::uint32_t section = 0x00200c32;
  constexpr std::uint32_t second_value = 11;
  constexpr std::uint32_t third_value = 33;
  fixture::Program code(0x1000, policy.big);
  for (const auto word : {store_word, load_first, load_second, invalidate_all, fourth_load}) {
    code.word(word);
  }
  fixture::MmuMachine machine(
      {{.program = code, .modes = {fixture::supervisor_mode}}}, {}, policy.big);
  machine.context.control = (machine.context.control & ~std::uint32_t{5}) | unsigned(policy.mmu) |
                            (unsigned(policy.enabled) << 2);
  machine.first(32, section | (policy.attributes << 2));
  if (!policy.mmu) {
    for (auto &region : machine.regions) {
      if (region.address == fixture::MmuMachine::code_physical + code.address()) {
        region.address = code.address();
      }
      if (region.address == fixture::MmuMachine::data_physical) { region.address = 0x02000000; }
    }
    for (auto &region : machine.layout) {
      if (region.address == fixture::MmuMachine::code_physical + code.address()) {
        region.address = code.address();
      }
      if (region.address == fixture::MmuMachine::data_physical) { region.address = 0x02000000; }
    }
    machine.core->set_memory({.regions = machine.regions, .generation = 1});
  }
  machine.prepare(policy.fixed);
  auto registers = machine.core->registers();
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  machine.core->set_registers(registers);
  (void)machine.core->run(1);
  fixture::require(fixture::get(std::span(machine.data).subspan(0x4000, 4), policy.big) ==
                       replacement,
                   "WB/WT misses do not allocate; external writes complete in order");
  fixture::put(std::span(machine.data).subspan(0x4000, 4), second_value, policy.big);
  (void)machine.core->run(1);
  fixture::require(machine.core->registers().r[1] == second_value,
                   "write miss did not allocate stale data");
  fixture::put(std::span(machine.data).subspan(0x4000, 4), third_value, policy.big);
  (void)machine.core->run(1);
  const bool cached = policy.mmu && policy.enabled && (policy.attributes & 2) != 0;
  fixture::require(machine.core->registers().r[3] == (cached ? second_value : third_value),
                   "M/C and C/B policy matrix, including C overridden when MMU is disabled");
  (void)machine.core->run(2);
  fixture::require(machine.core->registers().r[4] == third_value &&
                       fixture::get(std::span(machine.data).subspan(0x4000, 4), policy.big) ==
                           third_value,
                   "invalidate-all discards cached data without writeback");
}

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

void thumb_transfers(bool big, bool fixed) {
  constexpr std::array<std::uint16_t, 9> words{
      0x6801, 0x6042, 0x6843, 0x7042, 0x7844, 0x8042, 0x8845, 0xc00c, 0xc8c0};
  constexpr std::uint32_t high_word = 0x80000001;
  constexpr std::uint32_t all_bits = 0xffffffff;
  constexpr std::uint32_t status = 0x980000f3;
  fixture::Program code(0x1000, big);
  for (const auto word : words) { code.half(word); }
  fixture::Program maintenance(maintenance_entry, big);
  maintenance.word(clean_mva);
  fixture::MmuMachine machine({{.program = code,
                                .modes = {fixture::supervisor_mode},
                                .instruction_set = armv5::CodeSpan::InstructionSet::thumb},
                               {.program = maintenance,
                                .modes = {fixture::supervisor_mode},
                                .instruction_set = armv5::CodeSpan::InstructionSet::arm}},
                              {},
                              big);
  machine.context.control |= 4;
  machine.first(32, cacheable);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), initial, big);
  fixture::put(std::span(machine.data).subspan(third_word, 4), high_word, big);
  fixture::put(std::span(machine.data).subspan(fourth_word, 4), all_bits, big);
  machine.prepare(fixed);
  auto expected = machine.core->registers();
  expected.cpsr = status;
  expected.r[0] = fixture::MmuMachine::data_virtual;
  expected.r[2] = replacement;
  machine.core->set_registers(expected);
  (void)machine.core->run(1);
  std::ranges::fill(std::span(machine.data).subspan(0x4000, 32), std::byte{poison_byte});
  const auto backing = machine.data;
  const auto result = machine.core->run(words.size() - 1);
  expected.r[0] += 16;
  expected.r[1] = initial;
  expected.r[3] = replacement;
  expected.r[4] = byte_value;
  expected.r[5] = half_value;
  expected.r[6] = high_word;
  expected.r[7] = all_bits;
  expected.r[15] = code.pc();
  fixture::require(
      machine.core->registers() == expected && machine.data == backing &&
          result.progress == armv5::Progress{.instructions = 8, .base_cycles = 10},
      "Thumb byte/half/word/STM/LDM operate on cached bytes with exact writeback and flags");
  expected.cpsr &= ~std::uint32_t{0x20};
  expected.r[0] = fixture::MmuMachine::data_virtual;
  expected.r[15] = maintenance.address();
  machine.core->set_registers(expected);
  (void)machine.core->run(1);
  auto ram = backing;
  fixture::put(std::span(ram).subspan(0x4000, 4), replacement, big);
  fixture::put(std::span(ram).subspan(second_word, 4), replacement, big);
  fixture::put(std::span(ram).subspan(third_word, 4), high_word, big);
  fixture::put(std::span(ram).subspan(fourth_word, 4), all_bits, big);
  expected.r[15] = maintenance.pc();
  fixture::require(machine.data == ram && machine.core->registers() == expected,
                   "ARM maintenance cleans exactly the dirty half-line left by Thumb");
}

void permission_hit(bool forced, bool writing) {
  constexpr std::uint32_t privileged = 0x0020043e;
  constexpr std::uint32_t ordinary = 0xe5902000;
  constexpr std::uint32_t user_load = 0xe4b02004;
  constexpr std::uint32_t user_store = 0xe4a02004;
  constexpr std::uint32_t return_abort = 0xe25ef004;
  constexpr std::uint32_t source_status = 0x980000d3;
  constexpr std::uint32_t old_status = 0x55;
  constexpr std::uint32_t old_instruction_status = 0xaa;
  constexpr std::uint32_t saved_far = 0x87654321;
  constexpr std::uint32_t permission_status = 0x1d;
  constexpr std::uint32_t target = 0x1004;
  constexpr std::uint32_t abort_link = 0x100c;
  fixture::Program code;
  code.word(load_first);
  auto operation = ordinary;
  if (forced) { operation = writing ? user_store : user_load; }
  code.word(operation);
  fixture::Program handler(0x10);
  handler.word(return_abort);
  fixture::MmuMachine machine(
      {{.program = code, .modes = {fixture::supervisor_mode, fixture::user_mode}},
       {.program = handler, .modes = {fixture::abort_mode}}},
      {0x10});
  machine.context.control |= 4;
  machine.first(32, privileged);
  machine.entries.push_back(target);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), initial);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.cpsr = source_status;
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  registers.system.data_fault_status = old_status;
  registers.system.instruction_fault_status = old_instruction_status;
  registers.system.fault_address = saved_far;
  machine.core->set_registers(registers);
  (void)machine.core->run(1);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), replacement);
  registers = machine.core->registers();
  if (!forced) {
    registers.cpsr = (source_status & ~std::uint32_t{mode_mask}) | fixture::user_mode;
  }
  machine.core->set_registers(registers);
  auto expected = machine.core->state();
  expected.registers.r[14] = abort_link;
  expected.registers.r[15] = 0x10;
  expected.registers.cpsr =
      (registers.cpsr & ~std::uint32_t{state_mode_mask}) | fixture::abort_mode | 0x80;
  expected.registers.spsr = registers.cpsr;
  expected.registers.system.data_fault_status = permission_status;
  expected.registers.system.fault_address = registers.r[0];
  expected.banks.abort[1] = abort_link;
  expected.banks.spsr[3] = registers.cpsr;
  expected.progress.base_cycles += 3;
  const auto backing = machine.data;
  const auto result = machine.core->run_until(machine.core->state().progress.base_cycles + 1);
  fixture::require(machine.core->state() == expected && machine.data == backing &&
                       result.progress == armv5::Progress{.instructions = 0, .base_cycles = 3},
                   "cached hit still enforces User/AP permissions, including privileged LDRT/STRT "
                   "before writeback");
  registers.cpsr = source_status;
  registers.r[15] = code.address();
  machine.core->set_registers(registers);
  (void)machine.core->run(1);
  fixture::require(machine.core->registers().r[1] == initial,
                   "denied cached access did not modify the existing cache line");
}

void predication() {
  constexpr std::uint32_t invalidate_eq = 0x0e070f16;
  constexpr std::uint32_t test_clean_eq = 0x0e17ff7a;
  constexpr std::uint32_t lockdown_eq = 0x0e092f10;
  constexpr std::uint32_t saved_status = 0x980000d3;
  fixture::Program code;
  for (const auto word :
       {load_first, store_word, invalidate_eq, test_clean_eq, lockdown_eq, load_second}) {
    code.word(word);
  }
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}});
  machine.context.control |= 4;
  machine.first(32, cacheable);
  machine.prepare();
  auto expected = machine.core->registers();
  expected.cpsr = saved_status;
  expected.r[0] = fixture::MmuMachine::data_virtual;
  expected.r[2] = replacement;
  machine.core->set_registers(expected);
  const auto ram = machine.data;
  const auto result = machine.core->run(6);
  expected.r[3] = replacement;
  expected.r[15] = code.pc();
  fixture::require(machine.core->registers() == expected && machine.data == ram &&
                       result.progress == armv5::Progress{.instructions = 6, .base_cycles = 6},
                   "false cache maintenance and lockdown predicates preserve dirty bytes, flags, "
                   "lock bits and memory");
}

} // namespace

void cache_instructions() {
  predication();
  writeback(false);
  writeback(true);
  permission_hit(false, false);
  permission_hit(true, false);
  permission_hit(true, true);
  for (const bool big : {false, true}) {
    lockdown_registers(big);
    for (const bool fixed : {false, true}) {
      thumb_transfers(big, fixed);
      mixed_transfers(big, fixed, false);
      mixed_transfers(big, fixed, true);
      for (unsigned attributes = 0; attributes < 4; ++attributes) {
        for (const bool mmu : {false, true}) {
          for (const bool enabled : {false, true}) {
            policies({.attributes = attributes,
                      .mmu = mmu,
                      .enabled = enabled,
                      .big = big,
                      .fixed = fixed});
          }
        }
      }
    }
  }
}
