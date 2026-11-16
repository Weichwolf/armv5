// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace armv5::detail {
struct Span {
  std::uint32_t address;
  std::vector<std::byte> bytes;
};

struct Variant {
  std::uint32_t id;
  std::vector<Span> spans;
  std::vector<std::uint32_t> entries;
  std::vector<RegionLayout> layout;
};

struct Image {
  std::string identity, key;
  CompileOptions options;
  std::vector<Variant> variants;
  std::vector<CodeTransition> transitions;
  std::vector<std::uint32_t> vectors;
  std::vector<std::byte> bitcode, artifact;
  std::vector<std::string> exports;
};

[[noreturn]] void refuse(std::string_view identity, std::uint32_t address, std::string_view reason);
[[nodiscard]] Image describe(const CodeImage &input, const CompileOptions &options);
void lower_llvm(Image &image);
void pack(Image &image, const CodeImage &input);
[[nodiscard]] Image
unpack(const CodeImage &input, const CompileOptions &options, std::span<const std::byte> artifact);
} // namespace armv5::detail
