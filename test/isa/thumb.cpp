// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "support/arithmetic.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
constexpr std::uint32_t thumb_status = 0x30000033, nz_mask = 0xc0000000, carry_mask = 0x20000000,
                        thumb_bit = 0x20, low_ram = 0x4000;
constexpr armv5::CompileOptions options{
    .instruction_set = armv5::CompileOptions::InstructionSet::thumb, .callbacks = {}};
constexpr auto thumb_set = armv5::CodeSpan::InstructionSet::thumb;

constexpr std::uint32_t move_255 = 0x20ff, add_one = 0x3001, subtract_two = 0x3802,
                        compare_254 = 0x28fe, immediate_answer = 254, completed_status = 0x60000033,
                        low_alu_r0_r1 = 0x4008, vq_status = 0x18000033, status_high = 0xffffff00,
                        thumb_supervisor = 0x33, word_mask = 0xffffffff, count_mask = 31,
                        memory_input = 0x8123ff80, memory_store = 0x87654321,
                        literal_address = 0x3ffa, load_literal = 0x4801, literal_value = 0x12345678,
                        move_pc = 0x4687, unreachable_first = 0x2163, unreachable_second = 0x214d,
                        target_move = 0x212a, target_value = 42, push_four_lr = 0xb50f,
                        pop_four = 0xbcf0, store_three = 0xc00e, link_value = 99,
                        popped_offset = 0xfc, halfword_address = 0x1002, adr_result = 0x1008,
                        sp_result = 0x43fc, thumb_system = 0x3f, breakpoint = 0xbe00,
                        bl_prefix = 0xf000, blx_suffix = 0xe87e, bl_suffix = 0xf87e,
                        add_r4 = 0x3401, callee_address = 0x2100, arm_return = 0xe12fff1e,
                        add_r3 = 0x3301, thumb_return = 0x4770, irq_vector = 0x18,
                        irq_return = 0xe25ef004, arm_blx_immediate = 0xfa0003fe,
                        arm_blx_r0 = 0xe12fff30, add_r2 = 0x3201, move_r0_one = 0x2001,
                        move_r1_two = 0x2102, move_r1_one = 0x2101, conditional_branch = 0xd000;

void low_arithmetic() {
  struct Case {
    std::uint16_t word;
    std::uint32_t first, second, result, status;
  };

  constexpr std::array cases{Case{.word = 0x0008,
                                  .first = 0x80000001,
                                  .second = 0,
                                  .result = 0x80000001,
                                  .status = 0xb0000033}, // LSLS R0,R1,#0
                             Case{.word = 0x0808,
                                  .first = 0x80000001,
                                  .second = 0,
                                  .result = 0,
                                  .status = 0x70000033}, // LSRS #32
                             Case{.word = adr_result,
                                  .first = 0x80000001,
                                  .second = 0,
                                  .result = word_mask,
                                  .status = 0xb0000033}, // ASRS #32
                             Case{.word = 0x1c48,
                                  .first = 0x7fffffff,
                                  .second = 0,
                                  .result = 0x80000000,
                                  .status = 0x90000033}, // ADDS R0,R1,#1
                             Case{.word = 0x1e48,
                                  .first = 0x80000000,
                                  .second = 0,
                                  .result = 0x7fffffff,
                                  .status = 0x30000033}, // SUBS R0,R1,#1
                             Case{.word = 0x4148,
                                  .first = word_mask,
                                  .second = 1,
                                  .result = 1,
                                  .status = 0x20000033}, // ADCS R0,R1 (R0 = second)
                             Case{.word = 0x4188,
                                  .first = word_mask,
                                  .second = 0,
                                  .result = 1,
                                  .status = thumb_supervisor}, // SBCS R0,R1
                             Case{.word = 0x4248,
                                  .first = 0x80000000,
                                  .second = 0,
                                  .result = 0x80000000,
                                  .status = 0x90000033}, // NEGS R0,R1
                             Case{.word = 0x4348,
                                  .first = 0x80000000,
                                  .second = 2,
                                  .result = 0,
                                  .status = 0x70000033}, // MULS R0,R1
                             Case{.word = 0x43c8,
                                  .first = 0,
                                  .second = 0,
                                  .result = word_mask,
                                  .status = 0xb0000033}}; // MVNS R0,R1
  for (const auto &item : cases) {
    fixture::Program code;
    code.half(item.word);
    fixture::Machine machine(code);
    machine.prepare(options);
    auto input = machine.core->registers();
    input.cpsr = thumb_status;
    input.r[0] = item.second;
    input.r[1] = item.first;
    auto expected = input;
    expected.r[0] = item.result;
    expected.cpsr = item.status;
    expected.r[15] += 2;
    machine.core->set_registers(input);
    (void)machine.run(1);
    fixture::require(machine.core->registers() == expected, "Thumb low arithmetic/shifter flags");
  }
  fixture::Program immediate;
  immediate.half(move_255);     // MOVS R0,#fixture::byte_mask
  immediate.half(add_one);      // ADDS R0,#1
  immediate.half(subtract_two); // SUBS R0,#2
  immediate.half(compare_254);  // CMP R0,#immediate_answer
  fixture::Machine machine(immediate);
  machine.prepare(options);
  auto registers = machine.core->registers();
  registers.cpsr = thumb_status;
  machine.core->set_registers(registers);
  (void)machine.run(4);
  fixture::require(machine.core->registers().r[0] == immediate_answer &&
                       machine.core->registers().cpsr == completed_status,
                   "Thumb immediate MOV/ADD/SUB/CMP and halfword continuation");
}

fixture::Arithmetic
low_answer(unsigned opcode, std::uint32_t first, std::uint32_t second, bool carry) {
  if (opcode == 9) { return fixture::expected(3, {second, 0}, carry); }
  if (opcode == 13) { return fixture::expected(13, {0, first * second}, carry); }
  return fixture::expected(opcode, {first, second}, carry);
}

void all_low_alu() {
  constexpr std::array values{0U, 1U, 0x7fffffffU, 0x80000000U, word_mask};
  for (const unsigned opcode : {0U, 1U, 5U, 6U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U}) {
    const auto cycles = opcode == 13 ? 4U : 1U;
    fixture::Program code;
    code.half(static_cast<std::uint16_t>(low_alu_r0_r1 | (opcode << 6)));
    fixture::Machine machine(code);
    machine.prepare(options);
    for (const auto first : values) {
      for (const auto second : values) {
        for (const bool carry : {false, true}) {
          auto registers = machine.core->registers();
          registers.r[0] = first;
          registers.r[1] = second;
          registers.r[15] = code.address();
          registers.cpsr = vq_status | (carry ? carry_mask : 0);
          const auto answer = low_answer(opcode, first, second, carry);
          auto expected = registers;
          if (opcode != 8 && opcode != 10 && opcode != 11) { expected.r[0] = answer.result; }
          expected.r[15] += 2;
          expected.cpsr = (answer.flags & status_high) | thumb_supervisor;
          machine.core->set_registers(registers);
          const auto result = machine.run(1);
          fixture::require(machine.core->registers() == expected &&
                               result.progress.base_cycles == cycles,
                           "all Thumb low ALU numeric boundaries and untouched registers");
        }
      }
    }
  }
}

std::pair<std::uint32_t, bool> shift_answer(unsigned kind,
                                            std::pair<std::uint32_t, unsigned> operands) {
  const auto [input, amount] = operands;
  auto answer = input;
  bool carry = true;
  if (amount != 0) {
    if (kind == 0) {
      answer = amount < 32 ? std::uint32_t(std::uint64_t(input) << amount) : 0;
      carry = amount <= 32 && ((input >> (32 - amount)) & 1) != 0;
    }
    if (kind == 1) {
      answer = amount < 32 ? input >> amount : 0;
      carry = amount <= 32 && ((input >> (amount - 1)) & 1) != 0;
    }
    if (kind == 2) {
      answer = amount < 32 ? (input >> amount) | ~(word_mask >> amount) : word_mask;
      carry = amount >= 32 || ((input >> (amount - 1)) & 1) != 0;
    }
    if (kind == 3) {
      answer = std::rotr(input, int(amount & count_mask));
      carry = (answer & 0x80000000) != 0;
    }
  }
  return {answer, carry};
}

void register_shifts() {
  constexpr std::array counts{0U, 1U, count_mask, 32U, 33U, fixture::byte_mask, 256U};
  constexpr std::uint32_t input = 0x80000003;
  for (const unsigned kind : {0U, 1U, 2U, 3U}) {
    const auto opcode = kind == 3 ? 7U : kind + 2;
    fixture::Program program;
    program.half(std::uint16_t(low_alu_r0_r1 | (opcode << 6)));
    fixture::Machine machine(program);
    machine.prepare(options);
    for (const auto count : counts) {
      machine.core->reset(program.address());
      auto registers = machine.core->registers();
      registers.r[0] = input;
      registers.r[1] = count;
      registers.cpsr = thumb_status;
      const auto amount = count & fixture::byte_mask;
      const auto [answer, carry] = shift_answer(kind, {input, amount});
      const auto flags =
          (answer & 0x80000000) | (answer == 0 ? 0x40000000 : 0) | (carry ? carry_mask : 0);
      auto expected = registers;
      expected.r[0] = answer;
      expected.r[15] += 2;
      expected.cpsr = (registers.cpsr & ~(nz_mask | carry_mask)) | flags;
      machine.core->set_registers(registers);
      const auto result = machine.run(1);
      fixture::require(machine.core->registers() == expected && result.progress.base_cycles == 2,
                       "Thumb register shifts at zero/32/large counts");
    }
  }
}

struct MemoryCase {
  std::uint16_t word;
  unsigned width;
  bool reading, sign;
  unsigned offset = 4;
};

void verify_memory(const MemoryCase &item, bool big, bool fixed) {
  fixture::Program code(0x1000, big);
  code.half(item.word);
  fixture::Machine machine(code);
  auto selected = options;
  selected.big_endian = big;
  machine.prepare(selected, fixed);
  fixture::put(std::span(machine.ram).subspan(item.offset, 4), memory_input, big);
  auto expected_ram = machine.ram;
  auto registers = machine.core->registers();
  registers.cpsr = thumb_status;
  registers.r[0] = memory_store;
  registers.r[1] = low_ram;
  registers.r[2] = 4;
  registers.r[13] = low_ram;
  auto expected = registers;
  if (item.reading) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < item.width; ++index) {
      value |= std::to_integer<std::uint32_t>(machine.ram[item.offset + index])
               << ((big ? item.width - 1 - index : index) * 8);
    }
    if (item.sign && (value & (1U << (item.width * 8 - 1))) != 0) {
      value |= word_mask << (item.width * 8);
    }
    expected.r[0] = value;
  } else {
    for (unsigned index = 0; index < item.width; ++index) {
      expected_ram[item.offset + index] = std::byte(
          (registers.r[0] >> ((big ? item.width - 1 - index : index) * 8)) & fixture::byte_mask);
    }
  }
  expected.r[15] += 2;
  machine.core->set_registers(registers);
  (void)machine.run(1);
  fixture::require(machine.core->registers() == expected && machine.ram == expected_ram,
                   "Thumb register-offset width/sign/endian and fixed/dynamic memory");
}

void memory_forms() {
  constexpr std::array cases{
      MemoryCase{.word = 0x5088, .width = 4, .reading = false, .sign = false},
      MemoryCase{.word = 0x5288, .width = 2, .reading = false, .sign = false},
      MemoryCase{.word = 0x5488, .width = 1, .reading = false, .sign = false},
      MemoryCase{.word = 0x5688, .width = 1, .reading = true, .sign = true},
      MemoryCase{.word = 0x5888, .width = 4, .reading = true, .sign = false},
      MemoryCase{.word = 0x5a88, .width = 2, .reading = true, .sign = false},
      MemoryCase{.word = 0x5c88, .width = 1, .reading = true, .sign = false},
      MemoryCase{.word = 0x5e88, .width = 2, .reading = true, .sign = true}};
  constexpr std::array immediate_cases{
      MemoryCase{.word = 0x60c8, .width = 4, .reading = false, .sign = false, .offset = 12},
      MemoryCase{.word = 0x68c8, .width = 4, .reading = true, .sign = false, .offset = 12},
      MemoryCase{.word = 0x70c8, .width = 1, .reading = false, .sign = false, .offset = 3},
      MemoryCase{.word = 0x78c8, .width = 1, .reading = true, .sign = false, .offset = 3},
      MemoryCase{.word = 0x80c8, .width = 2, .reading = false, .sign = false, .offset = 6},
      MemoryCase{.word = 0x88c8, .width = 2, .reading = true, .sign = false, .offset = 6},
      MemoryCase{.word = 0x9003, .width = 4, .reading = false, .sign = false, .offset = 12},
      MemoryCase{.word = 0x9803, .width = 4, .reading = true, .sign = false, .offset = 12}};
  std::vector<MemoryCase> forms(cases.begin(), cases.end());
  forms.insert(forms.end(), immediate_cases.begin(), immediate_cases.end());
  for (const bool big : {false, true}) {
    for (const bool fixed : {false, true}) {
      for (const auto &item : forms) { verify_memory(item, big, fixed); }
    }
  }
  fixture::Program literal(literal_address);
  literal.half(load_literal); // LDR R0,[PC,#4]: aligned 0x3ffc + 4.
  fixture::Machine machine(literal);
  fixture::put(std::span(machine.ram).first(4), literal_value);
  machine.prepare(options);
  auto registers = machine.core->registers();
  registers.cpsr = thumb_status;
  machine.core->set_registers(registers);
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[0] == literal_value, "Thumb PC literal aligns PC+4");
}

void pc_and_multiple() {
  fixture::Program code;
  code.half(move_pc); // MOV PC,R0, no interworking.
  code.half(unreachable_first);
  code.half(unreachable_second);
  code.half(target_move);
  fixture::Machine machine(code);
  machine.prepare(options);
  auto registers = machine.core->registers();
  registers.cpsr = thumb_status;
  registers.r[0] = code.address() + 7;
  machine.core->set_registers(registers);
  (void)machine.run(2);
  fixture::require(machine.core->registers().r[1] == target_value &&
                       machine.core->registers().r[15] == code.pc() &&
                       (machine.core->registers().cpsr & thumb_bit) != 0,
                   "Thumb high MOV PC preserves halfword/state semantics");
  fixture::Program block;
  block.half(push_four_lr); // PUSH {R0..R3,LR}
  block.half(pop_four);     // POP {R4..R7}
  block.half(store_three);  // STMIA R0!,{R1..R3}
  fixture::Machine stack(block);
  stack.prepare(options);
  registers = stack.core->registers();
  registers.cpsr = thumb_status;
  registers.r[0] = low_ram;
  registers.r[1] = 11;
  registers.r[2] = 12;
  registers.r[3] = 13;
  registers.r[13] = low_ram + 0x100;
  registers.r[14] = link_value;
  stack.core->set_registers(registers);
  (void)stack.run(3);
  fixture::require(stack.core->registers().r[4] == low_ram && stack.core->registers().r[5] == 11 &&
                       stack.core->registers().r[6] == 12 && stack.core->registers().r[7] == 13 &&
                       stack.core->registers().r[13] == low_ram + popped_offset &&
                       stack.core->registers().r[0] == low_ram + 12 &&
                       fixture::get(std::span(stack.ram).subspan(8, 4)) == 13,
                   "Thumb PUSH/POP/STM ordering and writeback");
}

void multiple_quirks() {
  constexpr std::uint16_t load_with_base = 0xc90a;
  constexpr std::uint16_t pop_pc = 0xbd10;
  constexpr std::uint16_t nop = 0x46c0;
  fixture::Program load;
  load.half(load_with_base);
  fixture::Machine machine(load);
  machine.prepare(options);
  fixture::put(std::span(machine.ram).first(4), target_value);
  fixture::put(std::span(machine.ram).subspan(4, 4), link_value);
  auto registers = machine.core->registers();
  registers.cpsr = thumb_status;
  registers.r[1] = low_ram;
  auto expected = registers;
  expected.r[1] = target_value;
  expected.r[3] = link_value;
  expected.r[15] += 2;
  machine.core->set_registers(registers);
  (void)machine.run(1);
  fixture::require(machine.core->registers() == expected,
                   "Thumb LDM base in list suppresses writeback");
  for (const bool arm_destination : {false, true}) {
    fixture::Program pop;
    pop.half(pop_pc);
    fixture::Program destination(callee_address);
    if (arm_destination) {
      destination.word(fixture::data(13, false, 8, 0, false, 8));
    } else {
      destination.half(nop);
    }
    fixture::Modes stack(
        {{.program = pop, .modes = {fixture::supervisor_mode}, .instruction_set = thumb_set},
         {.program = destination,
          .modes = {fixture::supervisor_mode},
          .instruction_set = arm_destination ? armv5::CodeSpan::InstructionSet::arm : thumb_set}});
    fixture::put(std::span(stack.ram).first(4), target_value);
    fixture::put(std::span(stack.ram).subspan(4, 4),
                 destination.address() | (arm_destination ? 0U : 1U));
    registers = stack.core->registers();
    registers.cpsr = thumb_status;
    registers.r[13] = low_ram;
    expected = registers;
    expected.r[4] = target_value;
    expected.r[13] += 8;
    expected.r[15] = destination.address();
    if (arm_destination) { expected.cpsr &= ~thumb_bit; }
    stack.core->set_registers(registers);
    const auto result = stack.run(1);
    fixture::require(stack.core->registers() == expected && result.progress.base_cycles == 6,
                     "Thumb POP PC interworking/load order at an exact instruction bound");
  }
}

void addressing_and_conditions() {
  fixture::Program program(halfword_address);
  for (const auto word : {0xa001, 0xa9ff, 0xb081, 0xb002}) {
    program.half(static_cast<std::uint16_t>(word));
  }
  fixture::Machine machine(program);
  machine.prepare(options);
  auto registers = machine.core->registers();
  registers.cpsr = thumb_status;
  registers.r[13] = low_ram;
  machine.core->set_registers(registers);
  (void)machine.run(4);
  fixture::require(machine.core->registers().r[0] == adr_result &&
                       machine.core->registers().r[1] == sp_result &&
                       machine.core->registers().r[13] == low_ram + 4 &&
                       machine.core->registers().cpsr == thumb_status,
                   "Thumb ADR, SP-relative address and stack adjustment preserve flags");
  constexpr std::uint16_t compare_pc = 0x4578;
  fixture::Program compare(halfword_address);
  compare.half(compare_pc);
  fixture::Machine pc(compare);
  pc.prepare(options);
  registers = pc.core->registers();
  registers.r[0] = compare.address() + 4;
  registers.cpsr = thumb_status;
  pc.core->set_registers(registers);
  (void)pc.run(1);
  fixture::require(pc.core->registers().cpsr == completed_status,
                   "Thumb CMP permits PC only as its second operand, reading PC+4");
  constexpr std::array<std::uint16_t, 14> masks{0xf0f0,
                                                0x0f0f,
                                                0xcccc,
                                                0x3333,
                                                0xff00,
                                                0x00ff,
                                                0xaaaa,
                                                0x5555,
                                                0x0c0c,
                                                0xf3f3,
                                                0xaa55,
                                                0x55aa,
                                                0x0a05,
                                                0xf5fa};
  for (unsigned condition = 0; condition < masks.size(); ++condition) {
    fixture::Program code;
    code.half(static_cast<std::uint16_t>(conditional_branch | (condition << 8)));
    code.half(move_r0_one);
    code.half(move_r1_two);
    fixture::Machine branch(code);
    branch.prepare(options);
    for (unsigned flags = 0; flags < 16; ++flags) {
      auto input = branch.core->registers();
      input.cpsr = (flags << fixture::condition_shift) | thumb_supervisor;
      input.r[15] = code.address();
      branch.core->set_registers(input);
      const auto result = branch.run(1);
      const bool taken = (masks[condition] & (1U << flags)) != 0;
      input.r[15] += taken ? 4 : 2;
      fixture::require(branch.core->registers() == input &&
                           result.progress.base_cycles == (taken ? 3 : 1),
                       "all Thumb conditional branches against independent NZCV truth masks");
    }
  }
}

void exceptions_and_refusals() {
  struct Exception {
    std::uint16_t word;
    std::uint32_t vector, mode, return_word;
  };

  constexpr std::array exceptions{
      Exception{
          .word = 0xdf82, .vector = 8, .mode = fixture::supervisor_mode, .return_word = 0xe1b0f00e},
      Exception{.word = 0xde00, .vector = 4, .mode = 0x1b, .return_word = 0xe1b0f00e},
      Exception{.word = breakpoint, .vector = 12, .mode = 0x17, .return_word = 0xe25ef002}};
  for (const auto &item : exceptions) {
    fixture::Program program;
    program.half(item.word);
    program.half(move_r1_one);
    fixture::Program handler(item.vector);
    handler.word(item.return_word);
    fixture::Modes machine(
        {{.program = program, .modes = {fixture::system_mode}, .instruction_set = thumb_set},
         {.program = handler, .modes = {item.mode}}},
        {item.vector});
    auto registers = machine.core->registers();
    registers.cpsr = thumb_system;
    machine.core->set_registers(registers);
    (void)machine.run(1);
    fixture::require(machine.core->registers().spsr == registers.cpsr &&
                         machine.core->registers().r[14] ==
                             program.address() + (item.word == breakpoint ? 4 : 2) &&
                         machine.core->registers().r[15] == item.vector,
                     "Thumb exception entry preserves Thumb SPSR and precise LR");
    (void)machine.run(2);
    fixture::require(machine.core->registers().r[1] == 1 &&
                         machine.core->registers().r[15] == program.pc() &&
                         machine.core->registers().cpsr == registers.cpsr,
                     "Thumb SWI/UDF/BKPT ARM handler return");
  }
  constexpr std::array<std::uint16_t, 12> invalid{bl_prefix,
                                                  0xf800,
                                                  0xe800,
                                                  0xc800,
                                                  0xb400,
                                                  0xbc00,
                                                  0x4400,
                                                  0x4500,
                                                  0x4600,
                                                  0x47f8,
                                                  0xbf00,
                                                  0x4587};
  for (const auto word : invalid) {
    fixture::Program program;
    program.half(word);
    fixture::Machine machine(program);
    bool refused = false;
    try {
      (void)armv5::Core::lower(machine.image(), options);
    } catch (const std::runtime_error &error) {
      fixture::contains(error.what(), "authored-arm-fixture at 0x00001000");
      refused = true;
    }
    fixture::require(refused, "invalid Thumb encoding or BL pair accepted");
  }
}

void overflowing_thumb_link() {
  constexpr std::array addresses{0U, 0xfffffffcU};
  constexpr std::uint16_t negative_prefix = 0xf7ff;
  constexpr std::uint16_t suffix = 0xf800;
  for (const auto address : addresses) {
    fixture::Program program(address);
    program.half(address == 0 ? negative_prefix : bl_prefix);
    program.half(suffix);
    fixture::Machine machine(program);
    bool refused = false;
    try {
      (void)armv5::Core::lower(machine.image(), options);
    } catch (const std::runtime_error &error) {
      fixture::contains(error.what(), "unpredictable Thumb BL address overflow");
      refused = true;
    }
    fixture::require(refused, "Thumb BL cannot wrap past either address-space boundary");
  }
}

void shared_arm_thumb_bytes() {
  fixture::Program program;
  program.word(
      fixture::data(13, false, 0, 0, false, 0)); // ARM MOV R0,R0; Thumb LSLS R0,R0,#0 then B.
  auto storage = program.bytes();
  const std::array spans{armv5::CodeSpan{.address = program.address(),
                                         .bytes = storage,
                                         .processor_modes = {},
                                         .instruction_set = armv5::CodeSpan::InstructionSet::arm},
                         armv5::CodeSpan{.address = program.address(),
                                         .bytes = storage,
                                         .processor_modes = {},
                                         .instruction_set = thumb_set}};
  const std::array entries{program.address()};
  const std::array variants{armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries}};
  const armv5::CodeImage image{.identity = "authored-shared-arm-thumb-bytes",
                               .variants = variants,
                               .layouts = {},
                               .transitions = {},
                               .interrupt_vectors = {}};
  const auto native = armv5::Core::compile(image);
  constexpr std::uint32_t shared_value = 0x80000001;
  constexpr std::uint32_t arm_v_status = 0x10000013;
  const std::array regions{armv5::Memory::Region{.address = program.address(),
                                                 .bytes = storage,
                                                 .readable = true,
                                                 .writable = false,
                                                 .executable = true}};
  armv5::Core core({.regions = regions, .generation = 0});
  (void)core.install(native, 0);
  for (const bool thumb : {false, true}) {
    core.reset(program.address());
    auto registers = core.registers();
    registers.r[0] = shared_value;
    registers.cpsr = arm_v_status | (thumb ? thumb_bit : 0);
    core.set_registers(registers);
    (void)core.run(1);
    auto expected = registers;
    expected.r[15] += thumb ? 2 : 4;
    if (thumb) { expected.cpsr |= 0x80000000; }
    fixture::require(
        core.registers() == expected,
        "coherent overlapping ARM/Thumb declarations select distinct semantics for the same bytes");
  }
}

void forbidden_interworking_suffix() {
  fixture::Program arm;
  arm.word(arm_blx_r0);
  fixture::Program thumb(0x2000);
  thumb.half(bl_prefix);
  thumb.half(bl_suffix);
  fixture::Modes machine(
      {{.program = arm, .modes = {fixture::supervisor_mode}},
       {.program = thumb, .modes = {fixture::supervisor_mode}, .instruction_set = thumb_set}});
  auto registers = machine.core->registers();
  registers.r[0] = thumb.address() + 3;
  machine.core->set_registers(registers);
  const auto result = machine.core->run(1);
  fixture::require(result.fault.kind == armv5::Fault::Kind::unsupported &&
                       result.progress.instructions == 1 &&
                       machine.core->registers().r[15] == thumb.address() + 2,
                   "ARM interworking cannot enter a Thumb BL suffix, including at a run bound");
}

void interworking_and_pairs() {
  for (const bool exchange : {false, true}) {
    fixture::Program caller(0x2000);
    caller.half(bl_prefix);
    caller.half(exchange ? blx_suffix : bl_suffix);
    caller.half(add_r4); // ADDS R4,#1 after return
    fixture::Program callee(callee_address);
    if (exchange) {
      callee.word(fixture::data(4, false, 3, 3, true, 1));
      callee.word(arm_return);
    } else {
      callee.half(add_r3);
      callee.half(thumb_return);
    }
    fixture::Program vector(irq_vector);
    vector.word(irq_return);
    fixture::Modes machine(
        {{.program = caller, .modes = {fixture::supervisor_mode}, .instruction_set = thumb_set},
         {.program = callee,
          .modes = {fixture::supervisor_mode},
          .instruction_set = exchange ? armv5::CodeSpan::InstructionSet::arm : thumb_set},
         {.program = vector, .modes = {fixture::irq_mode}}},
        {irq_vector});
    auto registers = machine.core->registers();
    registers.cpsr = thumb_supervisor;
    machine.core->set_registers(registers);
    (void)machine.run(1);
    fixture::require(machine.core->registers().r[14] == caller.address() + 4 &&
                         machine.core->registers().r[15] == caller.address() + 2,
                     "Thumb BL prefix is an independently retired half");
    const auto prefix = machine.core->save();
    const auto prefix_state = machine.core->state();
    machine.core->interrupt({});
    (void)machine.run(5);
    fixture::require(machine.core->registers().r[3] == 1 && machine.core->registers().r[4] == 1 &&
                         machine.core->registers().r[14] == caller.address() + 5 &&
                         machine.core->registers().r[15] == caller.pc() &&
                         machine.core->registers().cpsr == thumb_supervisor,
                     "IRQ between Thumb BL/BLX halves and state-changing returns");
    machine.core->restore(prefix);
    fixture::require(machine.core->state() == prefix_state,
                     "Thumb BL prefix snapshot restores all interrupted state");
    const auto result = machine.run(4);
    fixture::require(result.progress.base_cycles == 8 && machine.core->registers().r[3] == 1 &&
                         machine.core->registers().r[4] == 1,
                     "Thumb BL/BLX suffix continuation and exact nominal costs");
  }
  for (const bool immediate : {false, true}) {
    fixture::Program caller;
    caller.word(immediate ? arm_blx_immediate : arm_blx_r0);
    caller.word(fixture::data(4, false, 1, 1, true, 1));
    fixture::Program callee(0x2000);
    callee.half(add_r2);
    callee.half(thumb_return);
    fixture::Modes machine(
        {{.program = caller, .modes = {fixture::supervisor_mode}},
         {.program = callee, .modes = {fixture::supervisor_mode}, .instruction_set = thumb_set}});
    auto registers = machine.core->registers();
    registers.r[0] = callee.address() | 1;
    machine.core->set_registers(registers);
    (void)machine.run(4);
    fixture::require(machine.core->registers().r[1] == 1 && machine.core->registers().r[2] == 1 &&
                         machine.core->registers().r[14] == caller.address() + 4 &&
                         machine.core->registers().r[15] == caller.pc() &&
                         (machine.core->registers().cpsr & thumb_bit) == 0,
                     "ARM BLX immediate/register and Thumb BX interworking");
  }
}
}

void thumb_instructions() {
  low_arithmetic();
  all_low_alu();
  register_shifts();
  memory_forms();
  pc_and_multiple();
  multiple_quirks();
  interworking_and_pairs();
  addressing_and_conditions();
  exceptions_and_refusals();
  forbidden_interworking_suffix();
  shared_arm_thumb_bytes();
  overflowing_thumb_link();
}
