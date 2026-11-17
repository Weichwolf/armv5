// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <array>
#include <cstdint>

namespace {
constexpr std::uint32_t processor_id = 0x41069265, cache_type = 0x1d112152, reset_control = 0x50078,
                        operand = 0x12345678, initial_status = 0x980000d3, non_flags = 0x0fffffff,
                        fault_bits = 255, lockdown_reset = 0xfff0;

struct Register {
  unsigned number, selector;
};

std::uint32_t transfer(bool read, unsigned destination, Register reg, unsigned predicate = 14) {
  constexpr std::uint32_t cp15_transfer = 0x0e000f10;
  return (predicate << fixture::condition_shift) | cp15_transfer | (read ? 0x00100000 : 0) |
         (reg.number << 16) | (destination << 12) | (reg.selector << 5);
}

void identification(bool big) {
  constexpr std::array<Register, 14> registers{{{.number = 0, .selector = 0},
                                                {.number = 0, .selector = 1},
                                                {.number = 0, .selector = 2},
                                                {.number = 1, .selector = 0},
                                                {.number = 2, .selector = 0},
                                                {.number = 3, .selector = 0},
                                                {.number = 5, .selector = 0},
                                                {.number = 5, .selector = 1},
                                                {.number = 6, .selector = 0},
                                                {.number = 10, .selector = 0},
                                                {.number = 13, .selector = 0},
                                                {.number = 13, .selector = 1},
                                                {.number = 9, .selector = 0},
                                                {.number = 9, .selector = 1}}};
  constexpr std::array<std::uint32_t, 14> expected_values{processor_id,
                                                          cache_type,
                                                          0,
                                                          reset_control,
                                                          0,
                                                          0,
                                                          0,
                                                          0,
                                                          0,
                                                          0,
                                                          0,
                                                          0,
                                                          lockdown_reset,
                                                          lockdown_reset};
  fixture::Program code(0x1000, big);
  for (unsigned index = 0; index < registers.size(); ++index) {
    code.word(transfer(true, index, registers[index]));
  }
  fixture::Machine machine(code);
  machine.prepare({.big_endian = big, .callbacks = {}});
  auto expected = machine.core->registers();
  for (unsigned index = 0; index < registers.size(); ++index) {
    expected.r[index] = expected_values[index];
    if (index == 3 && big) { expected.r[index] |= 0x80; }
  }
  expected.r[15] = code.pc();
  const auto result = machine.run(registers.size());
  fixture::require(machine.core->registers() == expected &&
                       result.progress == armv5::Progress{.instructions = registers.size(),
                                                          .base_cycles = registers.size()},
                   "CP15 reset identification, cache geometry and register values");
  for (unsigned selector = 3; selector < 8; ++selector) {
    fixture::Program alias(0x1000, big);
    alias.word(transfer(true, 0, {.number = 0, .selector = selector}));
    fixture::Machine other(alias);
    other.prepare({.big_endian = big, .callbacks = {}});
    (void)other.run(1);
    fixture::require(other.core->registers().r[0] == processor_id, "CP15 ID aliases");
  }
}

void writable_state() {
  constexpr std::array<Register, 4> registers{{{.number = 5, .selector = 0},
                                               {.number = 5, .selector = 1},
                                               {.number = 6, .selector = 0},
                                               {.number = 13, .selector = 1}}};
  fixture::Program code;
  for (unsigned index = 0; index < registers.size(); ++index) {
    code.word(transfer(false, 0, registers[index]));
    code.word(transfer(true, index + 1, registers[index]));
  }
  fixture::Machine machine(code);
  machine.prepare();
  for (const auto input : {0U, 0xffffffffU, operand}) {
    machine.core->reset(code.address());
    auto expected = machine.core->registers();
    expected.r[0] = input;
    expected.cpsr = initial_status;
    machine.core->set_registers(expected);
    expected.system.data_fault_status = input & fault_bits;
    expected.system.instruction_fault_status = input & fault_bits;
    expected.system.fault_address = input;
    expected.system.context_id = input;
    expected.r[1] = input & fault_bits;
    expected.r[2] = input & fault_bits;
    expected.r[3] = input;
    expected.r[4] = input;
    expected.r[15] = code.pc();
    const auto result = machine.run(8);
    fixture::require(machine.core->registers() == expected &&
                         result.progress == armv5::Progress{.instructions = 8, .base_cycles = 8},
                     "CP15 writable fault/context state and readback");
    const auto saved = machine.core->save();
    auto changed = machine.core->registers();
    changed.system.fault_address ^= operand;
    machine.core->set_registers(changed);
    machine.core->restore(saved);
    fixture::require(machine.core->registers() == expected,
                     "CP15 system registers survive snapshot restoration");
  }
}

void flags_and_predication() {
  fixture::Program code;
  code.word(transfer(true, 15, {.number = 0, .selector = 0}));
  code.word(transfer(true, 1, {.number = 0, .selector = 1}, 0));
  code.word(transfer(false, 0, {.number = 6, .selector = 0}, 1));
  fixture::Machine machine(code);
  machine.prepare();
  auto expected = machine.core->registers();
  expected.cpsr = initial_status;
  expected.r[0] = operand;
  machine.core->set_registers(expected);
  constexpr std::uint32_t id_nzcv = 0x40000000;
  expected.cpsr = (expected.cpsr & non_flags) | id_nzcv;
  expected.r[1] = cache_type;
  expected.r[15] = code.pc();
  const auto result = machine.run(3);
  fixture::require(machine.core->registers() == expected &&
                       result.progress == armv5::Progress{.instructions = 3, .base_cycles = 6},
                   "MRC R15 updates NZCV only and later predicates observe it");
}

void wait_for_interrupt(bool big, bool fixed) {
  constexpr std::uint32_t wait = 0xee070f90;
  constexpr std::uint32_t skipped_wait = 0x0e070f90;
  constexpr std::uint32_t flags = 0x98000000;
  constexpr std::uint32_t next_pc = 0x1008;
  constexpr std::array modes{fixture::user_mode,
                             fixture::fiq_mode,
                             fixture::irq_mode,
                             fixture::supervisor_mode,
                             fixture::abort_mode,
                             fixture::undefined_mode,
                             fixture::system_mode};
  fixture::Program code(0x1000, big);
  code.word(skipped_wait);
  code.word(wait);
  code.word(fixture::data(13, false, 1, 0, true, 7));
  armv5::Registers::System::Mapping mapping;
  mapping.control |= big ? 0x80 : 0;
  fixture::Modes machine(
      {{.program = code, .modes = {modes.begin(), modes.end()}}}, {}, fixed, 0x10000, {}, mapping);
  const auto initial = machine.core->save();
  for (const auto mode : modes) {
    machine.core->restore(initial);
    auto registers = machine.core->registers();
    registers.cpsr = flags | mode;
    machine.core->set_registers(registers);
    auto prefix = machine.core->state();
    prefix.registers.r[15] += 4;
    prefix.progress = {.instructions = 1, .base_cycles = 1};
    fixture::require(machine.core->run(1).execution == armv5::ExecutionState::ready &&
                         machine.core->state() == prefix,
                     "a false WFI predicate retires normally in every mode, including User");
    if (mode == fixture::user_mode) { continue; } // Taken User WFI is checked by undefined_access.
    auto asleep = prefix;
    asleep.execution = armv5::ExecutionState::stopped;
    fixture::require(machine.core->run_until(2).progress == armv5::Progress{} &&
                         machine.core->state() == asleep,
                     "WFI sleeps at its own PC without completing before wake");
    const auto saved = machine.core->save();
    fixture::require(machine.core->run(16).progress == armv5::Progress{} &&
                         machine.core->run_until(16).progress == armv5::Progress{} &&
                         machine.core->state() == asleep,
                     "repeated sleeping runs add no instructions or wait cycles");
    auto awake = prefix;
    awake.registers.r[15] = next_pc;
    awake.progress = {.instructions = 2, .base_cycles = 2};
    machine.core->resume_from_stop();
    fixture::require(machine.core->state() == awake, "wake completes WFI exactly once");
    machine.core->resume_from_stop();
    fixture::require(machine.core->run_until(2).progress == armv5::Progress{} &&
                         machine.core->state() == awake,
                     "wake is idempotent and the reached deadline admits no successor");
    (void)machine.core->run(1);
    auto after = awake;
    after.registers.r[1] = 7;
    after.registers.r[15] = code.pc();
    after.progress = {.instructions = 3, .base_cycles = 3};
    fixture::require(machine.core->state() == after, "wake continues at the prepared successor");
    machine.core->restore(saved);
    fixture::require(machine.core->state() == asleep, "snapshot reinstates an unfinished WFI");
    machine.core->resume_from_stop();
    (void)machine.core->run(1);
    fixture::require(machine.core->state() == after, "sleep/wake snapshot replay is exact");
    machine.core->restore(saved);
  }
  machine.core->reset(code.address());
  fixture::require(machine.core->state().execution == armv5::ExecutionState::ready &&
                       machine.core->state().progress == armv5::Progress{} &&
                       machine.core->registers().r[15] == code.address(),
                   "reset clears WFI without completing or preserving the wait");
}

struct UndefinedCase {
  std::uint32_t instruction, mode;
};

void undefined_access(UndefinedCase test) {
  fixture::Program code;
  code.word(test.instruction);
  fixture::Program handler(4);
  constexpr std::uint32_t return_lr = 0xe1b0f00e;
  handler.word(return_lr);
  fixture::Modes machine({{.program = code, .modes = {test.mode}},
                          {.program = handler, .modes = {fixture::undefined_mode}}},
                         {4});
  auto registers = machine.core->registers();
  registers.cpsr = test.mode;
  registers.r[0] = operand;
  machine.core->set_registers(registers);
  auto expected = machine.core->state();
  expected.registers.r[14] = code.address() + 4;
  expected.registers.r[15] = 4;
  expected.registers.cpsr = fixture::undefined_mode | 0x80;
  expected.registers.spsr = test.mode;
  expected.banks.undefined[1] = code.address() + 4;
  expected.banks.spsr[4] = test.mode;
  expected.progress = {.instructions = 1, .base_cycles = 3};
  const auto result = machine.run(1);
  fixture::require(
      machine.core->state() == expected && result.progress == expected.progress,
      "User CP15 or absent coprocessor takes precise Undefined without transferring data");
  (void)machine.run(1);
  fixture::require(machine.core->registers().cpsr == test.mode &&
                       machine.core->registers().r[15] == code.pc(),
                   "CP15 Undefined return");
}
} // namespace

void system_registers() {
  identification(false);
  identification(true);
  writable_state();
  flags_and_predication();
  constexpr std::uint32_t wait = 0xee070f90;
  undefined_access({.instruction = wait, .mode = fixture::user_mode});
  for (const bool big : {false, true}) {
    for (const bool fixed : {false, true}) { wait_for_interrupt(big, fixed); }
  }
  undefined_access(
      {.instruction = transfer(true, 0, {.number = 0, .selector = 0}), .mode = fixture::user_mode});
  undefined_access({.instruction = transfer(false, 0, {.number = 6, .selector = 0}),
                    .mode = fixture::user_mode});
  for (unsigned coprocessor = 0; coprocessor < 14; ++coprocessor) {
    for (const bool read : {false, true}) {
      constexpr std::uint32_t coprocessor_mask = 0xf00;
      const auto instruction =
          (transfer(read, 0, {.number = 0, .selector = 0}) & ~coprocessor_mask) |
          (coprocessor << 8);
      undefined_access({.instruction = instruction, .mode = fixture::supervisor_mode});
    }
  }
}
