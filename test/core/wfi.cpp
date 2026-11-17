// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace {
constexpr std::uint32_t wait = 0xee073f90; // R3 supplies the required-zero operand.
constexpr std::uint32_t sleeping_pc = 0x1004;
constexpr std::uint32_t successor_pc = 0x1008;
constexpr std::uint32_t link = 0x100c;
constexpr std::uint32_t flags = 0x98000013;
constexpr std::uint32_t stored = 0x11223344;
constexpr std::uint32_t return_irq = 0xe25ef004;
constexpr std::uint32_t irq_vector = 0x18;
constexpr std::uint32_t fiq_vector = 0x1c;
constexpr std::uint32_t interrupt_masks = 0xc0;

void events() {
  fixture::Program source;
  source.word(fixture::transfer(false, false, 2, 0, 0));
  source.word(wait);
  fixture::Program successor(successor_pc);
  successor.word(fixture::data(4, false, 1, 1, true, 1));
  successor.word(fixture::branch(successor.pc(), {.address = source.address()}));
  fixture::Program irq(irq_vector);
  fixture::Program fiq(fiq_vector);
  irq.word(return_irq);
  fiq.word(return_irq);
  fixture::Modes machine({{.program = source, .modes = {fixture::supervisor_mode}},
                          {.program = successor, .modes = {fixture::supervisor_mode}},
                          {.program = irq, .modes = {fixture::irq_mode}},
                          {.program = fiq, .modes = {fixture::fiq_mode}}},
                         {irq_vector, fiq_vector});
  auto registers = machine.core->registers();
  registers.cpsr = flags;
  registers.r[0] = 0x4000;
  registers.r[2] = stored;
  machine.core->set_registers(registers);
  auto expected = machine.core->state();
  constexpr std::array requests{
      armv5::InterruptRequest{},
      armv5::InterruptRequest{.kind = armv5::InterruptRequest::Kind::fiq}};
  for (unsigned event = 0; event < 8; ++event) {
    const auto result = machine.core->run_until(std::uint64_t(event + 1) * 256);
    expected.execution = armv5::ExecutionState::stopped;
    expected.registers.r[15] = sleeping_pc;
    ++expected.progress.instructions;
    ++expected.progress.base_cycles;
    fixture::require(machine.core->state() == expected &&
                         result.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
                         fixture::get(std::span(machine.ram).first(4)) == stored,
                     "a deadline run completes the store and waits without retiring WFI");
    machine.core->set_memory({.regions = machine.regions, .generation = event});
    (void)machine.core->install(machine.prepared(), 0);
    fixture::require(machine.core->state() == expected, "rebind/install preserves the wait");
    const auto asleep = machine.core->save();
    const auto request = requests[event % 2];
    const bool fast = request.kind == armv5::InterruptRequest::Kind::fiq;
    machine.core->interrupt(request);
    auto entered = expected;
    entered.execution = armv5::ExecutionState::ready;
    entered.registers.r[15] = fast ? fiq_vector : irq_vector;
    entered.registers.r[14] = link;
    constexpr std::uint32_t fiq_status = 0x980000d1;
    constexpr std::uint32_t irq_status = 0x98000092;
    entered.registers.cpsr = fast ? fiq_status : irq_status;
    entered.registers.spsr = flags;
    if (fast) {
      entered.banks.fiq[6] = link;
    } else {
      entered.banks.irq[1] = link;
    }
    entered.banks.spsr[fast ? 0 : 1] = flags;
    ++entered.progress.instructions;
    entered.progress.base_cycles += 4;
    fixture::require(machine.core->state() == entered,
                     "wake completes WFI before IRQ/FIQ, LR=WFI+8");
    (void)machine.core->run(3); // Exception return, increment, branch to the next WFI turn.
    expected.execution = armv5::ExecutionState::ready;
    expected.registers.r[15] = source.address();
    ++expected.registers.r[1];
    expected.banks = entered.banks;
    expected.progress.instructions += 4;
    expected.progress.base_cycles += 11;
    fixture::require(machine.core->state() == expected,
                     "event delivery returns after WFI with exact progress");
    machine.core->restore(asleep);
    machine.core->interrupt(request);
    (void)machine.core->run(3);
    fixture::require(machine.core->state() == expected,
                     "sleep/event/return snapshot replay is exact");
  }
  (void)machine.core->run(16);
  registers = machine.core->registers();
  registers.cpsr |= interrupt_masks;
  machine.core->set_registers(registers);
  const auto masked = machine.core->state();
  for (const auto request : requests) {
    fixture::require(!machine.core->interrupt_ready(request), "masked source is ineligible");
    machine.core->interrupt(request);
  }
  fixture::require(machine.core->state() == masked, "masked interrupt requests leave WFI asleep");
  machine.core->resume_from_stop();
  fixture::require(machine.core->state().execution == armv5::ExecutionState::ready &&
                       machine.core->registers().r[15] == successor_pc &&
                       machine.core->state().progress.instructions ==
                           masked.progress.instructions + 1,
                   "the owner separately signals physical wake without vector entry");
}

void missing_vector() {
  fixture::Program code;
  code.word(wait);
  fixture::Machine machine(code);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.cpsr = fixture::supervisor_mode;
  machine.core->set_registers(registers);
  (void)machine.core->run(1);
  machine.core->interrupt({});
  const auto fault = machine.core->state();
  fixture::require(fault.execution == armv5::ExecutionState::fault &&
                       fault.fault.kind == armv5::Fault::Kind::unprepared &&
                       fault.fault.address == irq_vector &&
                       fault.progress == armv5::Progress{.instructions = 1, .base_cycles = 1},
                   "a missing vector faults after WFI completion and before exception entry");
  machine.core->resume_from_stop();
  fixture::require(machine.core->state() == fault, "wake does not clear a latched fault");
}
} // namespace

void wait_lifecycle() {
  events();
  missing_vector();
}
