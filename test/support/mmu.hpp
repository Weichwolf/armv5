// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "isa.hpp"
#include "modes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fixture {
class MmuMachine {
public:
  static constexpr std::uint32_t entry = 0x1000, table_address = 0x4000, page_address = 0x8000,
                                 code_physical = 0x100000, data_physical = 0x200000,
                                 alternate_physical = 0x300000, data_virtual = 0x02004000;
  bool big;
  armv5::Registers::System::Mapping context;
  std::vector<std::byte> table{std::size_t{16384}}, pages{std::size_t{4096}},
      data{std::size_t{65536}}, alternate{std::size_t{65536}};
  std::vector<std::vector<std::byte>> code;
  std::vector<std::vector<std::uint32_t>> modes;
  std::vector<armv5::Memory::Region> regions;
  std::vector<armv5::CodeSpan> spans;
  std::vector<armv5::RegionLayout> layout;
  std::vector<std::uint32_t> entries, vectors;
  std::unique_ptr<armv5::Core> core;
  std::optional<armv5::NativeImage> native;

  explicit MmuMachine(const std::vector<ModeProgram> &programs,
                      std::vector<std::uint32_t> declared_vectors = {},
                      bool big_endian = false,
                      armv5::Io io = {})
      : big(big_endian), vectors(std::move(declared_vectors)) {
    constexpr std::uint32_t little_control = 0x50079;
    constexpr std::uint32_t big_control = 0x500f9;
    constexpr std::uint32_t code_section = 0x00100c12;
    constexpr std::uint32_t data_section = 0x00200c32;
    context = {.control = big ? big_control : little_control,
               .translation_table = table_address,
               .domains = 7};
    code.reserve(programs.size());
    modes.reserve(programs.size());
    for (const auto &program : programs) {
      code.push_back(program.program.bytes());
      modes.push_back(program.modes);
      spans.push_back({.address = program.program.address(),
                       .bytes = code.back(),
                       .processor_modes = modes.back(),
                       .instruction_set = program.instruction_set});
      entries.push_back(program.program.address());
      regions.push_back({.address = code_physical + program.program.address(),
                         .bytes = code.back(),
                         .readable = true,
                         .writable = false,
                         .executable = true});
    }
    regions.push_back({.address = table_address, .bytes = table, .writable = true});
    regions.push_back({.address = page_address, .bytes = pages, .writable = true});
    regions.push_back({.address = data_physical, .bytes = data, .writable = true});
    regions.push_back({.address = alternate_physical, .bytes = alternate, .writable = true});
    for (const auto &region : regions) {
      layout.push_back({.address = region.address,
                        .size = region.bytes.size(),
                        .readable = region.readable,
                        .writable = region.writable,
                        .executable = region.executable});
    }
    core = std::make_unique<armv5::Core>(armv5::Memory{.regions = regions, .generation = 0}, io);
    first(0, code_section);  // Code: 1MB section, domain 0, AP=3.
    first(32, data_section); // Data: domain 1, AP=3.
  }

  void first(unsigned index, std::uint32_t descriptor) {
    put(std::span(table).subspan(std::size_t(index) * 4, 4), descriptor, big);
  }

  void page(std::uint32_t descriptor) {
    for (std::size_t offset = 0; offset < pages.size(); offset += 4) {
      put(std::span(pages).subspan(offset, 4), descriptor, big);
    }
  }

  [[nodiscard]] armv5::Registers::System::Mapping mapping() const { return context; }

  [[nodiscard]] const armv5::NativeImage &prepared() const {
    if (!native) { throw std::logic_error("MMU fixture image has not been compiled"); }
    return *native;
  }

  void prepare(bool fixed = true, std::span<const armv5::CodeTransition> transitions = {}) {
    const std::array variants{
        armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries, .mapping = mapping()}};
    const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = layout}};
    const armv5::CodeImage image{.identity = "authored-mmu-program",
                                 .variants = variants,
                                 .layouts = fixed ? std::span<const armv5::VariantLayout>(layouts)
                                                  : std::span<const armv5::VariantLayout>{},
                                 .transitions = transitions,
                                 .interrupt_vectors = vectors};
    native.emplace(armv5::Core::compile(image));
    auto initial = core->registers();
    initial.system.mapping = mapping();
    initial.r[15] = entry;
    core->set_registers(initial);
    const auto before = core->state();
    (void)core->install(*native, 0);
    require(core->state() == before, "MMU install changed processor state/time");
  }
};
} // namespace fixture
