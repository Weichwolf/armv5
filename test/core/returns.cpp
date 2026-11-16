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
constexpr std::uint16_t thumb_move = 0x2003;
constexpr std::uint32_t callback_address = 0x20000, callback_value = 0xaabbccdd;

enum class ReturnForm : std::uint8_t { bx, move, load, multiple };

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
    code.word(form == ReturnForm::move ? fixture::data(13, false, 15, 0, false, 14) : bx_lr);
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
  callback_prefix();
  predicated_call(false);
  predicated_call(true);
  for (const auto form :
       {ReturnForm::bx, ReturnForm::move, ReturnForm::load, ReturnForm::multiple}) {
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
