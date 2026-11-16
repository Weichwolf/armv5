// SPDX-License-Identifier: GPL-3.0-only
#include "support/modes.hpp"

#include "armv5/core.hpp"
#include "support/isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::uint32_t msr_control_r0 = 0xe121f000, mrs_saved_r1 = 0xe14f1000,
                        msr_saved_r0 = 0xe16ff000, return_lr = 0xe1b0f00e;
constexpr std::uint32_t interrupt_masks = 0xc0, reset_status = 0xd3, load_return = 0xe8fd8003,
                        first_loaded = 42, second_loaded = 99, returned_status = 0x60000013,
                        stack_after = 0x400c, store_user = 0xe8c07f00, load_user = 0xe8d17f00,
                        user_seed = 17, current_seed = 19, loaded_seed = 23, load_address = 0x4040,
                        multiply = 0xe0000291, accumulate_flags = 0xe0334291,
                        multiply_status = 0x38000013, addend = 0x12345678;
constexpr std::array modes{fixture::user_mode,
                           fixture::fiq_mode,
                           fixture::irq_mode,
                           fixture::supervisor_mode,
                           fixture::abort_mode,
                           fixture::undefined_mode,
                           fixture::system_mode};

void bank_switches() {
  fixture::Program program;
  program.word(msr_control_r0);
  program.word(fixture::branch(program.pc(), {.address = program.address()}));
  fixture::Modes machine({{.program = program, .modes = {modes.begin(), modes.end()}}});
  // Seed each architectural bank through the visible-register API, then switch in guest code.
  for (const auto mode : modes) {
    auto registers = machine.core->registers();
    registers.cpsr = mode;
    for (unsigned index = 8; index < 15; ++index) { registers.r[index] = mode * 256 + index; }
    registers.spsr = 0x80000000 | mode;
    machine.core->set_registers(registers);
  }
  const auto seeded = machine.core->state();
  for (const auto target : modes) {
    auto registers = machine.core->registers();
    registers.r[0] = target | interrupt_masks;
    registers.r[15] = program.address();
    machine.core->set_registers(registers);
    (void)machine.run(1);
    const auto actual = machine.core->registers();
    fixture::require(actual.cpsr == (target | interrupt_masks), "MSR control mode/masks");
    const auto high = target == fixture::fiq_mode ? seeded.banks.fiq : seeded.banks.user;
    for (unsigned index = 8; index < 13; ++index) {
      fixture::require(actual.r[index] == high[index - 8], "FIQ versus shared R8..R12");
    }
    std::array<std::uint32_t, 2> pair{};
    switch (target) {
      case fixture::fiq_mode: pair = {seeded.banks.fiq[5], seeded.banks.fiq[6]}; break;
      case fixture::irq_mode: pair = seeded.banks.irq; break;
      case fixture::supervisor_mode: pair = seeded.banks.supervisor; break;
      case fixture::abort_mode: pair = seeded.banks.abort; break;
      case fixture::undefined_mode: pair = seeded.banks.undefined; break;
      default: pair = {seeded.banks.user[5], seeded.banks.user[6]}; break;
    }
    fixture::require(actual.r[13] == pair[0] && actual.r[14] == pair[1], "banked SP/LR");
    // User MSR cannot change mode or masks. Return to System through the stopped API.
    if (target == fixture::user_mode) {
      auto privileged = machine.core->registers();
      privileged.cpsr = fixture::system_mode;
      machine.core->set_registers(privileged);
    }
  }
  fixture::Program user;
  user.word(msr_control_r0);
  fixture::Modes unprivileged({{.program = user, .modes = {fixture::user_mode}}});
  auto registers = unprivileged.core->registers();
  registers.cpsr = fixture::user_mode;
  registers.r[0] = reset_status;
  unprivileged.core->set_registers(registers);
  (void)unprivileged.run(1);
  fixture::require(unprivileged.core->registers().cpsr == fixture::user_mode,
                   "User cannot change CPSR control");
}

void saved_status_and_returns() {
  for (const auto mode : {fixture::fiq_mode,
                          fixture::irq_mode,
                          fixture::supervisor_mode,
                          fixture::abort_mode,
                          fixture::undefined_mode}) {
    fixture::Program handler;
    handler.word(msr_saved_r0);
    handler.word(mrs_saved_r1);
    handler.word(return_lr);
    fixture::Program destination(0x2000);
    destination.word(fixture::data(4, false, 2, 2, true, 1));
    fixture::Modes machine({{.program = handler, .modes = {mode}},
                            {.program = destination, .modes = {fixture::system_mode}}});
    constexpr std::uint32_t restored = 0xb800001f;
    auto registers = machine.core->registers();
    registers.cpsr = mode;
    registers.r[0] = restored;
    registers.r[14] = destination.address() + 3;
    machine.core->set_registers(registers);
    const auto result = machine.run(3);
    const auto actual = machine.core->registers();
    fixture::require(actual.cpsr == restored && actual.r[1] == restored &&
                         actual.r[15] == destination.address() && actual.r[2] == 0 &&
                         result.progress.instructions == 3,
                     "SPSR round trip and MOVS PC bank/status restoration at exact bound");
    (void)machine.run(1);
    fixture::require(machine.core->registers().r[2] == 1,
                     "return enters prepared target mode unit");
  }
  fixture::Program handler;
  handler.word(load_return); // LDMIA SP!,{R0,R1,PC}^
  fixture::Program destination(0x2000);
  destination.word(fixture::branch(destination.pc(), {.address = destination.pc()}));
  fixture::Modes machine({{.program = handler, .modes = {fixture::irq_mode}},
                          {.program = destination, .modes = {fixture::supervisor_mode}}});
  fixture::put(std::span(machine.ram).subspan(0, 4), first_loaded);
  fixture::put(std::span(machine.ram).subspan(4, 4), second_loaded);
  fixture::put(std::span(machine.ram).subspan(8, 4), destination.address());
  auto registers = machine.core->registers();
  registers.cpsr = fixture::irq_mode;
  registers.spsr = returned_status;
  registers.r[13] = 0x4000;
  machine.core->set_registers(registers);
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[0] == first_loaded &&
                       machine.core->registers().r[1] == second_loaded &&
                       machine.core->registers().r[13] == 0 &&
                       machine.core->registers().cpsr == registers.spsr &&
                       machine.core->state().banks.irq[0] == stack_after,
                   "LDM^ exception return/writeback to old bank");
}

void user_bank_transfers() {
  for (const auto mode : {fixture::fiq_mode,
                          fixture::irq_mode,
                          fixture::supervisor_mode,
                          fixture::abort_mode,
                          fixture::undefined_mode}) {
    fixture::Program program;
    program.word(store_user); // STMIA R0,{R8..R14}^
    program.word(load_user);  // LDMIA R1,{R8..R14}^
    fixture::Modes machine({{.program = program, .modes = {mode}}});
    auto registers = machine.core->registers();
    registers.cpsr = fixture::system_mode;
    for (unsigned index = 8; index < 15; ++index) { registers.r[index] = index * user_seed; }
    machine.core->set_registers(registers);
    registers.cpsr = mode;
    for (unsigned index = 8; index < 15; ++index) { registers.r[index] = index * current_seed; }
    registers.r[0] = 0x4000;
    registers.r[1] = load_address;
    machine.core->set_registers(registers);
    for (unsigned index = 8; index < 15; ++index) {
      fixture::put(std::span(machine.ram).subspan(0x40 + ((index - 8) * 4), 4),
                   index * loaded_seed);
    }
    (void)machine.run(2);
    const auto state = machine.core->state();
    for (unsigned index = 8; index < 15; ++index) {
      const auto old =
          mode != fixture::fiq_mode && index < 13 ? index * current_seed : index * user_seed;
      fixture::require(
          fixture::get(std::span(machine.ram).subspan(std::size_t(index - 8) * 4, 4)) == old,
          "STM^ selects shared/user bank");
      fixture::require(state.banks.user[index - 8] == index * loaded_seed,
                       "LDM^ updates user bank");
      fixture::require(state.registers.r[index] == (mode != fixture::fiq_mode && index < 13
                                                        ? index * loaded_seed
                                                        : index * current_seed),
                       "LDM^ preserves current bank");
    }
  }
}

void invalid_mode_forms() {
  constexpr std::array invalid_modes{fixture::user_mode, fixture::system_mode};
  constexpr std::array words{0xe14f0000U, 0xe16ff000U, 0xe1b0f00eU, 0xe8d17f00U, 0xe8c07f00U};
  for (const auto mode : invalid_modes) {
    for (const auto word : words) {
      fixture::Program code;
      code.word(word);
      fixture::Machine machine(code);
      bool refused = false;
      try {
        (void)armv5::Core::lower(machine.image(), {.processor_mode = mode, .callbacks = {}});
      } catch (const std::runtime_error &error) {
        fixture::contains(error.what(), "authored-arm-fixture at 0x00001000");
        fixture::contains(error.what(), "unpredictable");
        refused = true;
      }
      fixture::require(refused, "encoded bank/SPSR form without privileged exception mode");
    }
  }
  fixture::Program code;
  code.word(msr_control_r0);
  fixture::Machine machine(code);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[0] = 0; // Selected mode 0 is not an ARMv5 processor mode.
  machine.core->set_registers(registers);
  const auto result = machine.core->run(1);
  fixture::require(result.fault.kind == armv5::Fault::Kind::unsupported &&
                       result.progress == armv5::Progress{} &&
                       machine.core->registers() == registers,
                   "unpredictable dynamic MSR value refuses before retirement");
}

void basic_multiply() {
  fixture::Program program;
  program.word(multiply);         // MUL R0,R1,R2
  program.word(accumulate_flags); // MLAS R3,R1,R2,R4
  fixture::Machine machine(program);
  machine.prepare();
  constexpr std::array values{0U, 1U, 0xffffffffU, 0x80000000U, 0x7fffffffU, 0x55555555U};
  for (const auto first : values) {
    for (const auto second : values) {
      machine.core->reset(program.address());
      auto registers = machine.core->registers();
      registers.cpsr = multiply_status;
      registers.r[1] = first;
      registers.r[2] = second;
      registers.r[4] = addend;
      machine.core->set_registers(registers);
      (void)machine.run(2);
      const auto product = std::uint32_t(std::uint64_t(first) * second);
      const auto accumulated = product + registers.r[4];
      const auto flags = (accumulated & 0x80000000) | (accumulated == 0 ? 0x40000000 : 0);
      fixture::require(machine.core->registers().r[0] == product &&
                           machine.core->registers().r[3] == accumulated &&
                           machine.core->registers().cpsr == (multiply_status | flags),
                       "MUL/MLAS wraparound/NZ preserve CVQ");
    }
  }
}
}

void processor_modes() {
  bank_switches();
  saved_status_and_returns();
  user_bank_transfers();
  basic_multiply();
  invalid_mode_forms();
}
