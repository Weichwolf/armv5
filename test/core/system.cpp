// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
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
constexpr std::uint32_t entry = 0x1000, bus_value = 0x11223344, itcm_value = 0x55667788,
                        dtcm_value = 0x99aabbcc, both_tcms = 0x10001;
using Space = armv5::Memory::Space;

struct Image {
  fixture::Program program;
  std::vector<std::byte> code;
  std::array<armv5::CodeSpan, 1> spans;
  std::array<std::uint32_t, 1> entries{entry};
  std::array<armv5::CodeVariant, 2> variants;

  explicit Image(fixture::Program input) : program(std::move(input)), code(program.bytes()) {
    spans[0] = {.address = entry, .bytes = code, .processor_modes = {}};
    variants[0] = {.id = 0, .spans = spans, .entries = entries};
    variants[1] = {.id = 1,
                   .spans = spans,
                   .entries = entries,
                   .mapping = armv5::Registers::System::Mapping{.domains = 7}};
  }

  armv5::CodeImage declaration() {
    return {.identity = "authored-system-lifecycle",
            .variants = variants,
            .layouts = {},
            .transitions = {},
            .interrupt_vectors = {}};
  }
};

void mapping_bindings() {
  fixture::Program code;
  constexpr std::uint32_t read_domains = 0xee130f10;
  code.word(read_domains);
  Image image(std::move(code));
  const auto semantic = armv5::Core::lower(image.declaration());
  const auto native = armv5::Core::compile(image.declaration());
  const std::array regions{armv5::Memory::Region{.address = entry,
                                                 .bytes = image.code,
                                                 .readable = true,
                                                 .writable = false,
                                                 .executable = true}};
  armv5::Core core({.regions = regions, .generation = 0});
  core.reset(entry);
  (void)core.install(native, 0);
  const auto before = core.state();
  bool refused = false;
  try {
    (void)core.install(native, 1);
  } catch (const std::runtime_error &error) {
    fixture::contains(error.what(), "CP15 mapping differs");
    refused = true;
  }
  fixture::require(refused && core.state() == before,
                   "install checks CP15 assumptions without changing guest state/time");
  (void)core.install(native, 0);
  auto registers = core.registers();
  registers.system.mapping.domains = 7;
  core.set_registers(registers);
  const auto saved = core.save();
  const auto unbound = core.run(1);
  fixture::require(unbound.execution == armv5::ExecutionState::fault &&
                       unbound.fault.kind == armv5::Fault::Kind::unprepared &&
                       unbound.progress.instructions == 0,
                   "register API mapping change revokes the old binding");
  core.restore(saved);
  const auto matching = core.state();
  (void)core.install(native, 1);
  fixture::require(core.state() == matching, "matching context install changes no guest state");
  const auto result = core.run(1);
  fixture::require(result.execution == armv5::ExecutionState::ready && core.registers().r[0] == 7,
                   "prepared context reads actual architectural CP15 state");
  core.restore(saved);
  fixture::require(core.state() == matching, "snapshot restores exact CP15 mapping");
  image.variants[1].mapping = armv5::Registers::System::Mapping{.domains = 6};
  refused = false;
  try {
    (void)armv5::Core::load(image.declaration(), {}, semantic.artifact());
  } catch (const std::runtime_error &error) {
    fixture::contains(error.what(), "digest mismatch");
    refused = true;
  }
  fixture::require(refused, "cached artifact identity includes CP15 mapping assumptions");
}

void storage_spaces() {
  fixture::Program code;
  constexpr std::array instructions{0xee100f50U, 0xee191f11U, 0xee192f31U, 0xe5943000U};
  for (const auto instruction : instructions) { code.word(instruction); }
  Image image(std::move(code));
  std::array<std::byte, 4> bus{};
  std::array<std::byte, 4096> itcm{};
  std::array<std::byte, 8192> dtcm{};
  fixture::put(bus, bus_value);
  fixture::put(std::span(itcm).first(4), itcm_value);
  fixture::put(std::span(dtcm).first(4), dtcm_value);
  const auto original_itcm = itcm;
  const auto original_dtcm = dtcm;
  const std::array regions{
      armv5::Memory::Region{.address = entry,
                            .bytes = image.code,
                            .readable = true,
                            .writable = false,
                            .executable = true},
      armv5::Memory::Region{
          .address = 0, .bytes = bus, .readable = true, .writable = true, .executable = false},
      armv5::Memory::Region{.address = 0,
                            .bytes = itcm,
                            .readable = true,
                            .writable = true,
                            .executable = true,
                            .space = Space::itcm},
      armv5::Memory::Region{.address = 0,
                            .bytes = dtcm,
                            .readable = true,
                            .writable = true,
                            .executable = false,
                            .space = Space::dtcm}};
  armv5::Core core({.regions = regions, .generation = 0});
  core.reset(entry);
  const auto initial = core.registers();
  fixture::require(initial.system.mapping.instruction_tcm == 12 &&
                       initial.system.mapping.data_tcm == 16,
                   "TCM geometry is known before preparation/install and reset keeps it disabled");
  std::vector<armv5::RegionLayout> layout;
  layout.reserve(regions.size());
  for (const auto &region : regions) {
    layout.push_back({.address = region.address,
                      .size = region.bytes.size(),
                      .readable = region.readable,
                      .writable = region.writable,
                      .executable = region.executable,
                      .space = region.space});
  }
  const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = layout}};
  auto declaration = image.declaration();
  declaration.layouts = layouts;
  const auto semantic = armv5::Core::lower(declaration);
  const auto native = armv5::Core::compile(declaration);
  (void)core.install(native, 0);
  const auto result = core.run(4);
  auto expected = initial;
  expected.r[0] = both_tcms;
  expected.r[1] = 16;
  expected.r[2] = 12;
  expected.r[3] = bus_value;
  expected.r[15] = image.program.pc();
  fixture::require(
      core.registers() == expected && result.execution == armv5::ExecutionState::ready &&
          result.progress == armv5::Progress{.instructions = 4, .base_cycles = 4} &&
          itcm == original_itcm && dtcm == original_dtcm,
      "separate TCM presence/size registers and disabled data routing retain consumer bytes");
  const auto saved = core.save();
  const auto state = core.state();
  core.set_memory({.regions = regions, .generation = 1});
  (void)core.install(native, 0);
  core.restore(saved);
  fixture::require(core.state() == state && itcm == original_itcm && dtcm == original_dtcm,
                   "TCM storage rebinding/install/snapshot do not replace consumer storage");
  auto changed_regions = regions;
  changed_regions[2].bytes = dtcm;
  core.set_memory({.regions = changed_regions, .generation = 2});
  const auto changed_profile = core.state();
  bool mismatched = false;
  try {
    core.restore(saved);
  } catch (const std::invalid_argument &) { mismatched = true; }
  fixture::require(mismatched && core.state() == changed_profile,
                   "snapshot profile mismatch refuses before mutating processor state");
  core.set_memory({.regions = regions, .generation = 3});
  core.restore(saved);
  fixture::require(core.state() == state,
                   "snapshot restores after original TCM geometry is rebound");
  layout[2].space = Space::dtcm;
  bool stale = false;
  try {
    (void)armv5::Core::load(declaration, {}, semantic.artifact());
  } catch (const std::runtime_error &error) {
    fixture::contains(error.what(), "digest mismatch");
    stale = true;
  }
  fixture::require(stale, "artifact identity includes the address space of each layout region");
  for (const auto invalid :
       {armv5::Memory::Region{.address = 4, .bytes = itcm, .space = Space::itcm},
        armv5::Memory::Region{
            .address = 0, .bytes = std::span(itcm).first(2048), .space = Space::itcm},
        armv5::Memory::Region{
            .address = 0, .bytes = bus, .space = std::bit_cast<Space>(std::uint8_t{3})}}) {
    bool refused = false;
    try {
      const armv5::Core other({.regions = std::span(&invalid, 1), .generation = 0});
    } catch (const std::invalid_argument &) { refused = true; }
    fixture::require(refused, "invalid TCM geometry/space is rejected before execution");
  }
}
} // namespace

void system_lifecycle() {
  mapping_bindings();
  storage_spaces();
}
