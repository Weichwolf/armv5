// SPDX-License-Identifier: GPL-3.0-only
#include "support/mmu.hpp"

#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {
constexpr std::uint32_t move_zero_three_word = 0xe3a00003;
constexpr std::uint32_t return_abort_word = 0xe25ef004;
constexpr std::uint32_t load_second_word = 0xe5902000;
constexpr std::uint32_t next_pc = 0x1004;
constexpr std::uint32_t target_address = 0x1100;
constexpr std::uint32_t initial_cpsr = 0xd3;
constexpr std::uint32_t data_page_descriptor = 0x00200ff2;
constexpr std::uint32_t maintenance_address = 0x1010;
constexpr std::uint32_t after_maintenance = 0x1014;
constexpr std::uint32_t fine_table_descriptor = 0x8033;
constexpr std::uint32_t coarse_table_descriptor = 0x8031;
constexpr std::uint32_t old_value = 42, new_value = 99, new_section = 0x00300c32,
                        read_lockdown = 0xee1a5f10, preserve = 1, two_locked = 0x08000001,
                        three_locked = 0x0c000001;

struct Commands {
  std::uint32_t all, single;
};

void tlb_lifecycle(Commands commands, bool locked, bool big) {
  fixture::Program code(0x1000, big);
  constexpr std::uint32_t first = 0xe5901000;
  constexpr std::uint32_t second = load_second_word;
  constexpr std::uint32_t third = 0xe5903000;
  constexpr std::uint32_t fourth = 0xe5904000;
  for (const auto word :
       {first, second, commands.all, third, commands.single, fourth, read_lockdown}) {
    code.word(word);
  }
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}}, {}, big);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), old_value, big);
  fixture::put(std::span(machine.alternate).subspan(0x4000, 4), new_value, big);
  auto initial = machine.core->registers();
  initial.system.tlb_lockdown = locked ? preserve : 0;
  machine.core->set_registers(initial);
  const std::array transitions{
      armv5::CodeTransition{.source_variant = 0, .destination_variant = 0, .address = 0x100c},
      armv5::CodeTransition{
          .source_variant = 0, .destination_variant = 0, .address = after_maintenance}};
  for (const auto &transition : transitions) { machine.entries.push_back(transition.address); }
  machine.prepare(true, transitions);
  initial = machine.core->registers();
  initial.r[0] = fixture::MmuMachine::data_virtual;
  machine.core->set_registers(initial);
  fixture::require(machine.core->run(1).execution == armv5::ExecutionState::ready &&
                       machine.core->registers().r[1] == old_value &&
                       machine.core->registers().system.tlb_lockdown == (locked ? two_locked : 0),
                   "execution fills unified code/data TLB; locked fills advance the victim");
  const auto before = machine.core->state();
  const auto saved = machine.core->save();
  machine.first(32, new_section);
  (void)machine.core->install(machine.prepared(), 0);
  fixture::require(machine.core->state() == before, "reinstall preserves processor/TLB state");
  (void)machine.core->run(1);
  fixture::require(machine.core->registers().r[2] == old_value,
                   "page-table edits and reinstall do not discard an existing cached translation");
  const auto invalidated = machine.core->run(1);
  fixture::require(
      invalidated.execution == armv5::ExecutionState::transfer &&
          invalidated.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
          machine.core->state().transition == transitions[0] &&
          machine.core->state().transition_pending,
      "TLB invalidation retires once and revokes through the declared self-transition");
  const auto pending_state = machine.core->state();
  const auto pending = machine.core->save();
  (void)machine.core->install(machine.prepared(), 0);
  fixture::require(machine.core->state() == pending_state,
                   "post-invalidation install has no effects");
  (void)machine.core->run(1);
  fixture::require(machine.core->registers().r[3] == (locked ? old_value : new_value),
                   "invalidate-all preserves lockdown entries and removes ordinary translations");
  const auto after_all = machine.core->state();
  const auto single = machine.core->run(1);
  fixture::require(single.execution == armv5::ExecutionState::transfer &&
                       single.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
                       machine.core->state().transition == transitions[1],
                   "single-entry invalidation revokes");
  (void)machine.core->install(machine.prepared(), 0);
  (void)machine.core->run(2);
  fixture::require(
      machine.core->registers().r[4] == new_value &&
          machine.core->registers().r[5] == (locked ? three_locked : 0),
      "single-MVA invalidation removes locked entries and the next fill advances the victim");
  machine.core->restore(saved);
  fixture::require(machine.core->state() == before,
                   "snapshot restores CP15 and exact source prefix");
  (void)machine.core->run(1);
  fixture::require(
      machine.core->registers().r[2] == old_value,
      "snapshot restores hidden old TLB entries while consumer page tables stay changed");
  machine.core->restore(pending);
  fixture::require(machine.core->state() == pending_state,
                   "snapshot restores pending invalidation");
  (void)machine.core->run(1);
  fixture::require(
      machine.core->state() == after_all,
      "pending invalidation snapshot restores hidden TLB state without later fill residue");
}

void install_does_not_fill() {
  fixture::Program code;
  fixture::Program handler(12);
  code.word(move_zero_three_word);
  handler.word(return_abort_word);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}},
                               {.program = handler, .modes = {fixture::abort_mode}}},
                              {12});
  machine.prepare();
  auto before = machine.core->registers();
  before.system.fault_address = new_section;
  machine.core->set_registers(before);
  machine.first(0, 0); // Only the data bytes change; install must not have cached this descriptor.
  const auto result = machine.core->run_until(1);
  fixture::require(result.execution == armv5::ExecutionState::ready &&
                       result.progress == armv5::Progress{.instructions = 0, .base_cycles = 3} &&
                       machine.core->registers().r[15] == 12 &&
                       machine.core->registers().r[14] == next_pc &&
                       machine.core->registers().system.instruction_fault_status == 5 &&
                       machine.core->registers().system.fault_address == new_section,
                   "install validates without filling TLB; first actual fetch sees the changed "
                   "invalid descriptor");
}

void unified_and_wrap() {
  fixture::Program code;
  code.word(load_second_word);
  code.word(read_lockdown);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}});
  auto registers = machine.core->registers();
  constexpr std::uint32_t last_victim = 0x1c000001;
  registers.system.tlb_lockdown = last_victim;
  machine.core->set_registers(registers);
  machine.prepare();
  registers = machine.core->registers();
  registers.r[0] = code.address();
  machine.core->set_registers(registers);
  const auto result = machine.core->run(2);
  fixture::require(
      result.execution == armv5::ExecutionState::ready &&
          result.progress == armv5::Progress{.instructions = 2, .base_cycles = 2} &&
          machine.core->registers().r[2] == load_second_word &&
          machine.core->registers().r[5] == 1 && machine.core->registers().system.tlb_lockdown == 1,
      "instruction/data share one TLB entry; lockdown victim wraps from seven to zero");
}

struct PageService {
  armv5::Core *cpu = nullptr;
  armv5::State observed;
  std::uint32_t descriptor = 0x00101ff2;
  std::uint32_t requested = 0;
  unsigned calls = 0;
  bool accept = true;

  static bool
  read(void *owner, std::uint32_t address, unsigned width, std::uint32_t &out) noexcept {
    auto &self = *static_cast<PageService *>(owner);
    if (width != 4 || (address & 3) != 0) { return false; }
    self.observed = self.cpu->state();
    self.requested = address;
    ++self.calls;
    if (!self.accept) { return false; }
    self.cpu->request_yield();
    out = self.descriptor;
    return true;
  }
};

enum class YieldForm : std::uint8_t { alu, skipped, branch, load_pc, pop_pc };

struct FetchTargets {
  std::uint32_t next, target;
};

void expected_fetch_yield(armv5::State &expected, YieldForm form, FetchTargets targets) {
  const bool thumb = form == YieldForm::pop_pc;
  const bool next_thumb = form == YieldForm::load_pc;
  constexpr std::uint32_t saved_flags = 0x98000000;
  constexpr std::uint32_t one_locked = 0x04000001;
  expected.execution = armv5::ExecutionState::yielded;
  expected.registers.r[15] = targets.next;
  expected.registers.system.tlb_lockdown = one_locked;
  expected.progress = {.instructions = 1, .base_cycles = 1};
  if (form == YieldForm::alu) { expected.registers.r[0] = 3; }
  if (form == YieldForm::branch) {
    expected.registers.r[14] = targets.next;
    expected.banks.supervisor[1] = targets.next;
    expected.registers.r[15] = targets.target;
    expected.progress.base_cycles = 3;
  }
  if (form == YieldForm::load_pc || form == YieldForm::pop_pc) {
    expected.registers.r[thumb ? 13 : 0] += 4;
    if (thumb) { expected.banks.supervisor[0] += 4; }
    expected.registers.r[15] = targets.target;
    expected.registers.cpsr = saved_flags | initial_cpsr | (next_thumb ? 0x20 : 0);
    expected.registers.system.tlb_lockdown = two_locked;
    expected.progress.base_cycles = 5;
  }
}

void fetch_yield(YieldForm form, bool accept) {
  PageService service;
  fixture::Program code;
  fixture::Program target(target_address);
  const bool thumb = form == YieldForm::pop_pc;
  const bool next_thumb = form == YieldForm::load_pc;
  constexpr std::uint32_t conditional_swi = 0x0f000000;
  constexpr std::uint32_t code_coarse = 0x9011;
  constexpr std::uint32_t guest_data = 0x02004000;
  constexpr std::uint32_t saved_flags = 0x98000000;
  constexpr std::uint32_t page_read_address = 0x9004;
  constexpr std::uint16_t pop_pc = 0xbd00;
  constexpr std::uint16_t move_r1 = 0x2103;
  switch (form) {
    case YieldForm::alu: code.word(fixture::data(13, false, 0, 0, true, 3)); break;
    case YieldForm::skipped: code.word(conditional_swi); break;
    case YieldForm::branch:
      code.word(fixture::branch(code.pc(), {.address = target.address()}, 14, true));
      break;
    case YieldForm::load_pc: code.word(fixture::transfer(true, false, 15, 0, 4, false)); break;
    case YieldForm::pop_pc: code.half(pop_pc); break;
  }
  if (next_thumb) {
    target.half(move_r1);
  } else {
    target.word(fixture::data(13, false, 1, 0, true, 3));
  }
  fixture::MmuMachine machine(
      {{.program = code,
        .modes = {fixture::supervisor_mode},
        .instruction_set =
            thumb ? armv5::CodeSpan::InstructionSet::thumb : armv5::CodeSpan::InstructionSet::arm},
       {.program = target,
        .modes = {fixture::supervisor_mode},
        .instruction_set = next_thumb ? armv5::CodeSpan::InstructionSet::thumb
                                      : armv5::CodeSpan::InstructionSet::arm}},
      {},
      false,
      {.owner = &service, .read = &PageService::read});
  auto registers = machine.core->registers();
  registers.system.tlb_lockdown = 1;
  machine.core->set_registers(registers);
  machine.prepare();
  service.cpu = machine.core.get();
  service.accept = accept;
  machine.first(0, code_coarse); // Change the descriptor after side-effect-free installation.
  fixture::put(std::span(machine.data).subspan(0x4000, 4), target.address() | unsigned(next_thumb));
  registers = machine.core->registers();
  registers.cpsr = saved_flags | initial_cpsr | (thumb ? 0x20 : 0);
  registers.r[thumb ? 13 : 0] = guest_data;
  machine.core->set_registers(registers);
  const auto before = machine.core->state();
  auto expected = before;
  if (accept) {
    expected_fetch_yield(expected, form, {.next = code.pc(), .target = target.address()});
  }
  const auto result = machine.core->run(16);
  if (!accept) {
    expected.execution = armv5::ExecutionState::fault;
    expected.fault = result.fault;
    fixture::require(result.fault.kind == armv5::Fault::Kind::memory &&
                         result.fault.address == page_read_address,
                     "rejected instruction walk is a host memory fault");
  }
  fixture::require(service.calls == 1 && service.requested == page_read_address &&
                       service.observed == before && machine.core->state() == expected &&
                       result.progress == expected.progress,
                   "instruction-walk callback sees exact prefix; yield completes "
                   "predicate/branch/link/interworking and TLB effects");
  const auto saved = machine.core->save();
  if (accept &&
      (form == YieldForm::branch || form == YieldForm::load_pc || form == YieldForm::pop_pc)) {
    service.descriptor = 0; // A lost snapshot TLB entry would now fault on the device descriptor.
    (void)machine.core->run(1);
    const auto after = machine.core->state();
    fixture::require(after.registers.r[1] == 3 && service.calls == 1,
                     "target fetch reuses unified cached entry");
    machine.core->restore(saved);
    (void)machine.core->run(1);
    fixture::require(
        machine.core->state() == after && service.calls == 1,
        "callback/yield snapshot restores hidden translation state across ARM/Thumb destinations");
  }
}

void data_walk_callback(bool accept) {
  PageService service;
  service.accept = accept;
  service.descriptor = data_page_descriptor;
  fixture::Program code;
  code.word(fixture::transfer(false, false, 2, 0, 4, false));
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}},
                              {},
                              false,
                              {.owner = &service, .read = &PageService::read});
  constexpr std::uint32_t coarse_data = 0x9031;
  constexpr std::uint32_t data_page_read = 0x9010;
  constexpr std::uint32_t stored = 0x87654321;
  constexpr std::uint32_t one_locked = 0x04000001;
  machine.first(32, coarse_data);
  auto registers = machine.core->registers();
  registers.system.tlb_lockdown = 1;
  machine.core->set_registers(registers);
  machine.prepare();
  service.cpu = machine.core.get();
  registers = machine.core->registers();
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = stored;
  machine.core->set_registers(registers);
  auto observed = machine.core->state();
  observed.registers.system.tlb_lockdown = one_locked;
  auto expected = observed;
  auto ram = machine.data;
  if (accept) {
    expected.registers.r[0] += 4;
    expected.registers.r[15] = code.pc();
    expected.registers.system.tlb_lockdown = two_locked;
    expected.execution = armv5::ExecutionState::yielded;
    expected.progress = {.instructions = 1, .base_cycles = 1};
    fixture::put(std::span(ram).first(4), stored);
  }
  const auto result = machine.core->run(16);
  if (!accept) {
    expected.execution = armv5::ExecutionState::fault;
    expected.fault = result.fault;
    fixture::require(result.fault.kind == armv5::Fault::Kind::memory &&
                         result.fault.address == data_page_read,
                     "a rejected data page-table read is a host fault before the store");
  }
  fixture::require(service.calls == 1 && service.requested == data_page_read &&
                       service.observed == observed && machine.core->state() == expected &&
                       machine.data == ram,
                   "store translation uses the read callback, exposes exact prefix, and completes "
                   "accepted yield atomically");
}

void subpages(bool large, bool fine, bool big) {
  fixture::Program user(0x1000, big);
  fixture::Program maintenance(maintenance_address, big);
  fixture::Program handler(0x10, big);
  user.word(fixture::transfer(true, false, 2, 0, 0));
  user.word(fixture::transfer(false, false, 2, 0, 0));
  constexpr std::uint32_t invalidate = 0xee080f35;
  constexpr std::uint32_t abort_return = return_abort_word;
  constexpr std::uint32_t small_permissions = 0x00208272;
  constexpr std::uint32_t large_permissions = 0x00200271;
  constexpr std::uint32_t small_updated = 0x002082f2;
  constexpr std::uint32_t large_updated = 0x002002f1;
  constexpr std::uint32_t five_locked = 0x14000001;
  constexpr std::uint32_t six_locked = 0x18000001;
  constexpr std::uint32_t small_start = 0x02004000;
  constexpr std::uint32_t large_start = 0x02000000;
  constexpr std::uint32_t page_permission = 0x1f;
  maintenance.word(invalidate);
  maintenance.word(fixture::data(13, false, 1, 0, false, 1));
  handler.word(abort_return);
  fixture::MmuMachine machine({{.program = user, .modes = {fixture::user_mode}},
                               {.program = maintenance, .modes = {fixture::supervisor_mode}},
                               {.program = handler, .modes = {fixture::abort_mode}}},
                              {0x10},
                              big);
  const auto start = large ? large_start : small_start;
  const unsigned quarter_size = large ? 16384 : 1024;
  const unsigned physical_start = large ? 0 : 0x8000;
  machine.first(32, fine ? fine_table_descriptor : coarse_table_descriptor);
  machine.page(large ? large_permissions : small_permissions);
  for (unsigned quarter = 0; quarter < 4; ++quarter) {
    fixture::put(
        std::span(machine.data).subspan(physical_start + (std::size_t(quarter) * quarter_size), 4),
        old_value,
        big);
  }
  auto registers = machine.core->registers();
  registers.system.tlb_lockdown = 1;
  machine.core->set_registers(registers);
  machine.entries.push_back(next_pc);
  machine.entries.push_back(after_maintenance);
  const std::array transitions{armv5::CodeTransition{
      .source_variant = 0, .destination_variant = 0, .address = after_maintenance}};
  machine.prepare(true, transitions);
  auto access = [&](unsigned quarter, bool writing, bool allowed) {
    auto state = machine.core->registers();
    state.cpsr = fixture::user_mode;
    state.spsr = 0;
    state.r[0] = start + quarter * quarter_size;
    state.r[15] = writing ? next_pc : user.address();
    state.r[2] = new_value;
    machine.core->set_registers(state);
    const auto ram = machine.data;
    const auto result = machine.core->run_until(machine.core->state().progress.base_cycles + 1);
    if (allowed) {
      fixture::require(result.execution == armv5::ExecutionState::ready &&
                           result.progress ==
                               armv5::Progress{.instructions = 1, .base_cycles = 1} &&
                           machine.core->registers().r[2] == old_value,
                       "subpage User read has its own permission/physical offset");
    } else {
      fixture::require(
          result.progress == armv5::Progress{.instructions = 0, .base_cycles = 3} &&
              machine.core->registers().system.data_fault_status == page_permission &&
              machine.core->registers().system.fault_address == state.r[0] && machine.data == ram,
          "cached subpage permission remains effective until that subpage is invalidated");
    }
  };
  access(0, false, true);
  access(1, false, false);
  access(2, false, true);
  access(3, false, false);
  fixture::require(machine.core->registers().system.tlb_lockdown == five_locked,
                   "different permissions create four independent cached subpage ranges");
  machine.page(large ? large_updated : small_updated);
  registers = machine.core->registers();
  registers.cpsr = fixture::supervisor_mode;
  registers.r[0] = start + quarter_size;
  registers.r[15] = maintenance.address();
  machine.core->set_registers(registers);
  fixture::require(machine.core->run(1).execution == armv5::ExecutionState::transfer,
                   "subpage invalidate reaches the declared transition");
  (void)machine.core->install(machine.prepared(), 0);
  (void)machine.core->run(1);
  access(0, false, true);
  access(1, false, true);
  access(2, true, false);
  access(3, false, false);
  fixture::require(
      machine.core->registers().system.tlb_lockdown == six_locked,
      "only the invalidated subpage refills, while other preserved subpages remain cached");
}

void code_alias(bool partial, bool declared) {
  fixture::Program original;
  fixture::Program handler(0x10);
  constexpr std::uint32_t store_pair = 0xe8a00006;
  constexpr std::uint32_t abort_return = return_abort_word;
  constexpr std::uint32_t alias_section = 0x00100c12;
  constexpr std::uint32_t replacement = 0xe3a02009;
  constexpr std::uint32_t alias_start = 0x01001000;
  if (partial) {
    original.word(store_pair);
  } else {
    original.word(fixture::transfer(false, false, 1, 0, 0));
    original.word(fixture::data(13, false, 2, 0, true, 1));
  }
  handler.word(abort_return);
  fixture::MmuMachine machine({{.program = original, .modes = {fixture::supervisor_mode}},
                               {.program = handler, .modes = {fixture::abort_mode}}},
                              {0x10});
  machine.first(16, alias_section);
  machine.regions[0].writable = true;
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  auto bytes = original.bytes();
  fixture::put(std::span(bytes).subspan(partial ? 0 : 4, 4), replacement);
  auto changed = machine.spans;
  changed[0].bytes = bytes;
  const auto destination = partial ? 0x10U : next_pc;
  const std::array<std::uint32_t, 1> source_entries{original.address()};
  const std::array<std::uint32_t, 1> target_entries{destination};
  const std::array variants{
      armv5::CodeVariant{
          .id = 0, .spans = machine.spans, .entries = source_entries, .mapping = machine.mapping()},
      armv5::CodeVariant{
          .id = 1, .spans = changed, .entries = target_entries, .mapping = machine.mapping()}};
  const std::array transitions{
      armv5::CodeTransition{.source_variant = 0, .destination_variant = 1, .address = destination}};
  const armv5::CodeImage image{.identity = "authored-mmu-code-alias",
                               .variants = variants,
                               .layouts = {},
                               .transitions =
                                   declared ? std::span<const armv5::CodeTransition>(transitions)
                                            : std::span<const armv5::CodeTransition>{},
                               .interrupt_vectors = machine.vectors};
  const auto native = armv5::Core::compile(image);
  auto registers = machine.core->registers();
  registers.system.mapping = machine.mapping();
  registers.r[15] = original.address();
  registers.r[0] = alias_start + (partial ? 0 : 4);
  registers.r[1] = replacement;
  machine.core->set_registers(registers);
  (void)machine.core->install(native, 0);
  const auto result = machine.core->run(2);
  fixture::require(machine.code[0] == bytes && machine.core->registers().r[0] == registers.r[0] &&
                       machine.core->registers().r[15] == destination &&
                       result.progress == armv5::Progress{.instructions = partial ? 0U : 1U,
                                                          .base_cycles = partial ? 3U : 1U},
                   "translated code alias retains exact store/abort prefix and consumer bytes");
  if (partial) {
    fixture::require(machine.core->registers().system.data_fault_status == 8 &&
                         machine.core->registers().system.fault_address == alias_start + 4 &&
                         machine.core->registers().r[14] == original.address() + 8,
                     "partial translated code write enters Data Abort before binding revocation");
  }
  if (declared) {
    fixture::require(result.execution == armv5::ExecutionState::transfer,
                     "translated code alias takes declared transition");
    const auto before = machine.core->state();
    const auto snapshot = machine.core->save();
    (void)machine.core->install(native, 1);
    fixture::require(machine.core->state() == before,
                     "changed variant install changes no guest state/time");
    (void)machine.core->run(1);
    const auto after = machine.core->state();
    fixture::require(partial || after.registers.r[2] == 9,
                     "changed translated successor executes prepared new word");
    machine.core->restore(snapshot);
    (void)machine.core->run(1);
    fixture::require(machine.core->state() == after,
                     "translated changed-code transition snapshot replay");
  } else {
    fixture::require(result.fault.kind == armv5::Fault::Kind::changed_code &&
                         result.fault.address == registers.r[0],
                     "translated changed-code fault identifies the original guest alias, not its "
                     "physical backing");
  }
}

void code_remap(bool same_bytes) {
  fixture::Program original;
  original.word(fixture::data(13, false, 2, 0, true, 1));
  fixture::Program changed;
  changed.word(fixture::data(13, false, 2, 0, true, same_bytes ? 1 : 9));
  fixture::MmuMachine machine({{.program = original, .modes = {fixture::supervisor_mode}}});
  std::ranges::copy(changed.bytes(), machine.alternate.begin() + 0x1000);
  machine.regions.back().executable = true;
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  auto alternate = machine.spans;
  alternate[0].bytes = changed.bytes();
  const std::array variants{
      armv5::CodeVariant{.id = 0,
                         .spans = machine.spans,
                         .entries = machine.entries,
                         .mapping = machine.mapping()},
      armv5::CodeVariant{
          .id = 1, .spans = alternate, .entries = machine.entries, .mapping = machine.mapping()}};
  const std::array transitions{armv5::CodeTransition{
      .source_variant = 0, .destination_variant = 1, .address = original.address()}};
  const armv5::CodeImage image{.identity = "authored-mmu-code-remap",
                               .variants = variants,
                               .layouts = {},
                               .transitions = transitions,
                               .interrupt_vectors = {}};
  const auto native = armv5::Core::compile(image);
  auto registers = machine.core->registers();
  registers.system.mapping = machine.mapping();
  registers.r[15] = original.address();
  machine.core->set_registers(registers);
  (void)machine.core->install(native, 0);
  constexpr std::uint32_t alternate_section = 0x00300c12;
  machine.first(0, alternate_section);
  const auto result = machine.core->run(1);
  fixture::require(
      result.execution == armv5::ExecutionState::transfer && result.progress == armv5::Progress{} &&
          machine.core->registers() == registers,
      "changed physical instruction mapping revokes before execution, including equal bytes");
  const auto before = machine.core->state();
  (void)machine.core->install(native, 1);
  fixture::require(machine.core->state() == before,
                   "translated code remap install does not execute or initialize the processor");
  const auto executed = machine.core->run(1);
  fixture::require(executed.execution == armv5::ExecutionState::ready &&
                       executed.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
                       machine.core->registers().r[2] == (same_bytes ? 1U : 9U),
                   "remapped declared unit runs actual destination code");
}

} // namespace

void mmu_lifecycle() {
  for (const bool partial : {false, true}) {
    for (const bool declared : {false, true}) { code_alias(partial, declared); }
  }
  code_remap(false);
  code_remap(true);
  for (const bool large : {false, true}) {
    for (const bool fine : {false, true}) {
      for (const bool big : {false, true}) { subpages(large, fine, big); }
    }
  }
  for (const auto form : {YieldForm::alu,
                          YieldForm::skipped,
                          YieldForm::branch,
                          YieldForm::load_pc,
                          YieldForm::pop_pc}) {
    fetch_yield(form, true);
  }
  fetch_yield(YieldForm::alu, false);
  data_walk_callback(true);
  data_walk_callback(false);
  install_does_not_fill();
  unified_and_wrap();
  constexpr std::array commands{Commands{.all = 0xee080f15, .single = 0xee080f35},
                                Commands{.all = 0xee080f16, .single = 0xee080f36},
                                Commands{.all = 0xee080f17, .single = 0xee080f37}};
  for (const auto command : commands) {
    for (const bool locked : {false, true}) {
      for (const bool big : {false, true}) { tlb_lifecycle(command, locked, big); }
    }
  }
}
