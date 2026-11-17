// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"
#include "armv5/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

void dsp_instructions();
void thumb_instructions();
void system_registers();
void tcm_accesses();

namespace fixture {
inline constexpr unsigned condition_shift = 28, immediate_bit = 25, opcode_shift = 21,
                          flags_bit = 20, transfer_bit = 26, preindex_bit = 24, up_bit = 23,
                          byte_bit = 22;
inline constexpr unsigned byte_mask = 255;

inline void require(bool condition, std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline void contains(std::string_view actual, std::string_view expected) {
  if (actual.find(expected) != std::string_view::npos) { return; }
  std::string message(expected);
  message.append(" / got: ").append(actual);
  throw std::runtime_error(message);
}

// Independently authored fields, never a production decoder/semantic helper.
inline std::uint32_t data(unsigned opcode,
                          bool flags,
                          unsigned destination,
                          unsigned source,
                          bool immediate,
                          unsigned operand,
                          unsigned condition = 14) {
  return (condition << condition_shift) | (unsigned(immediate) << immediate_bit) |
         (opcode << opcode_shift) | (unsigned(flags) << flags_bit) | (source << 16) |
         (destination << 12) | operand;
}

inline std::uint32_t transfer(bool load,
                              bool byte,
                              unsigned destination,
                              unsigned base,
                              unsigned offset = 4,
                              bool pre = true,
                              bool up = true,
                              bool writeback = false,
                              bool register_offset = false) {
  return (14U << condition_shift) | (1U << transfer_bit) |
         (unsigned(register_offset) << immediate_bit) | (unsigned(pre) << preindex_bit) |
         (unsigned(up) << up_bit) | (unsigned(byte) << byte_bit) |
         (unsigned(writeback) << opcode_shift) | (unsigned(load) << flags_bit) | (base << 16) |
         (destination << 12) | offset;
}

struct BranchTarget {
  std::uint32_t address;
};

inline std::uint32_t
branch(std::uint32_t pc, BranchTarget target, unsigned condition = 14, bool link = false) {
  constexpr std::uint32_t prefix = 0x0a000000;
  constexpr std::uint32_t displacement_mask = 0x00ffffff;
  const auto distance = std::int64_t(target.address) - pc - 8;
  require((distance & 3) == 0, "fixture branch alignment");
  return (condition << condition_shift) | prefix | (unsigned(link) << preindex_bit) |
         (static_cast<std::uint32_t>(distance / 4) & displacement_mask);
}

inline void put(std::span<std::byte> bytes, std::uint32_t value, bool big_endian = false) {
  require(bytes.size() == 4, "fixture word size");
  for (unsigned index = 0; index < 4; ++index) {
    bytes[index] = std::byte((value >> ((big_endian ? 3 - index : index) * 8)) & byte_mask);
  }
}

inline std::uint32_t get(std::span<const std::byte> bytes, bool big_endian = false) {
  require(bytes.size() == 4, "fixture word size");
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= std::to_integer<std::uint32_t>(bytes[index]) << ((big_endian ? 3 - index : index) * 8);
  }
  return value;
}

class Program {
public:
  explicit Program(std::uint32_t address = 0x1000, bool big_endian = false)
      : address_(address), big_endian_(big_endian) {}

  void half(std::uint16_t instruction) {
    const auto low = std::byte(instruction & byte_mask);
    const auto high = std::byte(instruction >> 8);
    bytes_.push_back(big_endian_ ? high : low);
    bytes_.push_back(big_endian_ ? low : high);
  }

  void word(std::uint32_t instruction) {
    const auto offset = bytes_.size();
    bytes_.resize(offset + 4);
    put(std::span(bytes_).subspan(offset, 4), instruction, big_endian_);
  }

  [[nodiscard]] std::uint32_t pc() const {
    return address_ + static_cast<std::uint32_t>(bytes_.size());
  }

  [[nodiscard]] std::uint32_t address() const { return address_; }

  [[nodiscard]] const std::vector<std::byte> &bytes() const { return bytes_; }

private:
  std::uint32_t address_;
  bool big_endian_;
  std::vector<std::byte> bytes_;
};

class Machine {
public:
  std::vector<std::byte> code, ram;
  std::vector<std::uint32_t> entries;
  std::array<armv5::Memory::Region, 2> regions;
  std::array<armv5::CodeSpan, 1> spans;
  std::array<armv5::CodeVariant, 1> variants;
  std::array<armv5::RegionLayout, 2> layout_regions;
  std::array<armv5::VariantLayout, 1> layouts;
  std::unique_ptr<armv5::Core> core;
  std::optional<armv5::NativeImage> native;

  explicit Machine(const Program &program, std::size_t ram_size = 0x10000, armv5::Io io = {})
      : code(program.bytes()),
        ram(ram_size),
        entries{program.address()},
        regions{},
        spans{},
        variants{},
        layout_regions{},
        layouts{} {
    regions = {armv5::Memory::Region{.address = program.address(),
                                     .bytes = code,
                                     .readable = true,
                                     .writable = false,
                                     .executable = true},
               armv5::Memory::Region{.address = 0x4000,
                                     .bytes = ram,
                                     .readable = true,
                                     .writable = true,
                                     .executable = false}};
    spans.front() = {.address = program.address(), .bytes = code, .processor_modes = {}};
    for (unsigned index = 0; index < regions.size(); ++index) {
      layout_regions[index] = {.address = regions[index].address,
                               .size = regions[index].bytes.size(),
                               .readable = regions[index].readable,
                               .writable = regions[index].writable,
                               .executable = regions[index].executable};
    }
    layouts.front() = {.variant = 0, .regions = layout_regions};
    core = std::make_unique<armv5::Core>(armv5::Memory{.regions = regions, .generation = 0}, io);
    auto initial = core->registers();
    initial.r[15] = program.address();
    core->set_registers(initial);
  }

  [[nodiscard]] armv5::CodeImage image(bool fixed_layout = true) {
    variants.front() = {.id = 0, .spans = spans, .entries = entries};
    return {.identity = "authored-arm-fixture",
            .variants = variants,
            .layouts = fixed_layout ? std::span<const armv5::VariantLayout>(layouts)
                                    : std::span<const armv5::VariantLayout>{},
            .transitions = {},
            .interrupt_vectors = {}};
  }

  [[nodiscard]] const armv5::NativeImage &prepared() const {
    if (!native) { throw std::logic_error("fixture image has not been compiled"); }
    return *native;
  }

  void prepare(const armv5::CompileOptions &options = {}, bool fixed_layout = true) {
    auto initial = core->registers();
    const auto control =
        (initial.system.mapping.control & ~0x80U) | (options.big_endian ? 0x80 : 0);
    if (initial.system.mapping.control != control) {
      initial.system.mapping.control = control;
      core->set_registers(initial);
    }
    native.emplace(armv5::Core::compile(image(fixed_layout), options));
    (void)core->install(*native, 0);
  }

  void registers(armv5::Registers registers) const { core->set_registers(registers); }

  [[nodiscard]] armv5::RunResult run(std::uint64_t budget) const {
    auto result = core->run(budget);
    require(result.execution == armv5::ExecutionState::ready, result.fault.reason);
    return result;
  }
};
} // namespace fixture
