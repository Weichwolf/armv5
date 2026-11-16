// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

void processor_modes();
void interrupts();

namespace fixture {
inline constexpr std::uint32_t user_mode = 0x10, fiq_mode = 0x11, irq_mode = 0x12,
                               supervisor_mode = 0x13, abort_mode = 0x17, undefined_mode = 0x1b,
                               system_mode = 0x1f;

struct ModeProgram {
  Program program;
  std::vector<std::uint32_t> modes;
};

class Modes {
public:
  std::vector<ModeProgram> programs;
  std::vector<std::byte> ram = std::vector<std::byte>(0x10000);
  std::vector<std::vector<std::byte>> storage;
  std::vector<armv5::Memory::Region> regions;
  std::vector<armv5::CodeSpan> spans;
  std::vector<armv5::RegionLayout> layout;
  std::vector<std::uint32_t> entries, vectors;
  std::unique_ptr<armv5::Core> core;
  std::optional<armv5::NativeImage> native;

  explicit Modes(std::vector<ModeProgram> code,
                 std::vector<std::uint32_t> declared_vectors = {},
                 bool fixed = true)
      : programs(std::move(code)), vectors(std::move(declared_vectors)) {
    storage.reserve(programs.size());
    for (auto &item : programs) {
      storage.push_back(item.program.bytes());
      spans.push_back({.address = item.program.address(),
                       .bytes = item.program.bytes(),
                       .processor_modes = item.modes});
      // Code storage is consumer-owned and writable only through an explicit rebind.
      regions.push_back({.address = item.program.address(),
                         .bytes = storage.back(),
                         .readable = true,
                         .writable = false,
                         .executable = true});
      entries.push_back(item.program.address());
    }
    regions.push_back(
        {.address = 0x4000, .bytes = ram, .readable = true, .writable = true, .executable = false});
    for (const auto &region : regions) {
      layout.push_back({.address = region.address,
                        .size = region.bytes.size(),
                        .readable = region.readable,
                        .writable = region.writable,
                        .executable = region.executable});
    }
    const std::array variants{armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries}};
    const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = layout}};
    const armv5::CodeImage image{.identity = "authored-mode-fixture",
                                 .variants = variants,
                                 .layouts = fixed ? std::span<const armv5::VariantLayout>(layouts)
                                                  : std::span<const armv5::VariantLayout>{},
                                 .transitions = {},
                                 .interrupt_vectors = vectors};
    native.emplace(armv5::Core::compile(image));
    core = std::make_unique<armv5::Core>(armv5::Memory{.regions = regions, .generation = 0});
    (void)core->install(*native, 0);
    core->reset(programs.front().program.address());
  }

  [[nodiscard]] const armv5::NativeImage &prepared() const {
    if (!native) { throw std::logic_error("fixture image has not been compiled"); }
    return *native;
  }

  [[nodiscard]] armv5::RunResult run(std::uint64_t instructions) const {
    auto result = core->run(instructions);
    require(result.execution == armv5::ExecutionState::ready, result.fault.reason);
    return result;
  }
};
} // namespace fixture
