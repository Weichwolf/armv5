// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>

namespace armv5::detail {
class View {
public:
  static constexpr std::uint64_t size = std::uint64_t{1} << 32;

  struct Piece {
    std::uint64_t size = 0, offset = 0;
    int descriptor = -1;
    bool readable = false, writable = false, exact = false;
  };

  using Resolve = Piece (*)(void *, std::uint32_t);

  View() = default;
  ~View();
  View(const View &) = delete;
  View &operator=(const View &) = delete;

  [[nodiscard]] bool build(Resolve resolve, void *context);
  void clear() noexcept;
  [[nodiscard]] std::uintptr_t base() const noexcept;
  [[nodiscard]] static std::uint64_t page_size();

private:
  std::byte *base_ = nullptr;
};
} // namespace armv5::detail
