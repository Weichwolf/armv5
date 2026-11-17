// SPDX-License-Identifier: GPL-3.0-only
#include "cache.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace armv5::detail {
namespace {
constexpr std::uint32_t line_mask = 0xffffffe0, byte_mask = 255, clean_bit = 0x40000000;
constexpr unsigned word_bytes = 4, half_line = 16, way_shift = 30, random_right_shift = 17;

unsigned index(std::uint32_t address) {
  return (address / DataCache::line_bytes) % DataCache::sets;
}

std::uint32_t read(std::span<const std::byte> bytes, bool big) {
  std::uint32_t value = 0;
  for (unsigned offset = 0; offset < bytes.size(); ++offset) {
    value |= std::to_integer<std::uint32_t>(bytes[offset])
             << ((big ? bytes.size() - 1 - offset : offset) * 8);
  }
  return value;
}

void write(std::span<std::byte> bytes, std::uint32_t value, bool big) {
  for (unsigned offset = 0; offset < bytes.size(); ++offset) {
    bytes[offset] =
        std::byte((value >> ((big ? bytes.size() - 1 - offset : offset) * 8)) & byte_mask);
  }
}
} // namespace

DataCache::Line *DataCache::find(std::uint32_t mva) {
  for (auto &line : lines[index(mva)]) {
    if (line.valid && line.virtual_base == (mva & line_mask)) { return &line; }
  }
  return nullptr;
}

unsigned DataCache::victim(Request request) const {
  const auto start = request.round_robin ? next[index(request.mva)] : random_state % ways;
  for (unsigned offset = 0; offset < ways; ++offset) {
    const auto way = (start + offset) % ways;
    if ((request.lockdown & (1U << way)) == 0) { return way; }
  }
  return ways - 1;
}

bool DataCache::clean(Line &line, bool big, Bus bus, void *context) {
  for (unsigned half = 0; half < 2; ++half) {
    if ((line.dirty & (1U << half)) == 0) { continue; }
    for (unsigned offset = half * half_line; offset < (half + 1) * half_line;
         offset += word_bytes) {
      auto value = read(std::span(line.bytes).subspan(offset, word_bytes), big);
      if (!bus(context,
               {.address = line.physical_base + offset,
                .value = value,
                .writing = true,
                .mva = line.virtual_base + offset},
               value)) {
        return false;
      }
    }
    line.dirty &= ~(1U << half);
  }
  return true;
}

DataCache::Line *DataCache::fill(Request request, Bus bus, void *context) {
  const auto set = index(request.mva);
  const auto way = victim(request);
  auto &target = lines[set][way];
  if (!clean(target, request.big, bus, context)) { return nullptr; }
  Line replacement{.virtual_base = request.mva & line_mask,
                   .physical_base = request.physical & line_mask};
  const auto first = (request.mva % line_bytes) / word_bytes;
  for (unsigned count = 0; count < line_bytes / word_bytes; ++count) {
    const auto offset = ((first + count) % (line_bytes / word_bytes)) * word_bytes;
    std::uint32_t value = 0;
    if (!bus(context,
             {.address = replacement.physical_base + offset,
              .value = 0,
              .writing = false,
              .mva = replacement.virtual_base + offset},
             value)) {
      return nullptr;
    }
    write(std::span(replacement.bytes).subspan(offset, word_bytes), value, request.big);
  }
  replacement.valid = true;
  target = replacement;
  next[set] = (way + 1) % ways;
  // A deterministic pseudo-random stream; exact hardware taps are not exposed.
  random_state ^= random_state << 13;
  random_state ^= random_state >> random_right_shift;
  random_state ^= random_state << 5;
  return &target;
}

std::optional<std::uint32_t> DataCache::access(Request request, Bus bus, void *context) {
  auto *line = find(request.mva);
  if (line == nullptr && request.operation == Operation::write) {
    std::uint32_t ignored = 0;
    if (!bus(context,
             {.address = request.physical,
              .value = request.value,
              .writing = true,
              .width = request.width,
              .mva = request.mva},
             ignored)) {
      return std::nullopt;
    }
    return 0;
  }
  if (line == nullptr) { line = fill(request, bus, context); }
  if (line == nullptr) { return std::nullopt; }
  auto bytes = std::span(line->bytes).subspan(request.mva % line_bytes, request.width);
  const auto old = read(bytes, request.big);
  if (request.operation != Operation::read) {
    if (!request.back) {
      std::uint32_t ignored = 0;
      if (!bus(context,
               {.address = request.physical,
                .value = request.value,
                .writing = true,
                .width = request.width,
                .mva = request.mva},
               ignored)) {
        return std::nullopt;
      }
    }
    write(bytes, request.value, request.big);
    if (request.back) { line->dirty |= 1U << ((request.mva % line_bytes) / half_line); }
  }
  return old;
}

void DataCache::invalidate() {
  for (auto &set : lines) {
    for (auto &line : set) { line = {}; }
  }
}

bool DataCache::test_clean(Maintenance request, Bus bus, void *context, std::uint32_t &result) {
  for (auto &set : lines) {
    for (auto &line : set) {
      if (line.dirty != 0) { return clean(line, request.big, bus, context); }
    }
  }
  result = clean_bit;
  if ((request.selector >> 3) == 14) { invalidate(); }
  return true;
}

bool DataCache::maintain(Maintenance request, Bus bus, void *context, std::uint32_t &result) {
  const auto [selector, operand, big] = request;
  const auto operation = selector & 7;
  const auto group = selector >> 3;
  if (operation == 4) { return true; } // All external writes complete synchronously.
  if (operation == 0) {
    invalidate();
    return true;
  }
  if (operation == 3) { return test_clean(request, bus, context, result); }
  auto *line = operation == 1 ? find(operand) : &lines[index(operand)][operand >> way_shift];
  if (line == nullptr) { return true; }
  if (group != 6 && !clean(*line, big, bus, context)) { return false; }
  if (group != 10) { *line = {}; }
  return true;
}
} // namespace armv5::detail
