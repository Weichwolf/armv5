// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace armv5::detail {
struct DataCache {
  static constexpr unsigned sets = 64, ways = 4, line_bytes = 32;
  enum class Operation : std::uint8_t { read, write, swap };

  struct Request {
    std::uint32_t mva, physical, value, lockdown;
    unsigned width;
    Operation operation;
    bool big, back, round_robin;
  };

  struct BusRequest {
    std::uint32_t address, value;
    bool writing;
    unsigned width = 4;
    std::uint32_t mva = 0;
  };

  using Bus = bool (*)(void *, BusRequest, std::uint32_t &);

  struct Line {
    std::uint32_t virtual_base = 0, physical_base = 0;
    std::array<std::byte, line_bytes> bytes{};
    std::uint8_t dirty = 0;
    bool valid = false;
  };

  std::array<std::array<Line, ways>, sets> lines{};
  std::array<unsigned, sets> next{};
  std::uint32_t random_state = 1;

  [[nodiscard]] std::optional<std::uint32_t> access(Request request, Bus bus, void *context);

  struct Maintenance {
    unsigned selector;
    std::uint32_t operand;
    bool big;
  };

  [[nodiscard]] bool maintain(Maintenance request, Bus bus, void *context, std::uint32_t &result);

private:
  [[nodiscard]] Line *find(std::uint32_t mva);
  [[nodiscard]] unsigned victim(Request request) const;
  [[nodiscard]] static bool clean(Line &line, bool big, Bus bus, void *context);
  void invalidate();
  [[nodiscard]] bool test_clean(Maintenance request, Bus bus, void *context, std::uint32_t &result);
  [[nodiscard]] Line *fill(Request request, Bus bus, void *context);
};
} // namespace armv5::detail
