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
void call_returns();
void counted_loops();
void system_lifecycle();
void tcm_lifecycle();
void memory_control();
void control_lifecycle();
void mmu_lifecycle();

namespace fixture {
inline constexpr std::uint32_t user_mode = 0x10, fiq_mode = 0x11, irq_mode = 0x12,
                               supervisor_mode = 0x13, abort_mode = 0x17, undefined_mode = 0x1b,
                               system_mode = 0x1f;

struct ModeProgram {
  Program program;
  std::vector<std::uint32_t> modes;
  armv5::CodeSpan::InstructionSet instruction_set = armv5::CodeSpan::InstructionSet::inherit;
};

class Modes {
public:
  std::vector<ModeProgram> programs;
  std::vector<std::byte> ram;
  std::vector<std::vector<std::byte>> storage;
  std::vector<armv5::Memory::Region> regions;
  std::vector<armv5::CodeSpan> spans;
  std::vector<armv5::RegionLayout> layout;
  std::vector<std::uint32_t> entries, vectors;
  std::unique_ptr<armv5::Core> core;
  std::optional<armv5::NativeImage> native;

  explicit Modes(std::vector<ModeProgram> code,
                 std::vector<std::uint32_t> declared_vectors = {},
                 bool fixed = true,
                 std::size_t ram_size = 0x10000,
                 armv5::Io callbacks = {},
                 std::optional<armv5::Registers::System::Mapping> mapping = std::nullopt)
      : programs(std::move(code)), ram(ram_size), vectors(std::move(declared_vectors)) {
    storage.reserve(programs.size());
    for (auto &item : programs) {
      storage.push_back(item.program.bytes());
      spans.push_back({.address = item.program.address(),
                       .bytes = item.program.bytes(),
                       .processor_modes = item.modes,
                       .instruction_set = item.instruction_set});
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
    const std::array variants{
        armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries, .mapping = mapping}};
    const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = layout}};
    const armv5::CodeImage image{.identity = "authored-mode-fixture",
                                 .variants = variants,
                                 .layouts = fixed ? std::span<const armv5::VariantLayout>(layouts)
                                                  : std::span<const armv5::VariantLayout>{},
                                 .transitions = {},
                                 .interrupt_vectors = vectors};
    native.emplace(armv5::Core::compile(image));
    core = std::make_unique<armv5::Core>(armv5::Memory{.regions = regions, .generation = 0},
                                         callbacks);
    auto initial = core->registers();
    if (mapping) { initial.system.mapping = *mapping; }
    initial.r[15] = programs.front().program.address();
    core->set_registers(initial);
    (void)core->install(*native, 0);
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
