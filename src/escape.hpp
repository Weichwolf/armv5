// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "runtime.hpp"

#include <atomic>
#include <cstdint>
#include <span>

extern "C" {
#include <setjmp.h>
}

namespace armv5::detail {
class Escape {
public:
  struct Fault {
    std::uint32_t address;
    std::uintptr_t native_pc;
    bool writing;
  };

  class Suspend {
  public:
    Suspend() noexcept;
    ~Suspend();
    Suspend(const Suspend &) = delete;
    Suspend &operator=(const Suspend &) = delete;

  private:
    Escape *previous_;
  };

  [[nodiscard]] bool
  invoke(Entry entry, Runtime &runtime, std::span<const TextRange> ranges, std::uintptr_t base);
  [[nodiscard]] Fault fault() const noexcept;
  [[nodiscard]] static bool
  host_fault(std::uintptr_t address, bool writing, std::uintptr_t native_pc) noexcept;

private:
  static thread_local std::atomic<Escape *> active_;
  std::span<const TextRange> ranges_;
  std::uintptr_t base_ = 0;
  Escape *previous_ = nullptr;
  sigjmp_buf jump_{};
  std::atomic<std::uintptr_t> address_{0}, native_pc_{0};
  std::atomic<bool> writing_{false};
};
} // namespace armv5::detail
