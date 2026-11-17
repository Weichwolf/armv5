// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {
constexpr std::uint32_t entry = 0x1000, alternative = 0x100c, callee = 0x1020,
                        thumb_target = 0x1051, undeclared = 0x9000, stack_pointer = 0x4020;
constexpr std::uint32_t bx_lr = 0xe12fff1e;
constexpr std::uint32_t bxj_lr = 0xe12fff2e;
constexpr std::uint16_t thumb_move = 0x2003;
constexpr std::uint32_t callback_address = 0x20000, callback_value = 0xaabbccdd;

enum class ReturnForm : std::uint8_t { bx, bxj, move, load, multiple };

bool stacked(ReturnForm form) {
  return form == ReturnForm::load || form == ReturnForm::multiple;
}

fixture::Program program(ReturnForm form, unsigned condition = 14) {
  fixture::Program code(entry);
  code.word(fixture::branch(code.pc(), {.address = callee}, condition, true));
  code.word(fixture::data(13, false, 0, 0, true, 1));
  code.word(fixture::branch(code.pc(), {.address = code.pc()}));
  code.word(fixture::data(13, false, 0, 0, true, 2));
  code.word(fixture::branch(code.pc(), {.address = code.pc()}));
  while (code.pc() < callee) { code.word(fixture::data(13, false, 0, 0, false, 0)); }
  if (stacked(form)) {
    code.word(fixture::transfer(false, false, 2, 13, 4, true, false, true));
    constexpr std::uint32_t pop_pc = 0xe8bd8000;
    code.word(form == ReturnForm::multiple ? pop_pc
                                           : fixture::transfer(true, false, 15, 13, 4, false));
  } else {
    code.word(fixture::data(13, false, 14, 0, false, 2));
    auto returning = form == ReturnForm::bxj ? bxj_lr : bx_lr;
    if (form == ReturnForm::move) { returning = fixture::data(13, false, 15, 0, false, 14); }
    code.word(returning);
  }
  return code;
}

struct Case {
  ReturnForm form;
  std::uint32_t target;
  unsigned budget;
  std::optional<std::uint64_t> deadline;
};

template <class Machine> void replay(Machine &machine, const std::vector<std::byte> &expected_ram) {
  const auto before = machine.core->state();
  const auto snapshot = machine.core->save();
  (void)machine.core->run(2);
  const auto after = machine.core->state();
  machine.core->restore(snapshot);
  fixture::require(machine.core->state() == before, "snapshot after BL or stack prefix");
  (void)machine.core->run(2);
  fixture::require(machine.core->state() == after && machine.ram == expected_ram,
                   "return replay retains no later continuation");
}

void expected_prefix(Case test, armv5::Registers &expected, std::span<std::byte> expected_ram) {
  if (test.budget == 0) { return; }
  expected.r[14] = entry + 4;
  expected.r[15] = callee;
  if (test.budget >= 2) {
    expected.r[15] += 4;
    if (stacked(test.form)) {
      expected.r[13] -= 4;
      fixture::put(std::span(expected_ram).subspan(stack_pointer - 0x4000 - 4, 4), test.target);
    } else {
      expected.r[14] = test.target;
    }
  }
  if (test.budget >= 3) {
    expected.r[15] = test.target & ~std::uint32_t{1};
    if (stacked(test.form)) { expected.r[13] += 4; }
    if (test.target == thumb_target && test.form != ReturnForm::move) { expected.cpsr |= 0x20; }
  }
}

void verify(fixture::Modes &machine, Case test) {
  machine.core->reset(entry);
  std::ranges::fill(machine.ram, std::byte{0});
  auto expected = machine.core->registers();
  expected.r[2] = test.target;
  expected.r[13] = stack_pointer;
  machine.core->set_registers(expected);
  auto expected_ram = machine.ram;
  expected_prefix(test, expected, expected_ram);
  const bool missing =
      test.budget == 4 &&
      (test.target == undeclared || (test.form == ReturnForm::move && test.target == thumb_target));
  if (test.budget == 4 && !missing) {
    expected.r[0] = 3;
    if (test.target == entry + 4) { expected.r[0] = 1; }
    if (test.target == alternative) { expected.r[0] = 2; }
    expected.r[15] += test.target == thumb_target ? 2 : 4;
  }
  const auto completed = missing ? 3U : test.budget;
  constexpr std::array<std::uint64_t, 5> base_cycles{0, 3, 4, 7, 8};
  const auto cycles = base_cycles[completed] + (stacked(test.form) && completed >= 3 ? 2 : 0);
  const auto result =
      test.deadline ? machine.core->run_until(*test.deadline) : machine.core->run(test.budget);
  fixture::require(machine.core->registers() == expected && machine.ram == expected_ram &&
                       result.progress ==
                           armv5::Progress{.instructions = completed, .base_cycles = cycles} &&
                       result.execution ==
                           (missing ? armv5::ExecutionState::fault : armv5::ExecutionState::ready),
                   "call return, modified link/stack, interworking or exact bound");
  if (missing) {
    fixture::require(result.fault.kind == armv5::Fault::Kind::unprepared,
                     "undeclared return fault");
  }
  if (test.budget == 2) { replay(machine, expected_ram); }
}

struct CallbackContext {
  armv5::Core *cpu = nullptr;
  armv5::State observed;

  static bool
  read(void *owner, std::uint32_t address, unsigned width, std::uint32_t &out) noexcept {
    auto &state = *static_cast<CallbackContext *>(owner);
    if (address != callback_address || address + width != callback_address + 4) { return false; }
    state.observed = state.cpu->state();
    state.cpu->request_yield();
    out = callback_value;
    return true;
  }
};

void callback_prefix() {
  CallbackContext context;
  fixture::Program code;
  code.word(fixture::branch(code.pc(), {.address = entry + 8}, 14, true));
  code.word(fixture::branch(code.pc(), {.address = code.pc()}));
  code.word(fixture::transfer(true, false, 0, 1, 0));
  code.word(bx_lr);
  const armv5::Io io{.owner = &context, .read = &CallbackContext::read};
  fixture::Machine machine(code, 0x10000, io);
  context.cpu = machine.core.get();
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[1] = callback_address;
  machine.registers(registers);
  const auto ram = machine.ram;
  auto expected = machine.core->state();
  expected.registers.r[14] = entry + 4;
  expected.registers.r[15] = entry + 8;
  expected.banks.supervisor[1] = entry + 4;
  expected.progress = {.instructions = 1, .base_cycles = 3};
  (void)machine.core->run(16);
  fixture::require(context.observed == expected, "inlined call exposes exact callback prefix");
  expected.registers.r[0] = callback_value;
  expected.registers.r[15] = entry + 12;
  expected.progress = {.instructions = 2, .base_cycles = 4};
  expected.execution = armv5::ExecutionState::yielded;
  fixture::require(machine.core->state() == expected,
                   "yield completes the inlined body instruction");
  replay(machine, ram);
}

enum class PcLoad : std::uint8_t { single, multiple, exception, thumb_pop };
constexpr std::uint32_t arm_destination = 0x1100, thumb_destination = 0x1200,
                        callback_high = 0x87654321, initial_high = 0x11223344,
                        legacy_control = 0x58078, fiq_high = 0x55667788, saved_flags = 0xa8000000;

struct PcCallback {
  armv5::Core *cpu = nullptr;
  bool multiple;
  std::uint32_t target;
  std::vector<armv5::State> prefixes;

  static bool
  read(void *owner, std::uint32_t address, unsigned width, std::uint32_t &out) noexcept {
    auto &self = *static_cast<PcCallback *>(owner);
    if (width != 4 || address < callback_address || address - callback_address > 4) {
      return false;
    }
    self.prefixes.push_back(self.cpu->state());
    self.cpu->request_yield();
    out = self.multiple && address == callback_address ? callback_high : self.target;
    return true;
  }
};

fixture::Program pc_load_program(PcLoad form) {
  constexpr std::uint32_t load_multiple = 0xe8b08100;
  constexpr std::uint32_t restore_spsr = 0x00400000;
  constexpr std::uint16_t pop_pc = 0xbd00;
  fixture::Program code;
  switch (form) {
    case PcLoad::single: code.word(fixture::transfer(true, false, 15, 0, 4, false)); break;
    case PcLoad::multiple: code.word(load_multiple); break; // LDMIA R0!,{R8,PC}.
    case PcLoad::exception: code.word(load_multiple | restore_spsr); break;
    case PcLoad::thumb_pop: code.half(pop_pc); break;
  }
  return code;
}

armv5::State pc_yield_expected(armv5::State expected, PcLoad form, std::uint32_t target) {
  const bool multiple = form == PcLoad::multiple || form == PcLoad::exception;
  const bool pop = form == PcLoad::thumb_pop;
  expected.registers.r[pop ? 13 : 0] += multiple ? 8 : 4;
  if (pop) { expected.banks.supervisor[0] += 4; }
  expected.registers.r[15] = target & ((target & 1) != 0 ? ~std::uint32_t{1} : ~std::uint32_t{3});
  if (multiple) {
    expected.registers.r[8] = callback_high;
    expected.banks.user[0] = callback_high;
  }
  if (form == PcLoad::exception) {
    expected.registers.cpsr = expected.registers.spsr;
    expected.banks.spsr[2] = expected.registers.spsr;
    expected.registers.spsr = 0;
    expected.registers.r[8] = fiq_high;
  } else {
    expected.registers.cpsr = (expected.registers.cpsr & ~0x20U) | ((target & 1) != 0 ? 0x20 : 0);
  }
  expected.progress = {.instructions = 1, .base_cycles = multiple ? 6U : 5U};
  expected.execution = armv5::ExecutionState::yielded;
  return expected;
}

void pc_yield(
    PcLoad form, std::uint32_t target, unsigned budget, bool cycle_bound = false, bool l4 = false) {
  const bool pop = form == PcLoad::thumb_pop;
  const bool multiple = form == PcLoad::multiple || form == PcLoad::exception;
  PcCallback callback{.multiple = multiple, .target = target, .prefixes = {}};
  const auto code = pc_load_program(form);
  fixture::Program arm(arm_destination);
  fixture::Program thumb(thumb_destination);
  constexpr std::uint16_t move_r1 = 0x2103;
  constexpr std::uint16_t bl_prefix = 0xf000;
  constexpr std::uint16_t bl_suffix = 0xf800;
  arm.word(fixture::data(13, false, 1, 0, true, 3));
  thumb.half(move_r1); // MOVS R1,#3, then a BL pair whose suffix rejects branch entry.
  thumb.half(bl_prefix);
  thumb.half(bl_suffix);
  fixture::Modes machine(
      {{.program = code,
        .modes = {fixture::supervisor_mode},
        .instruction_set =
            pop ? armv5::CodeSpan::InstructionSet::thumb : armv5::CodeSpan::InstructionSet::arm},
       {.program = arm, .modes = {fixture::supervisor_mode, fixture::fiq_mode}},
       {.program = thumb,
        .modes = {fixture::supervisor_mode, fixture::fiq_mode},
        .instruction_set = armv5::CodeSpan::InstructionSet::thumb}},
      {},
      true,
      0x10000,
      {.owner = &callback, .read = &PcCallback::read},
      l4 ? std::optional(armv5::Registers::System::Mapping{.control = legacy_control})
         : std::nullopt);
  callback.cpu = machine.core.get();
  auto registers = machine.core->registers();
  registers.cpsr = fixture::fiq_mode;
  registers.r[8] = fiq_high;
  machine.core->set_registers(registers);
  registers.cpsr = armv5::Registers::reset_cpsr | (pop ? 0x20 : 0);
  registers.r[pop ? 13 : 0] = callback_address;
  registers.r[8] = initial_high;
  registers.spsr = saved_flags | fixture::fiq_mode | ((target & 1) != 0 ? 0x20 : 0);
  machine.core->set_registers(registers);
  auto callback_expected = machine.core->state();
  auto expected = pc_yield_expected(callback_expected, form, target);
  if (l4 && form != PcLoad::exception) {
    expected.registers.cpsr = callback_expected.registers.cpsr;
    expected.registers.r[15] = target & (pop ? ~std::uint32_t{1} : ~std::uint32_t{3});
  }
  const bool invalid = target == thumb_destination + 5 && form != PcLoad::exception;
  const bool missing = target == undeclared || target == undeclared + 1;
  const bool refused = invalid || (missing && ((target & 1) != 0 || budget > 1));
  const auto result = cycle_bound ? machine.core->run_until(budget) : machine.core->run(budget);
  fixture::require(callback.prefixes.size() == (multiple ? 2U : 1U) &&
                       callback.prefixes[0] == callback_expected,
                   "PC callback observes pre-instruction state");
  if (multiple) {
    callback_expected.registers.r[8] = callback_high;
    callback_expected.banks.user[0] = callback_high;
    fixture::require(callback.prefixes[1] == callback_expected,
                     "yield waits for all LDM accesses and exposes exact partial loads");
  }
  if (refused) {
    expected.execution = armv5::ExecutionState::fault;
    expected.fault = result.fault;
    fixture::require(result.fault.kind == (invalid ? armv5::Fault::Kind::unsupported
                                                   : armv5::Fault::Kind::unprepared),
                     "yield retains ordinary branch destination validation");
  }
  fixture::require(machine.core->state() == expected && result.progress == expected.progress,
                   "yield completes PC alignment, interworking, bank changes and SPSR restore");
  const auto snapshot = machine.core->save();
  (void)machine.core->run(1);
  const auto after = machine.core->state();
  machine.core->restore(snapshot);
  fixture::require(machine.core->state() == expected,
                   "PC yield snapshot retains complete boundary");
  (void)machine.core->run(1);
  fixture::require(machine.core->state() == after, "PC yield snapshot replay");
}

void legacy_pc_yields() {
  struct Case {
    PcLoad form;
    std::uint32_t target;
  };

  const std::array cases{Case{.form = PcLoad::single, .target = arm_destination + 1},
                         Case{.form = PcLoad::multiple, .target = arm_destination + 1},
                         Case{.form = PcLoad::exception, .target = thumb_destination + 1},
                         Case{.form = PcLoad::thumb_pop, .target = thumb_destination}};
  for (const auto test : cases) {
    pc_yield(test.form, test.target, 1, false, true);
    pc_yield(test.form, test.target, 16, false, true);
    pc_yield(test.form, test.target, 1, true, true);
  }
}

void predicated_call(bool taken) {
  fixture::Modes machine(
      {{.program = program(ReturnForm::bx, 0), .modes = {fixture::supervisor_mode}}});
  auto expected = machine.core->registers();
  expected.cpsr |= taken ? 0x40000000 : 0;
  expected.r[2] = taken ? entry + 4 : undeclared;
  machine.core->set_registers(expected);
  const auto ram = machine.ram;
  expected.r[0] = 1;
  expected.r[14] = taken ? entry + 4 : 0;
  expected.r[15] = entry + 8;
  const auto result = machine.run(4);
  fixture::require(machine.core->registers() == expected && machine.ram == ram &&
                       result.progress == armv5::Progress{.instructions = 4, .base_cycles = 8},
                   "predicated static call must preserve link, flags and skipped callee effects");
}

}

void call_returns() {
  for (const auto form : {PcLoad::single, PcLoad::multiple, PcLoad::exception, PcLoad::thumb_pop}) {
    for (const auto target : {arm_destination + 2, thumb_destination + 1, thumb_destination + 5}) {
      pc_yield(form, target, 1);
      pc_yield(form, target, 16);
      pc_yield(form, target, 1, true);
    }
  }
  for (const auto target : {undeclared, undeclared + 1}) {
    pc_yield(PcLoad::single, target, 1);
    pc_yield(PcLoad::single, target, 16);
  }
  legacy_pc_yields();
  callback_prefix();
  predicated_call(false);
  predicated_call(true);
  for (const auto form : {ReturnForm::bx,
                          ReturnForm::bxj,
                          ReturnForm::move,
                          ReturnForm::load,
                          ReturnForm::multiple}) {
    fixture::Program compressed(thumb_target & ~std::uint32_t{1});
    compressed.half(thumb_move);
    fixture::Modes machine({{.program = program(form), .modes = {fixture::supervisor_mode}},
                            {.program = compressed,
                             .modes = {fixture::supervisor_mode},
                             .instruction_set = armv5::CodeSpan::InstructionSet::thumb}});
    const std::array<std::uint64_t, 5> boundaries{
        0, 3, 4, stacked(form) ? 9U : 7U, stacked(form) ? 10U : 8U};
    for (const auto target : {entry + 4, alternative, thumb_target, undeclared}) {
      for (unsigned budget = 0; budget <= 4; ++budget) {
        verify(machine,
               {.form = form, .target = target, .budget = budget, .deadline = std::nullopt});
      }
      for (std::uint64_t deadline = 0; deadline <= boundaries.back(); ++deadline) {
        unsigned count = 0;
        while (boundaries[count] < deadline) { ++count; }
        verify(machine, {.form = form, .target = target, .budget = count, .deadline = deadline});
      }
    }
  }
}
