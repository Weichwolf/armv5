// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/mmu.hpp"
#include "support/modes.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace {
constexpr std::uint32_t caches = 0x5004;
constexpr std::uint32_t process = 0x04000000;
constexpr std::uint32_t code_section = 0x00100c12;
constexpr std::uint32_t maintenance_pc = 0x1004;
constexpr std::uint32_t successor = 0x1008;
constexpr std::uint32_t initial_flags = 0x08000013;
constexpr std::uint32_t added_flags = 0x98000013;
constexpr std::uint32_t positive_rail = 0x7fffffff;
constexpr std::uint32_t negative_rail = 0x80000000;

struct Invalidation {
  std::uint32_t word, operand;
  bool affected;
};

struct Context {
  bool big, mmu, declared;
};

void invalidate(Invalidation operation, Context context) {
  constexpr std::uint32_t adds = 0xe2944001;
  fixture::Program code(0x1000, context.big);
  code.word(adds);
  code.word(operation.word);
  code.word(fixture::data(13, false, 5, 0, true, 9));
  fixture::MmuMachine machine(
      {{.program = code, .modes = {fixture::supervisor_mode}}}, {}, context.big);
  machine.context.control |= caches;
  if (context.mmu) {
    machine.context.process_id = process;
    machine.first(64, code_section);
  } else {
    machine.context.control &= ~std::uint32_t{1};
    machine.regions[0].address = code.address();
    machine.layout[0].address = code.address();
    machine.core->set_memory({.regions = machine.regions, .generation = 1});
  }
  machine.entries.push_back(successor);
  const std::array transitions{
      armv5::CodeTransition{.source_variant = 0, .destination_variant = 0, .address = successor}};
  machine.prepare(true,
                  context.declared ? std::span<const armv5::CodeTransition>(transitions)
                                   : std::span<const armv5::CodeTransition>{});
  auto registers = machine.core->registers();
  registers.cpsr = initial_flags;
  registers.r[0] = operation.operand;
  registers.r[4] = positive_rail;
  machine.core->set_registers(registers);
  auto expected = machine.core->state();
  expected.registers.r[4] = negative_rail;
  expected.registers.r[15] = successor;
  expected.registers.cpsr = added_flags;
  expected.progress = {.instructions = 2, .base_cycles = 2};
  const auto ram = machine.data;
  const auto result = machine.core->run(2);
  if (operation.affected) {
    if (context.declared) {
      expected.execution = armv5::ExecutionState::transfer;
      expected.transition_pending = true;
      expected.transition = transitions[0];
    } else {
      expected.execution = armv5::ExecutionState::fault;
      expected.fault = result.fault;
      fixture::require(result.fault.kind == armv5::Fault::Kind::changed_code &&
                           result.fault.address == maintenance_pc,
                       "undeclared ICache invalidation identifies its guest maintenance PC");
    }
  }
  fixture::require(machine.core->state() == expected && result.progress == expected.progress &&
                       machine.data == ram,
                   "ICache invalidation revokes affected prepared ranges after its exact "
                   "retirement without changing consumer bytes or executing the successor");
  const auto saved = machine.core->save();
  if (operation.affected && !context.declared) {
    fixture::require(machine.core->run(1).progress == armv5::Progress{},
                     "a missing invalidation transition remains a latched fault");
    return;
  }
  (void)machine.core->install(machine.prepared(), 0);
  fixture::require(machine.core->state() == expected,
                   "revalidation has no guest state/time effects");
  (void)machine.core->run(1);
  const auto after = machine.core->state();
  fixture::require(after.registers.r[5] == 9 && after.registers.r[15] == code.pc() &&
                       after.progress == armv5::Progress{.instructions = 3, .base_cycles = 3},
                   "revalidated successor executes its prepared word");
  machine.core->restore(saved);
  (void)machine.core->run(1);
  fixture::require(machine.core->state() == after && machine.data == ram,
                   "snapshot restores the exact pending invalidation transition");
}
} // namespace

void cache_lifecycle() {
  constexpr std::uint32_t invalidate_all = 0xee070f15;
  constexpr std::uint32_t invalidate_both = 0xee070f17;
  constexpr std::uint32_t invalidate_mva = 0xee070f35;
  constexpr std::uint32_t invalidate_set = 0xee070f55;
  constexpr std::uint32_t invalidate_equal = 0x0e070f15;
  constexpr std::uint32_t next_line = 0x1020;
  constexpr std::uint32_t last_way = 0xc0000000;
  constexpr std::uint32_t upper_index = 0x1800;
  for (const bool big : {false, true}) {
    for (const bool mmu : {false, true}) {
      const auto mva = (mmu ? process : 0) | 0x1000;
      const std::array operations{
          Invalidation{.word = invalidate_all, .operand = 0, .affected = true},
          Invalidation{.word = invalidate_both, .operand = 0, .affected = true},
          Invalidation{.word = invalidate_mva, .operand = mva, .affected = true},
          Invalidation{.word = invalidate_mva, .operand = mva + 32, .affected = false},
          Invalidation{.word = invalidate_set, .operand = last_way, .affected = true},
          Invalidation{.word = invalidate_set, .operand = next_line, .affected = false},
          Invalidation{.word = invalidate_set, .operand = upper_index, .affected = false},
          Invalidation{.word = invalidate_equal, .operand = 0, .affected = false}};
      for (const auto operation : operations) {
        for (const bool declared : {false, true}) {
          invalidate(operation, {.big = big, .mmu = mmu, .declared = declared});
        }
      }
      if (mmu) {
        invalidate({.word = invalidate_mva, .operand = 0x1000, .affected = false},
                   {.big = big, .mmu = true, .declared = false});
      }
    }
  }
}
