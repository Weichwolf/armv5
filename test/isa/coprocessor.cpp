// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace {
constexpr std::uint32_t flags = 0x98000000, zero_carry = 0x60000000, reset_masks = 0xc0,
                        return_lr = 0xe1b0f00e, initial_data_status = 0x55,
                        initial_instruction_status = 0xaa, initial_far = 0x87654321,
                        nzcv_mask = 0xf0000000, mode_mask = 0x1f, state_mode_mask = 0x3f,
                        largest_unsigned = 0xffffffff;
constexpr std::array modes{fixture::user_mode,
                           fixture::fiq_mode,
                           fixture::irq_mode,
                           fixture::supervisor_mode,
                           fixture::abort_mode,
                           fixture::undefined_mode,
                           fixture::system_mode};

struct Form {
  std::uint32_t word;
  bool unconditional, register_transfer;
};

// Authored fields: varied opcodes/registers and long, indexed, unindexed and
// literal memory forms. An ABSENT handshake precedes all operand transfers.
constexpr std::array forms{
    Form{.word = 0x0ea920d3, .unconditional = true, .register_transfer = true},   // MCR.
    Form{.word = 0x0eb920d3, .unconditional = true, .register_transfer = true},   // MRC.
    Form{.word = 0x0eb9f0d3, .unconditional = true, .register_transfer = true},   // MRC R15.
    Form{.word = 0x0eb920c3, .unconditional = true, .register_transfer = false},  // CDP.
    Form{.word = 0x0df020ff, .unconditional = true, .register_transfer = false},  // LDCL pre/up/!.
    Form{.word = 0x0d2020ff, .unconditional = true, .register_transfer = false},  // STC pre/down/!.
    Form{.word = 0x0c7020ff, .unconditional = true, .register_transfer = false},  // LDCL post/down.
    Form{.word = 0x0ca020ff, .unconditional = true, .register_transfer = false},  // STC post/up.
    Form{.word = 0x0c9020ff, .unconditional = true, .register_transfer = false},  // LDC unindexed.
    Form{.word = 0x0dcf20ff, .unconditional = true, .register_transfer = false},  // STCL literal.
    Form{.word = 0x0c4320d9, .unconditional = false, .register_transfer = false}, // MCRR.
    Form{.word = 0x0c5320d9, .unconditional = false, .register_transfer = false}}; // MRRC.

struct Case {
  std::uint32_t entry;
  bool conditional;
};

struct Calls {
  unsigned count = 0;

  static bool read(void *owner,
                   std::uint32_t /*address*/,
                   unsigned /*width*/,
                   std::uint32_t & /*value*/) noexcept {
    ++static_cast<Calls *>(owner)->count;
    return false;
  }

  static bool write(void *owner,
                    std::uint32_t /*address*/,
                    unsigned /*width*/,
                    std::uint32_t /*value*/) noexcept {
    ++static_cast<Calls *>(owner)->count;
    return false;
  }
};

struct Program {
  std::vector<fixture::ModeProgram> code;
  std::vector<Case> cases;
};

Program program(unsigned coprocessor, bool big) {
  Program output;
  std::uint32_t address = 0x1000;
  for (const auto form : forms) {
    for (const auto predicate : {0U, 14U, 15U}) {
      if ((predicate == 15 && !form.unconditional) ||
          (coprocessor == 15 && form.register_transfer && predicate != 15)) {
        continue;
      }
      fixture::Program code(address, big);
      output.cases.push_back({.entry = address, .conditional = predicate == 0});
      code.word(fixture::data(4, true, 5, 5, true, 1)); // ADDS R5,R5,#1.
      code.word((predicate << fixture::condition_shift) | form.word | (coprocessor << 8));
      address = code.pc();
      output.code.push_back({.program = std::move(code), .modes = {modes.begin(), modes.end()}});
    }
  }
  return output;
}

armv5::State prefix(const armv5::State &initial, Case test, bool zero) {
  auto expected = initial;
  expected.registers.r[5] = zero ? 0 : 1;
  expected.registers.r[15] = test.entry + 8;
  expected.registers.cpsr = (initial.registers.cpsr & ~nzcv_mask) | (zero ? zero_carry : 0);
  expected.progress = {.instructions = 2, .base_cycles = 2};
  return expected;
}

armv5::State exception_entry(armv5::State expected) {
  const auto saved = expected.registers.cpsr;
  const auto next = expected.registers.r[15];
  if ((saved & mode_mask) == fixture::fiq_mode) {
    for (unsigned index = 8; index < 13; ++index) {
      expected.registers.r[index] = expected.banks.user[index - 8];
    }
  }
  expected.registers.r[13] = expected.banks.undefined[0];
  expected.registers.r[14] = next;
  expected.registers.r[15] = 4;
  expected.registers.cpsr = (saved & ~state_mode_mask) | fixture::undefined_mode | 0x80;
  expected.registers.spsr = saved;
  expected.banks.undefined[1] = next;
  expected.banks.spsr[4] = saved;
  expected.progress.base_cycles = 4;
  return expected;
}

struct Input {
  std::uint32_t mode;
  bool zero;
};

void execute_case(fixture::Modes &machine, const Calls &calls, Case test, Input input) {
  const auto mapping = machine.core->registers().system.mapping;
  machine.core->reset(test.entry);
  auto registers = machine.core->registers();
  for (unsigned index = 0; index < 15; ++index) { registers.r[index] = initial_far + index; }
  registers.cpsr = flags | reset_masks | input.mode;
  registers.r[0] = input.zero ? largest_unsigned : 0x4000U;
  registers.r[5] = input.zero ? largest_unsigned : 0;
  registers.system.mapping = mapping;
  registers.system.data_fault_status = initial_data_status;
  registers.system.instruction_fault_status = initial_instruction_status;
  registers.system.fault_address = initial_far;
  machine.core->set_registers(registers);
  (void)machine.core->install(machine.prepared(), 0);
  const auto initial = machine.core->state();
  auto returned = prefix(initial, test, input.zero);
  const bool trapped = !test.conditional || input.zero;
  const auto expected = trapped ? exception_entry(returned) : returned;
  const auto ram = machine.ram;
  const auto result = machine.core->run(2);
  fixture::require(machine.core->state() == expected && result.progress == expected.progress &&
                       calls.count == 0 && machine.ram == ram,
                   "absent external/illegal CP15 class preserves exact ADDS prefix, banks, "
                   "fault registers and memory; no transfer, callback or writeback");
  if (!trapped) { return; }
  returned.banks.undefined[1] = returned.registers.r[15];
  returned.banks.spsr[4] = returned.registers.cpsr;
  if (input.mode == fixture::undefined_mode) {
    returned.registers.r[14] = returned.registers.r[15];
    returned.registers.spsr = returned.registers.cpsr;
  }
  returned.progress = {.instructions = 3, .base_cycles = 7};
  const auto resumed = machine.core->run(1);
  fixture::require(machine.core->state() == returned &&
                       resumed.progress == armv5::Progress{.instructions = 1, .base_cycles = 3} &&
                       calls.count == 0 && machine.ram == ram,
                   "undefined return restores ARM mode/banks and prefix flags at the original "
                   "virtual continuation");
}

struct Configuration {
  unsigned coprocessor;
  bool big, fixed;
};

void absence(Configuration configuration) {
  const auto [coprocessor, big, fixed] = configuration;
  auto authored = program(coprocessor, big);
  fixture::Program handler(4, big);
  handler.word(return_lr);
  Calls calls;
  armv5::Registers::System::Mapping mapping;
  mapping.control |= big ? 0x80 : 0;
  authored.code.push_back({.program = handler, .modes = {fixture::undefined_mode}});
  fixture::Modes machine(
      std::move(authored.code),
      {4},
      fixed,
      4096,
      {.owner = &calls, .read = &Calls::read, .write = &Calls::write, .swap = &Calls::read},
      mapping);
  for (const auto test : authored.cases) {
    for (const auto mode : modes) {
      for (const bool zero : {false, true}) {
        execute_case(machine, calls, test, {.mode = mode, .zero = zero});
      }
    }
  }
}
} // namespace

void coprocessor_instructions() {
  for (unsigned coprocessor = 0; coprocessor < 16; ++coprocessor) {
    if (coprocessor == 14) { continue; } // Internal debug operations remain separate.
    for (const bool big : {false, true}) {
      for (const bool fixed : {false, true}) {
        absence({.coprocessor = coprocessor, .big = big, .fixed = fixed});
      }
    }
  }
}
