// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "isa.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace fixture {
class TcmMachine {
public:
  static constexpr std::uint32_t instruction_base = 0x4000, data_base = 0x8000;
  enum class Layout : std::uint8_t { dynamic, bus, all };
  std::vector<std::byte> code, bus_instruction, bus_data, instruction, data;
  std::vector<armv5::Memory::Region> regions;
  std::unique_ptr<armv5::Core> core;

  explicit TcmMachine(const Program &program)
      : code(program.bytes()),
        bus_instruction(4096),
        bus_data(8192),
        instruction(4096),
        data(8192) {
    using Space = armv5::Memory::Space;
    regions = {{.address = program.address(),
                .bytes = code,
                .readable = true,
                .writable = false,
                .executable = true},
               {.address = instruction_base,
                .bytes = bus_instruction,
                .readable = true,
                .writable = true,
                .executable = true},
               {.address = data_base,
                .bytes = bus_data,
                .readable = true,
                .writable = true,
                .executable = true},
               {.address = 0,
                .bytes = instruction,
                .readable = true,
                .writable = true,
                .executable = true,
                .space = Space::itcm},
               {.address = 0,
                .bytes = data,
                .readable = true,
                .writable = true,
                .executable = false,
                .space = Space::dtcm}};
    core = std::make_unique<armv5::Core>(armv5::Memory{.regions = regions, .generation = 0});
    core->reset(program.address());
  }

  [[nodiscard]] armv5::NativeImage compile(const armv5::CodeImage &declaration, Layout mode) const {
    using Space = armv5::Memory::Space;
    std::vector<armv5::RegionLayout> layout;
    layout.reserve(regions.size());
    for (const auto &region : regions) {
      if (mode == Layout::bus && region.space != Space::bus) { continue; }
      layout.push_back({.address = region.address,
                        .size = region.bytes.size(),
                        .readable = region.readable,
                        .writable = region.writable,
                        .executable = region.executable,
                        .space = region.space});
    }
    std::vector<armv5::VariantLayout> variants;
    variants.reserve(declaration.variants.size());
    if (mode != Layout::dynamic) {
      for (const auto &variant : declaration.variants) {
        variants.push_back({.variant = variant.id, .regions = layout});
      }
    }
    auto image = declaration;
    image.layouts = variants;
    return armv5::Core::compile(image);
  }
};
} // namespace fixture
