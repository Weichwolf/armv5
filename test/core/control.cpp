// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace {
constexpr std::uint32_t entry = 0x1000, vector_base = 0xffff0000, saved_flags = 0xa8000000,
                        fault_address = 0x4001, denied_address = 0x4004, high_alignment = 0x2002,
                        stale_data = 0x55, stale_instruction = 0xaa, stale_address = 0x12345678,
                        arm_nop = 0xe1a00000, thumb_nop = 0x46c0, swi = 0xef000000,
                        exception_return = 0xe1b0f00e;

enum class Cause : std::uint8_t { software, undefined, breakpoint, alignment, irq, fiq };

struct Exception {
  Cause cause;
  std::uint32_t arm, thumb, offset, mode;
  unsigned spsr;
};

void vector_case(Exception test, bool big, bool thumb) {
  const bool asynchronous = test.cause == Cause::irq || test.cause == Cause::fiq;
  const auto size = thumb ? 2U : 4U;
  auto link = entry + size;
  if (test.cause == Cause::breakpoint || asynchronous) { link = entry + 4; }
  if (test.cause == Cause::alignment) { link = entry + 8; }
  const auto return_address = asynchronous ? entry : entry + size;
  const auto vector = vector_base + test.offset;
  fixture::Program code(entry, big);
  fixture::Program handler(vector, big);
  if (thumb) {
    code.half(static_cast<std::uint16_t>(test.thumb));
  } else {
    code.word(test.arm);
  }
  if (thumb) {
    code.half(thumb_nop);
  } // MOV R8,R8 at the return address.
  else {
    code.word(arm_nop);
  }
  handler.word(fixture::data(2, true, 15, 14, true, link - return_address)); // SUBS PC,LR,#n.
  armv5::Registers::System::Mapping mapping;
  mapping.control |= high_alignment | (big ? 0x80 : 0);
  fixture::Modes machine({{.program = code,
                           .modes = {fixture::system_mode},
                           .instruction_set = thumb ? armv5::CodeSpan::InstructionSet::thumb
                                                    : armv5::CodeSpan::InstructionSet::arm},
                          {.program = handler, .modes = {test.mode}}},
                         {vector},
                         true,
                         0x10000,
                         {},
                         mapping);
  auto registers = machine.core->registers();
  registers.cpsr = saved_flags | fixture::system_mode | (thumb ? 0x20 : 0);
  registers.r[0] = fault_address;
  registers.system.data_fault_status = stale_data;
  registers.system.instruction_fault_status = stale_instruction;
  registers.system.fault_address = stale_address;
  machine.core->set_registers(registers);
  auto expected = machine.core->state();
  expected.registers.cpsr = saved_flags | 0x80 | test.mode | (test.cause == Cause::fiq ? 0x40 : 0);
  expected.registers.spsr = registers.cpsr;
  expected.registers.r[14] = link;
  expected.registers.r[15] = vector;
  expected.banks.spsr[test.spsr] = registers.cpsr;
  switch (test.cause) {
    case Cause::software: expected.banks.supervisor[1] = link; break;
    case Cause::undefined: expected.banks.undefined[1] = link; break;
    case Cause::breakpoint: expected.banks.abort[1] = link; break;
    case Cause::alignment:
      expected.banks.abort[1] = link;
      expected.registers.system.data_fault_status = 1;
      expected.registers.system.fault_address = fault_address;
      break;
    case Cause::irq: expected.banks.irq[1] = link; break;
    case Cause::fiq: expected.banks.fiq[6] = link; break;
  }
  expected.progress = {.instructions = asynchronous || test.cause == Cause::alignment ? 0U : 1U,
                       .base_cycles = 3};
  if (asynchronous) {
    const armv5::InterruptRequest request{.kind = test.cause == Cause::fiq
                                                      ? armv5::InterruptRequest::Kind::fiq
                                                      : armv5::InterruptRequest::Kind::irq};
    fixture::require(machine.core->interrupt_ready(request), "high-vector interrupt eligible");
    machine.core->interrupt(request);
  } else {
    const auto result = machine.core->run_until(1);
    fixture::require(result.progress == expected.progress, "high-vector instruction boundary");
  }
  fixture::require(machine.core->state() == expected,
                   "V-bit exception selects actual high vector, ARM state, banks, LR/SPSR");
  const auto saved = machine.core->save();
  (void)machine.run(1);
  const auto after = machine.core->state();
  registers.r[15] = return_address;
  if (test.cause == Cause::alignment) {
    registers.system.data_fault_status = 1;
    registers.system.fault_address = fault_address;
  }
  fixture::require(after.registers == registers, "high-vector return restores ARM/Thumb state");
  machine.core->restore(saved);
  fixture::require(machine.core->state() == expected, "snapshot retains high-vector entry");
  (void)machine.run(1);
  fixture::require(machine.core->state() == after, "high-vector entry snapshot replay");
  machine.core->reset(entry);
  fixture::require(machine.core->registers().system == armv5::Registers::System{},
                   "reset restores the declared low-vector little-endian pin profile");
}

void high_vectors(bool big, bool thumb) {
  constexpr std::array cases{Exception{.cause = Cause::software,
                                       .arm = 0xef000000,
                                       .thumb = 0xdf00,
                                       .offset = 8,
                                       .mode = fixture::supervisor_mode,
                                       .spsr = 2},
                             Exception{.cause = Cause::undefined,
                                       .arm = 0xe7f000f0,
                                       .thumb = 0xde00,
                                       .offset = 4,
                                       .mode = fixture::undefined_mode,
                                       .spsr = 4},
                             Exception{.cause = Cause::breakpoint,
                                       .arm = 0xe1200070,
                                       .thumb = 0xbe00,
                                       .offset = 12,
                                       .mode = fixture::abort_mode,
                                       .spsr = 3},
                             Exception{.cause = Cause::alignment,
                                       .arm = 0xe5902000,
                                       .thumb = 0x6802,
                                       .offset = 16,
                                       .mode = fixture::abort_mode,
                                       .spsr = 3},
                             Exception{.cause = Cause::irq,
                                       .arm = 0xe1a00000,
                                       .thumb = 0x46c0,
                                       .offset = 24,
                                       .mode = fixture::irq_mode,
                                       .spsr = 1},
                             Exception{.cause = Cause::fiq,
                                       .arm = 0xe1a00000,
                                       .thumb = 0x46c0,
                                       .offset = 28,
                                       .mode = fixture::fiq_mode,
                                       .spsr = 0}};
  for (const auto test : cases) { vector_case(test, big, thumb); }
}

struct AbortObserver {
  armv5::Core *cpu = nullptr;
  armv5::State entry;

  static bool
  read(void *owner, std::uint32_t /*address*/, unsigned /*width*/, std::uint32_t &value) noexcept {
    auto &self = *static_cast<AbortObserver *>(owner);
    self.entry = self.cpu->state();
    self.cpu->request_yield();
    value = 0;
    return true;
  }
};

void deny_second_word(fixture::Modes &machine) {
  machine.regions.back().bytes = std::span(machine.ram).first(4);
  machine.regions.push_back({.address = denied_address,
                             .bytes = std::span(machine.ram).subspan(4, 4),
                             .readable = false,
                             .writable = false});
  machine.layout.back().size = 4;
  machine.layout.push_back(
      {.address = denied_address, .size = 4, .readable = false, .writable = false});
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  const std::array variants{
      armv5::CodeVariant{.id = 0,
                         .spans = machine.spans,
                         .entries = machine.entries,
                         .mapping = machine.core->registers().system.mapping}};
  const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = machine.layout}};
  const armv5::CodeImage image{.identity = "authored-loop-external-abort",
                               .variants = variants,
                               .layouts = layouts,
                               .transitions = {},
                               .interrupt_vectors = machine.vectors};
  machine.native.emplace(armv5::Core::compile(image));
  (void)machine.core->install(*machine.native, 0);
}

void loop_abort_prefix(bool external) {
  fixture::Program code;
  fixture::Program handler(0x10);
  // The second turn either becomes unaligned or reaches denied storage.
  const std::array body{fixture::transfer(true, false, 2, 0, 0),
                        fixture::data(4, false, 0, 0, true, external ? 4 : 1),
                        fixture::data(2, true, 3, 3, true, 1)};
  for (const auto instruction : body) { code.word(instruction); }
  code.word(fixture::branch(code.pc(), {.address = code.address()}, 1)); // BNE.
  constexpr std::uint32_t observe_abort = 0xe5954000;
  constexpr std::uint32_t abort_status = 0x200000d7;
  constexpr std::uint32_t saved_status = 0x200000d3;
  constexpr unsigned budget = 100;
  handler.word(observe_abort); // LDR R4,[R5]: observe entry, then yield.
  AbortObserver observer;
  armv5::Registers::System::Mapping mapping;
  mapping.control |= external ? 0 : 2;
  fixture::Modes machine({{.program = code, .modes = {fixture::supervisor_mode}},
                          {.program = handler, .modes = {fixture::abort_mode}}},
                         {0x10},
                         true,
                         0x10000,
                         {.owner = &observer, .read = &AbortObserver::read},
                         mapping);
  if (external) { deny_second_word(machine); }
  observer.cpu = machine.core.get();
  auto registers = machine.core->registers();
  registers.r[0] = 0x4000;
  registers.r[3] = 3;
  registers.r[5] = 0x20000;
  machine.core->set_registers(registers);
  auto expected = machine.core->state();
  expected.registers.r[0] = external ? denied_address : fault_address;
  expected.registers.r[3] = 2;
  expected.registers.r[14] = entry + 8;
  expected.registers.r[15] = 0x10;
  expected.registers.cpsr = abort_status;
  expected.registers.spsr = saved_status;
  expected.registers.system.data_fault_status = external ? 8 : 1;
  expected.registers.system.fault_address = expected.registers.r[0];
  expected.banks.abort[1] = entry + 8;
  expected.banks.spsr[3] = saved_status;
  expected.progress = {.instructions = 4, .base_cycles = 9};
  const auto before = machine.core->save();
  const auto result = machine.core->run(budget);
  fixture::require(observer.entry == expected &&
                       result.execution == armv5::ExecutionState::yielded &&
                       result.progress == armv5::Progress{.instructions = 5, .base_cycles = 10},
                   "data abort inside a granted loop retains the completed turn, counter "
                   "and SUBS flags at the exact abort prefix");
  const auto after = machine.core->state();
  machine.core->restore(before);
  (void)machine.core->run(budget);
  fixture::require(observer.entry == expected && machine.core->state() == after,
                   "granted data-abort snapshot replay has no later turn residue");
}

void undeclared_high_vector() {
  fixture::Program code;
  fixture::Program low(8);
  code.word(swi);
  low.word(exception_return);
  const armv5::Registers::System::Mapping mapping{.control = 0x52078};
  fixture::Modes machine({{.program = code, .modes = {fixture::supervisor_mode}},
                          {.program = low, .modes = {fixture::supervisor_mode}}},
                         {8},
                         true,
                         0x10000,
                         {},
                         mapping);
  auto expected = machine.core->registers();
  expected.r[15] = code.pc();
  const auto result = machine.core->run(1);
  fixture::require(result.execution == armv5::ExecutionState::fault &&
                       result.fault.kind == armv5::Fault::Kind::unprepared &&
                       result.fault.address == vector_base + 8 &&
                       machine.core->registers() == expected &&
                       result.progress == armv5::Progress{.instructions = 1, .base_cycles = 3},
                   "a low-vector declaration cannot authorize an undeclared high vector");
}
} // namespace

void control_lifecycle() {
  for (const bool big : {false, true}) {
    for (const bool thumb : {false, true}) { high_vectors(big, thumb); }
  }
  loop_abort_prefix(false);
  loop_abort_prefix(true);
  undeclared_high_vector();
}
