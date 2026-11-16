// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {
constexpr armv5::InterruptRequest irq{};
constexpr armv5::InterruptRequest fiq{.kind = armv5::InterruptRequest::Kind::fiq};
constexpr std::uint32_t irq_mode = 0x12, fiq_mode = 0x11, supervisor_mode = 0x13, abort_mode = 0x17,
                        undefined_mode = 0x1b;
constexpr std::uint32_t return_interrupt = 0xe25ef004, return_exception = 0xe1b0f00e;

constexpr std::uint32_t irq_vector_address = 0x18, fiq_vector_address = 0x1c, irq_body = 0x180,
                        fiq_body = 0x1c0, initial_status = 0xa8000013, irq_status = 0xa8000092,
                        fiq_status = 0xa80000d1, saved_r8 = 0x1234,
                        initial_system_status = 0x9800001f, entry_status_bits = 0x98000080,
                        swi = 0xef000000, load_multiple = 0xe8900003, load_postindexed = 0xe4912004,
                        saved_r2 = 0x5678, abort_load_value = 0xdeadbeef;

void asynchronous_entry() {
  fixture::Program program;
  program.word(fixture::data(4, false, 0, 0, true, 1));
  program.word(fixture::branch(program.pc(), {.address = program.address()}));
  fixture::Program fiq_handler(fiq_body);
  fiq_handler.word(fixture::data(4, false, 8, 8, true, 1));
  fiq_handler.word(return_interrupt);
  fixture::Program fiq_vector(fiq_vector_address);
  fiq_vector.word(fixture::branch(fiq_vector.pc(), {.address = fiq_handler.address()}));
  // IRQ vector branches away so its handler cannot overlap the FIQ vector.
  fixture::Program irq_vector(irq_vector_address);
  irq_vector.word(fixture::branch(irq_vector.pc(), {.address = irq_body}));
  fixture::Program actual_irq(irq_body);
  actual_irq.word(fixture::data(4, false, 1, 1, true, 1));
  actual_irq.word(return_interrupt);
  fixture::Modes machine({{.program = program, .modes = {supervisor_mode}},
                          {.program = irq_vector, .modes = {irq_mode}},
                          {.program = actual_irq, .modes = {irq_mode}},
                          {.program = fiq_vector, .modes = {fiq_mode}},
                          {.program = fiq_handler, .modes = {fiq_mode}}},
                         {irq_vector_address, fiq_vector_address});
  fixture::require(!machine.core->interrupt_ready(irq) && !machine.core->interrupt_ready(fiq),
                   "reset masks IRQ/FIQ");
  const auto reset = machine.core->state();
  machine.core->interrupt(irq);
  fixture::require(machine.core->state() == reset, "masked IRQ changed state/time");
  auto registers = machine.core->registers();
  registers.cpsr = initial_status;
  registers.r[8] = saved_r8;
  registers.r[13] = 0x8000;
  machine.core->set_registers(registers);
  (void)machine.run(1);
  const auto before = machine.core->state();
  const auto snapshot = machine.core->save();
  machine.core->interrupt(irq);
  const auto entry = machine.core->state();
  fixture::require(entry.registers.r[15] == irq_vector_address &&
                       entry.registers.r[14] == program.address() + 8 &&
                       entry.registers.spsr == before.registers.cpsr &&
                       entry.registers.cpsr == irq_status && entry.registers.r[13] == 0 &&
                       entry.progress.instructions == before.progress.instructions &&
                       entry.progress.base_cycles == before.progress.base_cycles + 3 &&
                       !machine.core->interrupt_ready(irq) && machine.core->interrupt_ready(fiq),
                   "IRQ precise boundary, bank, masks, LR/SPSR and charge");
  (void)machine.run(2); // IRQ branch and body; return remains pending.
  const auto interrupted = machine.core->state();
  const auto nested = machine.core->save();
  machine.core->interrupt(fiq);
  const auto fast = machine.core->state();
  fixture::require(
      fast.registers.cpsr == fiq_status && fast.registers.spsr == interrupted.registers.cpsr &&
          fast.registers.r[8] == 0 && fast.registers.r[14] == interrupted.registers.r[15] + 4 &&
          !machine.core->interrupt_ready(irq) && !machine.core->interrupt_ready(fiq),
      "FIQ preempts IRQ with own high bank");
  (void)machine.run(3); // FIQ vector/body/return.
  fixture::require(machine.core->registers() == interrupted.registers &&
                       machine.core->state().banks.fiq[0] == 1,
                   "FIQ restores interrupted IRQ register view");
  (void)machine.run(1);
  fixture::require(machine.core->registers().cpsr == before.registers.cpsr &&
                       machine.core->registers().r[15] == before.registers.r[15] &&
                       machine.core->registers().r[8] == registers.r[8] &&
                       machine.core->registers().r[13] == registers.r[13] &&
                       machine.core->registers().r[1] == 1,
                   "IRQ SUBS PC,LR,#4 restores status and supervisor bank");
  machine.core->restore(nested);
  fixture::require(machine.core->state() == interrupted,
                   "nested interrupted snapshot retained later FIQ residue");
  machine.core->interrupt(fiq);
  (void)machine.run(4);
  const auto replay = machine.core->state();
  machine.core->restore(nested);
  machine.core->interrupt(fiq);
  (void)machine.run(4);
  fixture::require(machine.core->state() == replay, "interrupted snapshot deterministic replay");
  machine.core->restore(snapshot);
  fixture::require(machine.core->state() == before,
                   "pre-interrupt snapshot restores all banks and counters");
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[15] == program.address(),
                   "restored continuation executes next instruction");
}

void synchronous_entry() {
  struct Case {
    std::uint32_t word, vector, mode, link;
  };

  constexpr std::array cases{
      Case{.word = 0xef000123, .vector = 8, .mode = supervisor_mode, .link = 4},
      Case{.word = 0xe7f012f3, .vector = 4, .mode = undefined_mode, .link = 4},
      Case{.word = 0xe1200070, .vector = 12, .mode = abort_mode, .link = 4}};
  for (const auto &item : cases) {
    fixture::Program program;
    program.word(item.word);
    program.word(fixture::data(4, false, 2, 2, true, 1));
    fixture::Program vector(item.vector);
    vector.word(return_exception);
    fixture::Modes machine({{.program = program, .modes = {fixture::system_mode}},
                            {.program = vector, .modes = {item.mode}}},
                           {item.vector});
    auto registers = machine.core->registers();
    registers.cpsr = initial_system_status;
    machine.core->set_registers(registers);
    (void)machine.run(1);
    const auto entry = machine.core->state();
    fixture::require(entry.registers.r[15] == item.vector &&
                         entry.registers.r[14] == program.address() + item.link &&
                         entry.registers.spsr == registers.cpsr &&
                         entry.registers.cpsr == (entry_status_bits | item.mode) &&
                         entry.progress == armv5::Progress{.instructions = 1, .base_cycles = 3},
                     "SWI/UDF/BKPT vector entry at retirement bound");
    (void)machine.run(1);
    fixture::require(machine.core->registers().cpsr == registers.cpsr &&
                         machine.core->registers().r[15] == program.address() + 4,
                     "exception return restores System state");
    (void)machine.run(1);
    fixture::require(machine.core->registers().r[2] == 1, "post-exception execution");
  }
  fixture::Program undeclared;
  undeclared.word(swi);
  fixture::Machine missing(undeclared);
  missing.prepare();
  fixture::require(missing.core->run(1).fault.kind == armv5::Fault::Kind::unprepared &&
                       missing.core->state().fault.address == 8,
                   "undeclared exception vector fails with identity/address/reason");
}

void data_aborts() {
  for (const bool fixed : {false, true}) {
    for (const bool multiple : {false, true}) {
      fixture::Program program;
      // The base is loaded before the second access aborts: it must still be restored.
      program.word(multiple ? load_multiple : load_postindexed); // LDM R0,{R0,R1} / LDR R2,[R1],#4
      fixture::Program handler(0x10);
      handler.word(return_interrupt); // Skip failed instruction: SUBS PC,LR,#4.
      fixture::Modes machine({{.program = program, .modes = {supervisor_mode}},
                              {.program = handler, .modes = {abort_mode}}},
                             {0x10},
                             fixed);
      auto registers = machine.core->registers();
      const auto last = 0x4000 + static_cast<std::uint32_t>(machine.ram.size()) - 4;
      registers.r[0] = multiple ? last : saved_r8;
      registers.r[1] = last + 4;
      registers.r[2] = saved_r2;
      fixture::put(std::span(machine.ram).last(4), abort_load_value);
      machine.core->set_registers(registers);
      const auto result = machine.core->run_until(1);
      fixture::require(result.execution == armv5::ExecutionState::ready &&
                           result.progress.instructions == 0 && result.progress.base_cycles == 3 &&
                           machine.core->registers().r[15] == 0x10 &&
                           machine.core->registers().r[14] == program.address() + 8 &&
                           machine.core->registers().r[0] == registers.r[0] &&
                           machine.core->registers().r[1] == registers.r[1] &&
                           machine.core->registers().r[2] == registers.r[2],
                       "data abort precise base/destination preservation and no retirement");
      (void)machine.run(1);
      fixture::require(machine.core->registers().r[15] == program.address() + 4 &&
                           machine.core->registers().cpsr == registers.cpsr,
                       "data abort return");
    }
  }
}

void abort_after_code_write() {
  constexpr std::uint32_t store_pair = 0xe8a00006;
  fixture::Program original;
  original.word(store_pair); // STMIA R0!,{R1,R2}: first store changes code, second aborts.
  const auto replacement = fixture::branch(original.address(), {.address = original.address()});
  fixture::Program changed;
  changed.word(replacement);
  fixture::Program vector(16);
  vector.word(return_interrupt);
  auto storage = original.bytes();
  auto handler = vector.bytes();
  const std::array supervisor{supervisor_mode};
  const std::array abort{abort_mode};
  const std::array old_spans{
      armv5::CodeSpan{
          .address = original.address(), .bytes = original.bytes(), .processor_modes = supervisor},
      armv5::CodeSpan{
          .address = vector.address(), .bytes = vector.bytes(), .processor_modes = abort}};
  const std::array new_spans{armv5::CodeSpan{.address = changed.address(),
                                             .bytes = changed.bytes(),
                                             .processor_modes = supervisor},
                             old_spans[1]};
  const std::array entries{original.address(), vector.address()};
  const std::array variants{armv5::CodeVariant{.id = 0, .spans = old_spans, .entries = entries},
                            armv5::CodeVariant{.id = 1, .spans = new_spans, .entries = entries}};
  const std::array vectors{vector.address()};
  const std::array transitions{armv5::CodeTransition{
      .source_variant = 0, .destination_variant = 1, .address = vector.address()}};
  const armv5::CodeImage image{.identity = "authored-abort-code-transition",
                               .variants = variants,
                               .layouts = {},
                               .transitions = transitions,
                               .interrupt_vectors = vectors};
  const auto native = armv5::Core::compile(image);
  const std::array regions{armv5::Memory::Region{.address = original.address(),
                                                 .bytes = storage,
                                                 .readable = true,
                                                 .writable = true,
                                                 .executable = true},
                           armv5::Memory::Region{.address = vector.address(),
                                                 .bytes = handler,
                                                 .readable = true,
                                                 .writable = false,
                                                 .executable = true}};
  armv5::Core core({.regions = regions, .generation = 0});
  core.reset(original.address());
  (void)core.install(native, 0);
  auto registers = core.registers();
  registers.r[0] = original.address();
  registers.r[1] = replacement;
  core.set_registers(registers);
  const auto result = core.run(1);
  fixture::require(result.execution == armv5::ExecutionState::transfer &&
                       result.progress.instructions == 0 &&
                       core.registers().r[0] == original.address() &&
                       core.registers().r[14] == original.address() + 8 &&
                       core.registers().r[15] == vector.address() &&
                       core.registers().spsr == registers.cpsr && storage == changed.bytes(),
                   "data abort entry precedes code-binding revocation after a partial store");
  (void)core.install(native, 1);
  (void)core.run(1);
  fixture::require(core.registers().cpsr == registers.cpsr &&
                       core.registers().r[15] == original.address() + 4,
                   "declared changed-code transition resumes the precise abort handler");
  auto no_transition = image;
  no_transition.transitions = {};
  const auto isolated = armv5::Core::compile(no_transition);
  std::ranges::copy(original.bytes(), storage.begin());
  core.reset(original.address());
  core.set_registers(registers);
  (void)core.install(isolated, 0);
  const auto failure = core.run(1);
  fixture::require(failure.fault.kind == armv5::Fault::Kind::changed_code &&
                       failure.fault.address == original.address() &&
                       core.registers().r[15] == vector.address() &&
                       core.registers().spsr == registers.cpsr,
                   "partial-store revocation keeps changed-code address and entered abort state");
}

}

void interrupts() {
  asynchronous_entry();
  synchronous_entry();
  data_aborts();
  abort_after_code_write();
}
