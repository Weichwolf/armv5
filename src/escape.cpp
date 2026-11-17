// SPDX-License-Identifier: GPL-3.0-only
#include "escape.hpp"

#include "runtime.hpp"
#include "view.hpp"

#include <atomic>
#include <cstdint>
#include <span>

extern "C" {
#include <setjmp.h>
}

namespace armv5::detail {
thread_local std::atomic<Escape *> Escape::active_{nullptr};
static_assert(std::atomic<Escape *>::is_always_lock_free);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

Escape::Suspend::Suspend() noexcept
    : previous_(active_.exchange(nullptr, std::memory_order_relaxed)) {}

Escape::Suspend::~Suspend() {
  active_.store(previous_, std::memory_order_relaxed);
}

bool Escape::invoke(Entry entry,
                    Runtime &runtime,
                    std::span<const TextRange> ranges,
                    std::uintptr_t base) {
  ranges_ = ranges;
  base_ = base;
  previous_ = active_.load(std::memory_order_relaxed);
  if (sigsetjmp(jump_, 0) == 0) {
    active_.store(this, std::memory_order_relaxed);
    entry(&runtime);
    active_.store(previous_, std::memory_order_relaxed);
    return true;
  }
  active_.store(previous_, std::memory_order_relaxed);
  return false;
}

Escape::Fault Escape::fault() const noexcept {
  return {.address = static_cast<std::uint32_t>(address_.load(std::memory_order_relaxed) - base_),
          .native_pc = native_pc_.load(std::memory_order_relaxed),
          .writing = writing_.load(std::memory_order_relaxed)};
}

bool Escape::host_fault(std::uintptr_t address, bool writing, std::uintptr_t native_pc) noexcept {
  auto *owner = active_.load(std::memory_order_relaxed);
  if (owner == nullptr || owner->base_ == 0 || address - owner->base_ >= View::size) {
    return false;
  }
  for (const auto range : owner->ranges_) {
    if (native_pc - range.address >= range.size) { continue; }
    owner->address_.store(address, std::memory_order_relaxed);
    owner->native_pc_.store(native_pc, std::memory_order_relaxed);
    owner->writing_.store(writing, std::memory_order_relaxed);
    siglongjmp(owner->jump_, 1);
  }
  return false;
}
} // namespace armv5::detail
