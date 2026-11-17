// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"

#include <array>
#include <cstdint>

namespace armv5::detail {
struct Mmu {
  enum class ReadStatus : std::uint8_t { ready, external_abort, host_fault, callback };
  enum class Status : std::uint8_t { ready, fault, host_fault, callback, unpredictable };

  struct ReadResult {
    ReadStatus status;
    std::uint32_t value;
  };

  using Read = ReadResult (*)(void *, std::uint32_t);

  struct Access {
    bool privileged, writing, commit, check = true;
  };

  struct Translation {
    Status status = Status::ready;
    std::uint32_t mva = 0, physical = 0, available = 0, fault_status = 0, fault_address = 0;
    std::uint32_t attributes = 0;
    bool tiny = false;
  };

  struct Entry {
    std::uint32_t virtual_base = 0, physical_base = 0, size = 0;
    std::uint8_t domain = 0, permission = 0, attributes = 0;
    bool section = false, tiny = false;
  };

  // The normal TLB's placement/replacement is outside the programmer's model.
  // Retain 64 translations with deterministic replacement and eight locked slots.
  std::array<Entry, 64> normal{};
  std::array<Entry, 8> locked{};
  unsigned victim = 0;

  [[nodiscard]] static std::uint32_t modified(const Registers::System::Mapping &mapping,
                                              std::uint32_t address);
  [[nodiscard]] Translation translate(
      Registers::System &system, std::uint32_t address, Access access, Read read, void *context);
  void invalidate(bool single, std::uint32_t mva);

private:
  [[nodiscard]] const Entry *find(std::uint32_t mva) const;
  void insert(const Entry &entry, Registers::System &system);

  struct Walk {
    Translation result;
    Entry entry;
  };

  [[nodiscard]] static Walk
  walk(const Registers::System &system, std::uint32_t mva, Read read, void *context);
  [[nodiscard]] static Translation permissions(const Registers::System &system,
                                               const Entry &entry,
                                               std::uint32_t mva,
                                               Access access);
};
} // namespace armv5::detail
