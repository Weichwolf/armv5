// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace armv5 {

// Addresses and sizes are bytes. Aliases lend overlapping host spans. The owner
// changes executable bytes/mappings only while stopped, then calls set_memory
// with a new generation and installs a declared variant before executing again.
struct Memory {
  struct Region {
    std::uint32_t address = 0;
    std::span<std::byte> bytes;
    bool readable = true, writable = true, executable = false;
  };

  std::span<const Region> regions;
  std::uint64_t generation = 0;
};

// Synchronous owner-thread callbacks; false is a fault. They must not throw or
// reenter execution. Values use guest integer significance, independent of host
// endian. Access widths are bytes (1, 2 or 4); atomic swap is one callback.
struct Io {
  void *owner = nullptr;
  bool (*read)(void *,
               std::uint32_t address,
               unsigned width,
               std::uint32_t &value) noexcept = nullptr;
  bool (*write)(void *,
                std::uint32_t address,
                unsigned width,
                std::uint32_t value) noexcept = nullptr;
  bool (*swap)(void *,
               std::uint32_t address,
               unsigned width,
               std::uint32_t &value) noexcept = nullptr;
};

} // namespace armv5
