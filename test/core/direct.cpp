// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "consumer.hpp"
#include "support/faults.hpp"
#include "support/isa.hpp"
#include "support/mapped.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>

namespace {
constexpr std::uint32_t entry = 0x1000;
constexpr std::uint32_t input = 0x12345678;
constexpr std::uint32_t output = 0x87654321;
constexpr auto direct = armv5::CompileOptions::MemoryMode::direct;

class Alias {
public:
  int descriptor;
  std::span<std::byte> bytes;

  explicit Alias(int backing) : descriptor(dup(backing)) {
    if (descriptor < 0) {
      throw std::system_error(errno, std::generic_category(), "duplicate backing");
    }
    auto *mapping = mmap(nullptr,
                         fixture::AddressSpace::data_address,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         descriptor,
                         0);
    if (mapping == MAP_FAILED) {
      const auto error = errno;
      (void)close(descriptor);
      throw std::system_error(error, std::generic_category(), "map shared code alias");
    }
    bytes = {static_cast<std::byte *>(mapping), fixture::AddressSpace::data_address};
  }

  ~Alias() {
    (void)munmap(bytes.data(), bytes.size());
    (void)close(descriptor);
  }

  Alias(const Alias &) = delete;
  Alias &operator=(const Alias &) = delete;
};

fixture::Program program(std::uint32_t address = entry) {
  fixture::Program code(address);
  code.word(fixture::transfer(true, false, 0, 1, 0));
  code.word(fixture::transfer(false, false, 2, 1, 4));
  return code;
}

void ordinary() {
  fixture::MappedMachine machine(program(), 4096, direct);
  fixture::put(std::span(machine.ram).first(4), input);
  auto expected = machine.core->state();
  expected.registers.r[1] = fixture::AddressSpace::data_address;
  expected.registers.r[2] = output;
  machine.core->set_registers(expected.registers);
  machine.prepare();
  expected.registers.r[0] = input;
  expected.registers.r[15] = entry + 8;
  expected.progress = {.instructions = 2, .base_cycles = 2};
  const auto result = machine.run(2);
  fixture::require(result.progress == expected.progress && machine.core->state() == expected,
                   "Direct loads/stores preserve complete state and progress");
  fixture::require(fixture::get(std::span(machine.ram).subspan(4, 4)) == output,
                   "Direct store updates consumer backing");
  machine.verify_storage();
}

void tcm_overlay(bool writable) {
  constexpr std::uint32_t target = 0x8000;
  constexpr std::uint32_t backing_offset = 0x10000;
  constexpr std::uint32_t enabled_4k = 13;
  constexpr std::uint32_t page_size = 4096;
  fixture::MappedMachine machine(program(), backing_offset + page_size, direct);
  auto bytes = std::span(machine.ram).subspan(backing_offset, page_size);
  fixture::put(bytes.first(4), input);
  machine.regions_.push_back(
      {.address = 0,
       .bytes = bytes,
       .readable = true,
       .writable = writable,
       .space = armv5::Memory::Space::dtcm,
       .mapping_fd = machine.space_.descriptor(),
       .mapping_offset = fixture::AddressSpace::data_address + backing_offset});
  machine.layout_.push_back({.address = 0,
                             .size = page_size,
                             .readable = true,
                             .writable = writable,
                             .space = armv5::Memory::Space::dtcm,
                             .shared_mapping = true});
  machine.core->set_memory({.regions = machine.regions_, .generation = 1});
  auto expected = machine.core->state();
  expected.registers.system.mapping.data_tcm = target | enabled_4k;
  expected.registers.r[1] = target;
  expected.registers.r[2] = output;
  machine.mapping_ = expected.registers.system.mapping;
  machine.core->set_registers(expected.registers);
  machine.prepare();
  auto expected_ram = machine.ram;
  if (writable) {
    fixture::put(std::span(expected_ram).subspan(backing_offset + 4, 4), output);
    expected.registers.r[0] = input;
    expected.registers.r[15] = entry + 8;
    expected.progress = {.instructions = 2, .base_cycles = 2};
    (void)machine.run(2);
    fixture::require(machine.core->state() == expected && machine.ram == expected_ram,
                     "Direct DTCM overlay reads and writes local storage instead of bus RAM");
  } else {
#if defined(__linux__) && defined(__x86_64__)
    const fixture::FaultHandler handler;
    const auto result = machine.core->run(2);
    handler.verify_mask();
    fixture::require(result.execution == armv5::ExecutionState::fault &&
                         result.fault.kind == armv5::Fault::Kind::memory &&
                         result.fault.address == target + 4 && machine.ram == expected_ram,
                     "read-only DTCM overlay faults rather than writing writable bus storage");
    fixture::contains(result.fault.reason, "write at guest PC 0x1004");
#endif
  }
  machine.verify_storage();
}

void io_fallback() {
  constexpr std::uint32_t address = 0x20000;
  fixture::MappedMachine machine(program(), 4096, direct);
  machine.regions_.back().bytes = machine.regions_.back().bytes.first(0x10000);
  machine.layout_.back().size = 0x10000;

  struct Context {
    armv5::Core *core = nullptr;
    unsigned calls = 0;
    bool prefix = false;

    static bool
    read(void *opaque, std::uint32_t requested, unsigned width, std::uint32_t &value) noexcept {
      auto &context = *static_cast<Context *>(opaque);
      const auto state = context.core->state();
      context.prefix = requested == address && width == 4 && state.registers.r[15] == entry &&
                       state.registers.r[1] == address && state.progress == armv5::Progress{};
      ++context.calls;
      value = input;
      return true;
    }
  } context;

  machine.core =
      std::make_unique<armv5::Core>(armv5::Memory{.regions = machine.regions_},
                                    armv5::Io{.owner = &context, .read = &Context::read});
  context.core = machine.core.get();
  auto registers = machine.core->registers();
  registers.r[1] = address;
  registers.r[15] = entry;
  machine.core->set_registers(registers);
  machine.prepare();
  auto expected = machine.core->state();
  expected.registers.r[0] = input;
  expected.registers.r[15] = entry + 4;
  expected.progress = {.instructions = 1, .base_cycles = 1};
  (void)machine.run(1);
  fixture::require(machine.core->state() == expected && context.calls == 1 && context.prefix,
                   "declared I/O remains exact when a host view cannot express its effects");
}

void translated(std::uint32_t physical_base) {
  constexpr std::uint32_t table = 0x4000;
  constexpr std::uint32_t alias = 0x300000;
  constexpr std::uint32_t data = 0x20000;
  constexpr unsigned section_shift = 20;
  constexpr std::uint32_t client_ap3_section = (3U << 10) | 2;
  constexpr std::size_t working_size = 0x300000;
  const auto code = program();
  fixture::MappedMachine machine(code, working_size, direct);
  machine.regions_.back().executable = true;
  machine.layout_.back().executable = true;
  machine.core->set_memory({.regions = machine.regions_, .generation = 1});
  fixture::put(std::span(machine.ram).first(4), physical_base | client_ap3_section);
  fixture::put(std::span(machine.ram).subspan(std::size_t(alias >> section_shift) * 4, 4),
               physical_base | client_ap3_section);
  if (physical_base != 0) {
    std::ranges::copy(code.bytes(), machine.ram.begin() + physical_base + entry - table);
  }
  const auto offset = physical_base + data - table;
  fixture::put(std::span(machine.ram).subspan(offset, 4), input);
  auto expected_ram = machine.ram;
  fixture::put(std::span(expected_ram).subspan(offset + 4, 4), output);
  auto registers = machine.core->registers();
  registers.system.mapping.control |= 1;
  registers.system.mapping.translation_table = table;
  registers.system.mapping.domains = 1;
  registers.r[2] = output;
  machine.mapping_ = registers.system.mapping;
  machine.core->set_registers(registers);
  machine.prepare();
  for (const auto address : {data, alias + data}) {
    registers.r[1] = address;
    machine.core->set_registers(registers);
    const auto before = machine.core->save();
    const auto result = machine.run(2);
    fixture::require(result.progress == armv5::Progress{.instructions = 2, .base_cycles = 2} &&
                         machine.core->registers().r[0] == input && machine.ram == expected_ram,
                     "Direct MMU identity/remap and alias update the shared physical backing");
    const auto completed = machine.core->state();
    machine.core->restore(before);
    (void)machine.run(2);
    fixture::require(machine.core->state() == completed && machine.ram == expected_ram,
                     "Direct translated snapshot replay preserves CPU and shared storage");
  }
  machine.verify_storage();
}

void remap_transition() {
  constexpr std::uint32_t table = 0x4000;
  constexpr std::uint32_t address = 0x100000;
  constexpr std::uint32_t old_physical = 0x200000;
  constexpr std::uint32_t new_physical = 0x300000;
  constexpr std::uint32_t section_ap3 = 0xc02;
  constexpr std::uint32_t invalidate_all_r3 = 0xee083f17;
  fixture::Program code(entry);
  code.word(fixture::transfer(true, false, 0, 1, 0));
  code.word(fixture::transfer(false, false, 2, 4, 0));
  code.word(invalidate_all_r3);
  code.word(fixture::transfer(true, false, 5, 1, 0));
  fixture::MappedMachine machine(code, new_physical, direct);
  auto write = [&](std::uint32_t physical, std::uint32_t value) {
    fixture::put(std::span(machine.ram).subspan(physical - table, 4), value);
  };
  write(table, section_ap3);
  write(table + 4, old_physical | section_ap3);
  write(old_physical, input);
  write(new_physical, output);
  auto expected = machine.core->state();
  auto &registers = expected.registers;
  registers.system.mapping.control |= 1;
  registers.system.mapping.translation_table = table;
  registers.system.mapping.domains = 1;
  registers.r[1] = address;
  registers.r[2] = new_physical | section_ap3;
  registers.r[4] = table + 4;
  machine.core->set_registers(registers);
  const std::array destination_entries{entry + 12};
  const std::array variants{armv5::CodeVariant{.id = 0,
                                               .spans = machine.spans_,
                                               .entries = machine.entries_,
                                               .mapping = registers.system.mapping},
                            armv5::CodeVariant{.id = 1,
                                               .spans = machine.spans_,
                                               .entries = destination_entries,
                                               .mapping = registers.system.mapping}};
  const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = machine.layout_},
                           armv5::VariantLayout{.variant = 1, .regions = machine.layout_}};
  const std::array transitions{
      armv5::CodeTransition{.source_variant = 0, .destination_variant = 1, .address = entry + 12}};
  const armv5::CodeImage image{.identity = "authored-live-page-table-remap",
                               .variants = variants,
                               .layouts = layouts,
                               .transitions = transitions,
                               .interrupt_vectors = {}};
  const auto native = armv5::Core::compile(image, machine.options_);
  (void)machine.core->install(native, 0);
  auto expected_ram = machine.ram;
  fixture::put(std::span(expected_ram).subspan(4, 4), registers.r[2]);
  registers.r[0] = input;
  registers.r[15] = entry + 12;
  expected.progress = {.instructions = 3, .base_cycles = 3};
  expected.execution = armv5::ExecutionState::transfer;
  expected.transition_pending = true;
  expected.transition = transitions.front();
  const auto result = machine.core->run(4);
  fixture::require(result.execution == expected.execution && machine.core->state() == expected &&
                       machine.ram == expected_ram,
                   "guest PTE store and TLB invalidation stop at the declared remap boundary");
  (void)machine.core->install(native, 1);
  registers.r[5] = output;
  registers.r[15] = code.pc();
  expected.progress = {.instructions = 4, .base_cycles = 4};
  expected.execution = armv5::ExecutionState::ready;
  expected.transition_pending = false;
  (void)machine.run(1);
  fixture::require(machine.core->state() == expected && machine.ram == expected_ram,
                   "the installed Direct view follows the new guest translation");
  machine.verify_storage();
}

void stale_translation() {
  constexpr std::uint32_t table = 0x4000;
  constexpr std::uint32_t coarse = 0x8000;
  constexpr std::uint32_t address = 0x20000;
  constexpr std::uint32_t retained = 0x30000;
  constexpr std::uint32_t page_ap3 = 0xff2;
  constexpr std::uint32_t section_ap3 = 0xc02;
  fixture::MappedMachine machine(program(), 0x40000, direct);
  machine.options_.memory_mode = armv5::CompileOptions::MemoryMode::exact;
  auto write = [&](std::uint32_t physical, std::uint32_t value) {
    fixture::put(std::span(machine.ram).subspan(physical - table, 4), value);
  };
  write(table, coarse | 1);
  write(coarse + 4, entry | page_ap3);
  write(coarse + ((address >> 12) * 4), retained | page_ap3);
  write(address, output);
  write(retained, input);
  auto registers = machine.core->registers();
  registers.r[1] = address;
  registers.system.mapping.control |= 1;
  registers.system.mapping.translation_table = table;
  registers.system.mapping.domains = 1;
  machine.mapping_ = registers.system.mapping;
  machine.core->set_registers(registers);
  machine.prepare();
  (void)machine.run(1);
  machine.core->set_registers(registers);
  const auto saved = machine.core->save();
  auto expected = machine.core->state();
  write(table, section_ap3);
  const auto expected_ram = machine.ram;
  machine.options_.memory_mode = direct;
  machine.core->set_registers(registers);
  machine.prepare();
  (void)machine.run(1);
  fixture::require(
      machine.core->registers().r[0] == input && machine.ram == expected_ram,
      "Direct preparation falls back to exact for a retained page inside a new section");
  machine.core->reset(entry);
  machine.core->set_registers(registers);
  machine.prepare();
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[0] == output,
                   "fresh Direct binding uses the replacement section");
  machine.core->restore(saved);
  (void)machine.run(1);
  expected.registers.r[0] = input;
  expected.registers.r[15] = entry + 4;
  ++expected.progress.instructions;
  ++expected.progress.base_cycles;
  fixture::require(machine.core->state() == expected && machine.ram == expected_ram,
                   "restoring old TLB state replaces a newer Direct view without changing RAM");
  machine.verify_storage();
}

void backing_alias(bool changing) {
  constexpr std::uint32_t alias_address = 0x20000;
  fixture::Program code(entry);
  code.word(fixture::transfer(false, false, 2, 1, 0));
  const auto original = fixture::data(13, false, 0, 0, true, 1);
  code.word(original);
  const auto replacement = changing ? fixture::data(13, false, 0, 0, true, 7) : original;
  std::unique_ptr<Alias> alias;
  fixture::MappedMachine machine(code, 4096, direct);
  alias = std::make_unique<Alias>(machine.space_.descriptor());
  machine.options_.memory_mode = armv5::CompileOptions::MemoryMode::exact;
  machine.regions_.back().bytes = machine.regions_.back().bytes.first(0x10000);
  machine.layout_.back().size = 0x10000;
  machine.regions_.push_back(
      {.address = alias_address, .bytes = alias->bytes, .mapping_fd = alias->descriptor});
  machine.layout_.push_back({.address = alias_address,
                             .size = alias->bytes.size(),
                             .writable = true,
                             .shared_mapping = true});
  machine.core->set_memory({.regions = machine.regions_, .generation = 1});
  auto expected = machine.core->state();
  expected.registers.r[1] = alias_address + entry + 4;
  expected.registers.r[2] = replacement;
  machine.core->set_registers(expected.registers);
  machine.prepare();
  expected.registers.r[15] = entry + (changing ? 4 : 8);
  expected.registers.r[0] = changing ? 0 : 1;
  expected.progress = {.instructions = changing ? 1U : 2U, .base_cycles = changing ? 1U : 2U};
  const auto result = machine.core->run(2);
  if (changing) {
    expected.execution = armv5::ExecutionState::fault;
    expected.fault = result.fault;
    fixture::require(result.fault.kind == armv5::Fault::Kind::changed_code &&
                         result.fault.address == alias_address + entry + 4,
                     "shared-file code alias revokes before the changed successor");
  }
  fixture::require(machine.core->state() == expected && result.progress == expected.progress &&
                       fixture::get(alias->bytes.subspan(entry + 4, 4)) == replacement,
                   "distinct mmap and dup descriptors preserve exact code-write prefixes");
}

#if defined(__linux__) && defined(__x86_64__)

void fault(bool writing, std::uint32_t pc = entry, bool dead_result = false) {
  const fixture::FaultHandler handler;
  auto code = program(pc);
  if (dead_result) {
    code = fixture::Program(pc);
    code.word(fixture::transfer(true, false, 0, 1, 0));
    code.word(fixture::data(13, false, 0, 0, true, 7));
  }
  fixture::MappedMachine machine(code, 4096, direct);
  // Whole host pages retain exact physical storage; the rest is a protected hole.
  constexpr std::uint32_t ram_size = 0x10000;
  constexpr std::uint32_t hole = 0x20000;
  machine.regions_.back().bytes = machine.regions_.back().bytes.first(ram_size);
  machine.layout_.back().size = ram_size;
  machine.core->set_memory({.regions = machine.regions_, .generation = 1});
  auto registers = machine.core->registers();
  registers.r[1] = writing ? pc : hole;
  registers.r[2] = output;
  machine.core->set_registers(registers);
  machine.prepare();
  const auto result = machine.core->run(2);
  handler.verify_mask();
  fixture::require(result.execution == armv5::ExecutionState::fault &&
                       result.fault.kind == armv5::Fault::Kind::memory &&
                       result.fault.address == (writing ? pc + 4 : hole) &&
                       result.fault.identity == "authored-full-space-benchmark",
                   "native host fault names consumer identity, operation and guest address");
  const auto *expected_site = "read at guest PC 0x1000";
  if (writing) { expected_site = "write at guest PC 0x1004"; }
  if (pc == 0) { expected_site = "read at guest PC 0x0;"; }
  fixture::contains(result.fault.reason, expected_site);
  fixture::contains(result.fault.reason, "state and progress unavailable");
  bool refused = false;
  try {
    machine.core->reset(entry);
  } catch (const std::logic_error &) { refused = true; }
  fixture::require(refused, "fatal Direct state cannot resume through reset");
}
#endif
} // namespace

void direct_memory() {
  fixture::require(!armv5::Core::host_fault(0, false, 0),
                   "host faults outside active native entries decline");
  ordinary();
  tcm_overlay(true);
  tcm_overlay(false);
  io_fallback();
  translated(0);
  translated(0x100000);
  remap_transition();
  stale_translation();
  backing_alias(false);
  backing_alias(true);
#if defined(__linux__) && defined(__x86_64__)
  fault(false);
  fault(true);
  fault(false, 0);
  fault(false, 0, true);
#endif
}
