// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace armv5::detail {
enum class Signal : std::uint8_t {
  none,
  yielded,
  changed,
  memory,
  unprepared,
  unsupported,
  transfer,
  status,
  exception_return,
  software_interrupt,
  undefined,
  prefetch_abort,
  data_abort,
  exchange,
  redispatch,
  yielded_exchange,
  yielded_exception_return,
  mapping_change
};

struct DirectRegion {
  std::uintptr_t address_bias;
  std::uint32_t safe_write;
};

struct Runtime {
  std::array<std::uint32_t, 16> r{};
  std::uint32_t cpsr = Registers::reset_cpsr, spsr = 0;
  State::Banks banks;
  std::uint64_t instructions = 0, cycles = 0, instruction_bound = 0, cycle_bound = 0;
  std::uint32_t signal = 0; // Fixed-width field in the private LLVM/native ABI.
  std::uint32_t address = 0;
  DirectRegion *direct = nullptr;
  void *owner = nullptr;
  using Access = std::uint64_t (*)(
      Runtime *, std::uint32_t, unsigned, unsigned, std::uint32_t, std::uintptr_t) noexcept;
  Access access = nullptr;
  Registers::System system;
  std::uint32_t tcm_status = 0;
};

static_assert(std::is_standard_layout_v<Runtime>);
using Entry = void (*)(Runtime *);

struct Native {
  std::shared_ptr<const Image> image;
  std::vector<Entry> entries;
  PreparationReport report;
  std::shared_ptr<const void> lifetime;
};

[[nodiscard]] Native native_bind(std::shared_ptr<const Image> image, const NativeSymbols &symbols);
[[nodiscard]] Native native_compile(std::shared_ptr<const Image> image);
} // namespace armv5::detail
