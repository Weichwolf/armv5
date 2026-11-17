// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace {
constexpr std::uint32_t entry = 0x1000, vector = 0x10, stale_data = 0x55, stale_instruction = 0xaa,
                        stale_address = 0x12345678, initial_flags = 0x98000000,
                        abort_flags = 0x98000097, arm_nop = 0xe1a08008, thumb_nop = 0x46c0;
constexpr std::array modes{fixture::user_mode,
                           fixture::fiq_mode,
                           fixture::irq_mode,
                           fixture::supervisor_mode,
                           fixture::abort_mode,
                           fixture::undefined_mode,
                           fixture::system_mode};
constexpr std::array<std::uint32_t, 4> data_words{0xdeadbeef, 0x12345678, 0x89abcdef, 0x13579bdf};
constexpr std::array<std::uint32_t, 4> store_words{0xaabbccdd, 0x11223344, 0x55667788, 0x99aabbcc};

enum class Operation : std::uint8_t { load, store, swap };
enum class Failure : std::uint8_t { absent, read_denied, write_denied, callback };

struct Instruction {
  std::uint32_t word;
  unsigned words;
  Operation operation;
  bool thumb = false;
  unsigned width = 4;
};

struct FaultCase {
  std::uint32_t address, far;
  unsigned beat = 0;
};

struct Refusal {
  armv5::Core *cpu = nullptr;
  unsigned calls = 0, width = 0;
  std::uint32_t address = 0;
  armv5::State prefix;

  static bool
  read(void *owner, std::uint32_t address, unsigned width, std::uint32_t & /*value*/) noexcept {
    if ((width != 1 && width != 2 && width != 4) || address % width != 0) { return false; }
    auto &self = *static_cast<Refusal *>(owner);
    self.prefix = self.cpu->state();
    self.address = address;
    self.width = width;
    ++self.calls;
    return false;
  }

  static bool
  write(void *owner, std::uint32_t address, unsigned width, std::uint32_t value) noexcept {
    return read(owner, address, width, value);
  }
};

class Machine {
public:
  std::vector<std::byte> code, handler, data, original;
  std::vector<armv5::Memory::Region> regions;
  std::unique_ptr<armv5::Core> core;
  std::optional<armv5::NativeImage> native;
  Refusal refusal;
  bool big;

  Machine(Instruction instruction, FaultCase fault, Failure failure, bool fixed, bool big_endian)
      : data(std::size_t(instruction.words) * 4), big(big_endian) {
    fixture::Program source(entry, big);
    fixture::Program target(vector, big);
    if (instruction.thumb) {
      source.half(thumb_nop);
      source.half(static_cast<std::uint16_t>(instruction.word));
    } else {
      source.word(arm_nop);
      source.word(instruction.word);
    }
    constexpr std::array reads{0xee159f10U, 0xee16af10U, 0xee15bf30U}; // DFSR/FAR/IFSR to R9..R11.
    for (const auto word : reads) { target.word(word); }
    target.word(fixture::data(2, true, 15, 14, true, instruction.thumb ? 6 : 4));
    code = source.bytes();
    handler = target.bytes();
    regions = {
        {.address = entry, .bytes = code, .readable = true, .writable = false, .executable = true},
        {.address = vector,
         .bytes = handler,
         .readable = true,
         .writable = false,
         .executable = true}};
    for (unsigned index = 0; index < instruction.words; ++index) {
      fixture::put(std::span(data).subspan(std::size_t(index) * 4, 4), data_words[index], big);
    }
    original = data;
    const auto first = fault.address & ~std::uint32_t{3};
    const auto failed = fault.beat * 4;
    const auto bytes = std::span(data);
    if (failed != 0) {
      regions.push_back({.address = first, .bytes = bytes.first(failed), .writable = true});
    }
    if (failure == Failure::read_denied || failure == Failure::write_denied) {
      regions.push_back({.address = first + failed,
                         .bytes = bytes.subspan(failed, 4),
                         .readable = failure != Failure::read_denied,
                         .writable = failure != Failure::write_denied});
    }
    if (failed + 4 < data.size()) {
      regions.push_back(
          {.address = first + failed + 4, .bytes = bytes.subspan(failed + 4), .writable = true});
    }
    const auto io = failure == Failure::callback ? armv5::Io{.owner = &refusal,
                                                             .read = &Refusal::read,
                                                             .write = &Refusal::write,
                                                             .swap = &Refusal::read}
                                                 : armv5::Io{};
    core = std::make_unique<armv5::Core>(armv5::Memory{.regions = regions, .generation = 0}, io);
    refusal.cpu = core.get();
    auto initial = core->registers();
    initial.r[15] = entry;
    initial.system.mapping.control |= big ? 0x80 : 0;
    core->set_registers(initial);
    const std::array<std::uint32_t, 1> abort_mode{fixture::abort_mode};
    const std::array spans{
        armv5::CodeSpan{.address = entry,
                        .bytes = code,
                        .processor_modes = modes,
                        .instruction_set = instruction.thumb
                                               ? armv5::CodeSpan::InstructionSet::thumb
                                               : armv5::CodeSpan::InstructionSet::arm},
        armv5::CodeSpan{.address = vector, .bytes = handler, .processor_modes = abort_mode}};
    const std::array<std::uint32_t, 2> entries{entry, vector};
    const std::array variants{armv5::CodeVariant{
        .id = 0, .spans = spans, .entries = entries, .mapping = initial.system.mapping}};
    std::vector<armv5::RegionLayout> layout;
    layout.reserve(regions.size());
    for (const auto &region : regions) {
      layout.push_back({.address = region.address,
                        .size = region.bytes.size(),
                        .readable = region.readable,
                        .writable = region.writable,
                        .executable = region.executable});
    }
    const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = layout}};
    const std::array vectors{vector};
    const armv5::CodeImage image{.identity = "authored-external-aborts",
                                 .variants = variants,
                                 .layouts = fixed ? std::span<const armv5::VariantLayout>(layouts)
                                                  : std::span<const armv5::VariantLayout>{},
                                 .transitions = {},
                                 .interrupt_vectors = vectors};
    native.emplace(armv5::Core::compile(image));
    (void)core->install(*native, 0);
  }
};

void check(
    Machine &machine, Instruction instruction, FaultCase fault, std::uint32_t mode, bool callback) {
  auto registers = machine.core->registers();
  registers.cpsr = initial_flags | mode | (instruction.thumb ? 0x20 : 0);
  registers.r[0] = fault.address;
  registers.r[1] = store_words[0];
  for (unsigned index = 0; index < store_words.size(); ++index) {
    registers.r[index + 2] = store_words[index];
  }
  registers.system.data_fault_status = stale_data;
  registers.system.instruction_fault_status = stale_instruction;
  registers.system.fault_address = stale_address;
  machine.core->set_registers(registers);
  auto expected = machine.core->state();
  const auto size = instruction.thumb ? 2U : 4U;
  expected.registers.r[15] = entry + size;
  expected.progress = {.instructions = 1, .base_cycles = 1}; // The prefix NOP completed.
  auto ram = machine.original;
  for (unsigned index = 0; index < fault.beat; ++index) {
    if (instruction.operation == Operation::load) {
      expected.registers.r[index + 2] = data_words[index];
    }
    if (instruction.operation == Operation::store) {
      fixture::put(
          std::span(ram).subspan(std::size_t(index) * 4, 4), store_words[index], machine.big);
    }
  }
  const auto prefix = expected;
  if (!callback) {
    expected.registers.cpsr = abort_flags;
    expected.registers.spsr = registers.cpsr;
    expected.registers.r[14] = entry + size + 8;
    expected.registers.r[15] = vector;
    expected.registers.system.data_fault_status = 8;
    expected.registers.system.fault_address = fault.far;
    expected.banks.abort[1] = entry + size + 8;
    expected.banks.spsr[3] = registers.cpsr;
    expected.progress.base_cycles = 4;
  }
  const auto result = machine.core->run_until(2);
  if (callback) {
    expected.execution = armv5::ExecutionState::fault;
    expected.fault = result.fault;
    fixture::require(result.fault.kind == armv5::Fault::Kind::memory &&
                         result.fault.address ==
                             (fault.address & ~(instruction.width - 1)) + fault.beat * 4 &&
                         machine.refusal.calls == 1 && machine.refusal.width == instruction.width &&
                         machine.refusal.prefix == prefix,
                     "callback rejection stays a host fault, with the exact pre-failure prefix");
  }
  fixture::require(machine.core->state() == expected && machine.data == ram &&
                       result.progress == expected.progress,
                   "external abort records independent burst FAR/DFSR and exact partial "
                   "register/memory prefix; callback faults preserve CP15 fault registers");
  const auto saved = machine.core->save();
  if (!callback) {
    const auto read = machine.core->run(3);
    const auto observed = machine.core->registers();
    fixture::require(read.execution == armv5::ExecutionState::ready && observed.r[9] == 8 &&
                         observed.r[10] == fault.far && observed.r[11] == stale_instruction,
                     "guest handler reads the architectural DFSR/FAR/unchanged IFSR through MRC");
    (void)machine.core->run(1);
    fixture::require(machine.core->registers().cpsr == registers.cpsr &&
                         machine.core->registers().r[15] == entry + size * 2 &&
                         machine.core->registers().r[0] == fault.address,
                     "external abort return restores source state and the original base");
  }
  machine.core->restore(saved);
  fixture::require(
      machine.core->state() == expected && machine.data == ram,
      "snapshot restores exact abort/host-fault state without touching consumer bytes");
}

void verify(Machine &machine, Instruction instruction, FaultCase fault, Failure failure) {
  const auto clean = machine.core->save();
  for (const auto mode : modes) {
    machine.core->restore(clean);
    std::ranges::copy(machine.original, machine.data.begin());
    machine.refusal.calls = 0;
    check(machine, instruction, fault, mode, failure == Failure::callback);
  }
  machine.core->restore(clean);
}

void fixture(Instruction instruction, FaultCase fault, Failure failure, bool fixed, bool big) {
  Machine machine(instruction, fault, failure, fixed, big);
  verify(machine, instruction, fault, failure);
}

void singles(bool fixed, bool big) {
  constexpr std::array instructions{
      Instruction{.word = 0xe4902004, .words = 1, .operation = Operation::load},  // LDR post.
      Instruction{.word = 0xe4802004, .words = 1, .operation = Operation::store}, // STR post.
      Instruction{.word = 0xe1002091, .words = 1, .operation = Operation::swap},
      Instruction{.word = 0x6802, .words = 1, .operation = Operation::load, .thumb = true},
      Instruction{.word = 0x6002, .words = 1, .operation = Operation::store, .thumb = true},
      Instruction{.word = 0xe4d02001, .words = 1, .operation = Operation::load, .width = 1},
      Instruction{.word = 0xe4c02001, .words = 1, .operation = Operation::store, .width = 1},
      Instruction{.word = 0xe1402091, .words = 1, .operation = Operation::swap, .width = 1},
      Instruction{.word = 0xe1d020b0, .words = 1, .operation = Operation::load, .width = 2},
      Instruction{.word = 0xe1c020b0, .words = 1, .operation = Operation::store, .width = 2},
      Instruction{.word = 0xe1d020f0, .words = 1, .operation = Operation::load, .width = 2},
      Instruction{.word = 0xe1d020d0, .words = 1, .operation = Operation::load, .width = 1}};
  for (const auto instruction : instructions) {
    const auto denied =
        instruction.operation == Operation::store ? Failure::write_denied : Failure::read_denied;
    // Reuse one prepared image across four byte offsets and all seven processor modes.
    for (const auto failure : {Failure::absent, denied, Failure::callback}) {
      Machine machine(instruction, {.address = 0x4000, .far = 0x4000}, failure, fixed, big);
      for (const auto address : {0x4000U, 0x4001U, 0x4002U, 0x4003U}) {
        if (instruction.width == 2 && address % 2 != 0) { continue; }
        verify(machine, instruction, {.address = address, .far = address}, failure);
      }
    }
    if (instruction.operation == Operation::swap) {
      fixture(instruction, {.address = 0x4000, .far = 0x4000}, Failure::write_denied, fixed, big);
    }
  }
}

void multiples(bool fixed, bool big) {
  constexpr std::array instructions{
      Instruction{
          .word = 0xe8b0003c, .words = 4, .operation = Operation::load}, // LDMIA R0!,{R2..R5}.
      Instruction{.word = 0xe8a0003c, .words = 4, .operation = Operation::store}};
  constexpr std::array faults{FaultCase{.address = 0x4000, .far = 0x400c, .beat = 0},
                              FaultCase{.address = 0x4000, .far = 0x400c, .beat = 2},
                              FaultCase{.address = 0x43f8, .far = 0x43fc, .beat = 0},
                              FaultCase{.address = 0x43f8, .far = 0x43fc, .beat = 1},
                              FaultCase{.address = 0x43f8, .far = 0x4404, .beat = 2},
                              FaultCase{.address = 0x43f8, .far = 0x4404, .beat = 3},
                              FaultCase{.address = 0xfffffff0, .far = 0xfffffffc, .beat = 1}};
  for (const auto instruction : instructions) {
    for (const auto fault : faults) {
      const auto denied =
          instruction.operation == Operation::load ? Failure::read_denied : Failure::write_denied;
      for (const auto failure : {Failure::absent, denied, Failure::callback}) {
        fixture(instruction, fault, failure, fixed, big);
      }
    }
  }
  constexpr std::array pairs{
      Instruction{.word = 0xe1c020d0, .words = 2, .operation = Operation::load},
      Instruction{.word = 0xe1c020f0, .words = 2, .operation = Operation::store},
      Instruction{.word = 0xc80c, .words = 2, .operation = Operation::load, .thumb = true},
      Instruction{.word = 0xc00c, .words = 2, .operation = Operation::store, .thumb = true}};
  constexpr std::uint32_t pair_start = 0x43f8;
  constexpr std::uint32_t pair_far = 0x43fc;
  for (const auto instruction : pairs) {
    for (const unsigned beat : {0U, 1U}) {
      fixture(instruction,
              {.address = pair_start, .far = pair_far, .beat = beat},
              Failure::absent,
              fixed,
              big);
    }
  }
}
} // namespace

void external_aborts() {
  for (const bool fixed : {false, true}) {
    for (const bool big : {false, true}) {
      singles(fixed, big);
      multiples(fixed, big);
    }
  }
}
