// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace armv5::detail {
inline constexpr std::uint32_t user_mode = 0x10, fiq_mode = 0x11, irq_mode = 0x12,
                               supervisor_mode = 0x13, abort_mode = 0x17, undefined_mode = 0x1b,
                               system_mode = 0x1f;

struct Span {
  std::uint32_t address;
  std::vector<std::byte> bytes;
  std::vector<std::uint32_t> modes;
  bool thumb;
};

struct Variant {
  std::uint32_t id;
  std::vector<Span> spans;
  std::vector<std::uint32_t> entries;
  std::vector<RegionLayout> layout;
  Registers::System::Mapping mapping;
};

struct Unit {
  std::size_t variant;
  std::uint32_t mode;
  bool thumb, direct = false;
};

struct Image {
  std::string identity, key;
  CompileOptions options;
  std::vector<Variant> variants;
  std::vector<Unit> units;
  std::vector<CodeTransition> transitions;
  std::vector<std::uint32_t> vectors;
  std::vector<std::byte> bitcode, artifact;
  std::vector<std::string> exports;
};

[[nodiscard]] bool valid_mode(std::uint32_t mode);
[[noreturn]] void refuse(std::string_view identity, std::uint32_t address, std::string_view reason);
[[nodiscard]] Image describe(const CodeImage &input, const CompileOptions &options);
void lower_llvm(Image &image);
void pack(Image &image, const CodeImage &input);
[[nodiscard]] Image
unpack(const CodeImage &input, const CompileOptions &options, std::span<const std::byte> artifact);
} // namespace armv5::detail
