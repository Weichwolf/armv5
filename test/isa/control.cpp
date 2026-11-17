// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {
constexpr std::uint32_t entry = 0x1000, abort_vector = 0x10, data_address = 0x4000,
                        initial_flags = 0xa8000000, stale_status = 0x55, stale_address = 0x12345678,
                        arm_abort_return = 0xe25ef004, thumb_abort_return = 0xe25ef006,
                        abort_status = 0x97, reset_status = 0xd3, blx_r0 = 0xe12fff30;
constexpr std::uint16_t thumb_move_r7 = 0x2703;
constexpr std::array modes{fixture::user_mode,
                           fixture::fiq_mode,
                           fixture::irq_mode,
                           fixture::supervisor_mode,
                           fixture::abort_mode,
                           fixture::undefined_mode,
                           fixture::system_mode};

struct Callback {
  unsigned calls = 0;

  static bool read(void *owner,
                   std::uint32_t /*address*/,
                   unsigned /*width*/,
                   std::uint32_t & /*value*/) noexcept {
    ++static_cast<Callback *>(owner)->calls;
    return false;
  }

  static bool write(void *owner,
                    std::uint32_t /*address*/,
                    unsigned /*width*/,
                    std::uint32_t /*value*/) noexcept {
    ++static_cast<Callback *>(owner)->calls;
    return false;
  }
};

struct Access {
  std::uint32_t instruction;
  unsigned width;
  int first_offset = 0;
  bool thumb = false;
  unsigned base = 0;
};

struct AlignmentInput {
  std::uint32_t address, mode;
};

void alignment_fault(fixture::Modes &machine,
                     Access test,
                     AlignmentInput input,
                     const std::vector<std::byte> &ram) {
  auto registers = machine.core->registers();
  registers.cpsr = initial_flags | input.mode | (test.thumb ? 0x20 : 0);
  registers.r[test.base] = input.address;
  registers.r[1] = stale_address;
  registers.r[2] = stale_address;
  registers.r[3] = stale_status;
  registers.system.data_fault_status = stale_status;
  registers.system.instruction_fault_status = stale_status;
  registers.system.fault_address = stale_address;
  machine.core->set_registers(registers);
  auto expected = machine.core->state();
  expected.registers.system.data_fault_status = 1;
  expected.registers.system.fault_address =
      input.address + static_cast<std::uint32_t>(test.first_offset);
  expected.registers.cpsr = initial_flags | abort_status;
  expected.registers.spsr = registers.cpsr;
  if (test.base == 13) { expected.registers.r[13] = 0; }
  expected.registers.r[14] = entry + 8;
  expected.registers.r[15] = abort_vector;
  if (test.base == 13 && input.mode == fixture::abort_mode) {
    expected.registers.r[13] = registers.r[13];
  }
  expected.banks.abort[1] = entry + 8;
  expected.banks.spsr[3] = registers.cpsr;
  expected.progress = {.instructions = 0, .base_cycles = 3};
  const auto result = machine.core->run_until(1);
  fixture::require(machine.core->state() == expected && result.progress == expected.progress &&
                       machine.ram == ram,
                   "A-bit alignment fault: exact first address, banks, flags, status, "
                   "no writeback/retirement/partial memory/callback effects");
  (void)machine.run(1);
  fixture::require(machine.core->registers().cpsr == registers.cpsr &&
                       machine.core->registers().r[15] == entry + (test.thumb ? 2 : 4) &&
                       machine.core->registers().r[test.base] == registers.r[test.base],
                   "alignment abort return preserves the original base and mode");
}

void alignment_faults(bool fixed, bool big) {
  // Explicit independently authored encodings, with R0 as base and R2/R3 as data.
  constexpr std::array cases{
      Access{.instruction = 0xe5902000, .width = 4},                     // LDR R2,[R0].
      Access{.instruction = 0xe5802000, .width = 4},                     // STR R2,[R0].
      Access{.instruction = 0xe4902004, .width = 4},                     // LDR R2,[R0],#4.
      Access{.instruction = 0xe5a02004, .width = 4, .first_offset = 4},  // STR R2,[R0,#4]!.
      Access{.instruction = 0xe1d020b0, .width = 2},                     // LDRH.
      Access{.instruction = 0xe1c020b0, .width = 2},                     // STRH.
      Access{.instruction = 0xe1d020f0, .width = 2},                     // LDRSH.
      Access{.instruction = 0xe1c020d0, .width = 4},                     // LDRD.
      Access{.instruction = 0xe1c020f0, .width = 4},                     // STRD.
      Access{.instruction = 0xe1002091, .width = 4},                     // SWP R2,R1,[R0].
      Access{.instruction = 0xe8b0000c, .width = 4},                     // LDMIA R0!,{R2,R3}.
      Access{.instruction = 0xe8a0000c, .width = 4},                     // STMIA R0!,{R2,R3}.
      Access{.instruction = 0xe820000c, .width = 4, .first_offset = -4}, // STMDA.
      Access{.instruction = 0xe930000c, .width = 4, .first_offset = -8}, // LDMDB.
      Access{.instruction = 0xe9b0000c, .width = 4, .first_offset = 4},
      Access{.instruction = 0x6802, .width = 4, .thumb = true},             // Thumb LDR R2,[R0].
      Access{.instruction = 0x6002, .width = 4, .thumb = true},             // Thumb STR.
      Access{.instruction = 0x8802, .width = 2, .thumb = true},             // Thumb LDRH.
      Access{.instruction = 0x8002, .width = 2, .thumb = true},             // Thumb STRH.
      Access{.instruction = 0xc80c, .width = 4, .thumb = true},             // Thumb LDMIA.
      Access{.instruction = 0xc00c, .width = 4, .thumb = true},             // Thumb STMIA.
      Access{.instruction = 0xbd00, .width = 4, .thumb = true, .base = 13}, // Thumb POP PC.
      Access{.instruction = 0xb404,
             .width = 4,
             .first_offset = -4,
             .thumb = true,
             .base = 13}}; // Thumb PUSH {R2}.
  for (const auto test : cases) {
    fixture::Program code(entry, big);
    fixture::Program handler(abort_vector, big);
    if (test.thumb) {
      code.half(static_cast<std::uint16_t>(test.instruction));
    } else {
      code.word(test.instruction);
    }
    handler.word(test.thumb ? thumb_abort_return : arm_abort_return); // Skip faulting load/store.
    Callback callback;
    armv5::Registers::System::Mapping mapping;
    mapping.control |= 2 | (big ? 0x80 : 0);
    fixture::Modes machine({{.program = code,
                             .modes = {modes.begin(), modes.end()},
                             .instruction_set = test.thumb ? armv5::CodeSpan::InstructionSet::thumb
                                                           : armv5::CodeSpan::InstructionSet::arm},
                            {.program = handler, .modes = {fixture::abort_mode}}},
                           {abort_vector},
                           fixed,
                           0x10000,
                           {.owner = &callback,
                            .read = &Callback::read,
                            .write = &Callback::write,
                            .swap = &Callback::read},
                           mapping);
    const auto clean = machine.core->save();
    const auto ram = machine.ram;
    // Both a mapped address and one served by refusing callbacks: alignment wins.
    for (const auto base : {data_address, 0x20000U}) {
      for (unsigned low = 1; low < test.width; ++low) {
        for (const auto mode : modes) {
          machine.core->restore(clean);
          alignment_fault(machine, test, {.address = base + low, .mode = mode}, ram);
          fixture::require(callback.calls == 0, "alignment check precedes every callback");
        }
      }
    }
  }
}

void aligned_and_byte_accesses(bool fixed, bool big) {
  fixture::Program code(entry, big);
  constexpr std::array instructions{0xe5902000U,
                                    0xe5d03001U,
                                    0xe1d040b2U,
                                    0xe5c03005U,
                                    0xe1d050d1U,
                                    0xe1006091U,
                                    0xe2800001U,
                                    0xe1407091U};
  for (const auto instruction : instructions) { code.word(instruction); }
  armv5::Registers::System::Mapping mapping;
  mapping.control |= 2 | (big ? 0x80 : 0);
  fixture::Modes machine(
      {{.program = code, .modes = {fixture::supervisor_mode}}}, {}, fixed, 0x10000, {}, mapping);
  constexpr std::uint32_t original_word = 0x12345678;
  constexpr std::uint32_t swapped_word = 0x87654321;
  fixture::put(std::span(machine.ram).first(4), original_word, big);
  auto registers = machine.core->registers();
  registers.r[0] = data_address;
  registers.r[1] = swapped_word;
  registers.system.data_fault_status = stale_status;
  registers.system.fault_address = stale_address;
  machine.core->set_registers(registers);
  auto expected_ram = machine.ram;
  fixture::put(std::span(expected_ram).first(4), swapped_word, big);
  constexpr std::byte swap_byte{0x21};

  struct EndianResult {
    unsigned byte, half, swapped;
  };

  constexpr EndianResult little{.byte = 0x56, .half = 0x1234, .swapped = 0x43};
  constexpr EndianResult large{.byte = 0x34, .half = 0x5678, .swapped = 0x65};
  const auto endian = big ? large : little;
  expected_ram[1] = swap_byte;
  expected_ram[5] = std::byte(endian.byte);
  registers.r[0] += 1;
  registers.r[2] = original_word;
  registers.r[3] = endian.byte;
  registers.r[4] = endian.half;
  registers.r[5] = registers.r[3]; // LDRSB at odd byte address.
  registers.r[6] = original_word;
  registers.r[7] = endian.swapped;
  registers.r[15] = code.pc();
  const auto result = machine.run(instructions.size());
  fixture::require(machine.core->registers() == registers && machine.ram == expected_ram &&
                       result.progress == armv5::Progress{.instructions = 8, .base_cycles = 10},
                   "A-bit permits aligned words/halfwords and arbitrary bytes/SWPB operands");
}

void predicated_alignment(bool big) {
  fixture::Program code(entry, big);
  constexpr std::uint32_t load_eq = 0x05902000;
  constexpr std::uint32_t swap_eq = 0x01002091;
  code.word(load_eq); // LDREQ R2,[R0] with Z clear: no alignment check.
  code.word(swap_eq); // SWPEQ R2,R1,[R0].
  armv5::Registers::System::Mapping mapping;
  mapping.control |= 2 | (big ? 0x80 : 0);
  fixture::Modes machine(
      {{.program = code, .modes = {fixture::supervisor_mode}}}, {}, true, 0x10000, {}, mapping);
  auto expected = machine.core->registers();
  expected.r[0] = data_address + 1;
  expected.system.data_fault_status = stale_status;
  expected.system.fault_address = stale_address;
  machine.core->set_registers(expected);
  expected.r[15] = code.pc();
  const auto ram = machine.ram;
  const auto result = machine.run(2);
  fixture::require(machine.core->registers() == expected && machine.ram == ram &&
                       result.progress == armv5::Progress{.instructions = 2, .base_cycles = 2},
                   "false predicates suppress alignment faults and preserve fault registers");
}

struct PcLoadCase {
  std::uint32_t instruction;
  bool thumb, exchanges;
  unsigned base;
  unsigned cycles;
};

void pc_load_case(PcLoadCase test, bool fixed, bool big, bool l4) {
  constexpr std::uint32_t destination = 0x1100;
  // A load would change instruction set when L4=0. BX/BLX always change it.
  const bool next_thumb = test.exchanges || (test.thumb == l4);
  const auto target = destination | (test.thumb ? 0U : 1U);
  fixture::Program code(entry, big);
  fixture::Program next(destination, big);
  if (test.thumb) {
    code.half(static_cast<std::uint16_t>(test.instruction));
  } else {
    code.word(test.instruction);
  }
  if (next_thumb) {
    next.half(thumb_move_r7);
  } // MOVS R7,#3.
  else {
    next.word(fixture::data(13, false, 7, 0, true, 3));
  }
  armv5::Registers::System::Mapping mapping;
  mapping.control |= (l4 ? 0x8000 : 0) | (big ? 0x80 : 0);
  fixture::Modes machine({{.program = code,
                           .modes = {fixture::supervisor_mode},
                           .instruction_set = test.thumb ? armv5::CodeSpan::InstructionSet::thumb
                                                         : armv5::CodeSpan::InstructionSet::arm},
                          {.program = next,
                           .modes = {fixture::supervisor_mode},
                           .instruction_set = next_thumb ? armv5::CodeSpan::InstructionSet::thumb
                                                         : armv5::CodeSpan::InstructionSet::arm}},
                         {},
                         fixed,
                         0x10000,
                         {},
                         mapping);
  auto expected = machine.core->registers();
  expected.cpsr = initial_flags | reset_status | (test.thumb ? 0x20 : 0);
  expected.r[test.base] = test.exchanges ? target : data_address;
  machine.core->set_registers(expected);
  fixture::put(std::span(machine.ram).first(4), target, big);
  const auto ram = machine.ram;
  expected.r[15] = destination;
  expected.cpsr = initial_flags | reset_status | (next_thumb ? 0x20 : 0);
  if (test.exchanges) {
    if (test.instruction == blx_r0) { expected.r[14] = code.pc(); }
  } else {
    expected.r[test.base] += 4;
  }
  const auto result = machine.run(1);
  fixture::require(machine.core->registers() == expected && machine.ram == ram &&
                       result.progress ==
                           armv5::Progress{.instructions = 1, .base_cycles = test.cycles},
                   "L4 inhibits LDR/LDM/POP interworking; BX/BLX keep their own semantics");
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[7] == 3 &&
                       machine.core->registers().r[15] == destination + (next_thumb ? 2 : 4),
                   "L4 destination executes its declared ARM/Thumb instruction");
}

void load_pc(bool fixed, bool big, bool l4) {
  constexpr std::array cases{
      PcLoadCase{
          .instruction = 0xe490f004, .thumb = false, .exchanges = false, .base = 0, .cycles = 5},
      PcLoadCase{
          .instruction = 0xe8b08000, .thumb = false, .exchanges = false, .base = 0, .cycles = 5},
      PcLoadCase{.instruction = 0xbd00, .thumb = true, .exchanges = false, .base = 13, .cycles = 5},
      PcLoadCase{
          .instruction = 0xe12fff10, .thumb = false, .exchanges = true, .base = 0, .cycles = 3},
      PcLoadCase{
          .instruction = 0xe12fff30, .thumb = false, .exchanges = true, .base = 0, .cycles = 3}};
  for (const auto test : cases) { pc_load_case(test, fixed, big, l4); }
}
} // namespace

void memory_control() {
  for (const bool fixed : {false, true}) {
    for (const bool big : {false, true}) {
      alignment_faults(fixed, big);
      aligned_and_byte_accesses(fixed, big);
      for (const bool l4 : {false, true}) { load_pc(fixed, big, l4); }
    }
  }
  predicated_alignment(false);
  predicated_alignment(true);
}
