// SPDX-License-Identifier: GPL-3.0-only
#include "view.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace armv5::detail {
namespace {
struct Mapping {
  std::uint64_t address, size, offset;
  int descriptor, protection;
};

bool continuous(const Mapping &mapping, const View::Piece &piece, int protection) {
  return mapping.protection == protection &&
         (protection == 0 || (mapping.descriptor == piece.descriptor &&
                              mapping.offset + mapping.size == piece.offset));
}

bool aligned(const Mapping &mapping, std::uint64_t page) {
  return mapping.address % page == 0 && mapping.size % page == 0 &&
         (mapping.protection == 0 || (mapping.descriptor >= 0 && mapping.offset % page == 0));
}
} // namespace

View::~View() {
  clear();
}

void View::clear() noexcept {
  if (base_ != nullptr) { (void)munmap(base_, size); }
  base_ = nullptr;
}

std::uintptr_t View::base() const noexcept {
  return reinterpret_cast<std::uintptr_t>(base_);
}

std::uint64_t View::page_size() {
  const auto result = sysconf(_SC_PAGESIZE);
  if (result <= 0) { throw std::runtime_error("host page size is unavailable"); }
  return static_cast<std::uint64_t>(result);
}

bool View::build(Resolve resolve, void *context) {
  clear();
  std::vector<Mapping> mappings;
  for (std::uint64_t address = 0; address < size;) {
    const auto piece = resolve(context, static_cast<std::uint32_t>(address));
    if (piece.exact || (piece.writable && !piece.readable)) { return false; }
    if (piece.size == 0) { throw std::logic_error("empty host-view mapping piece"); }
    const auto count = std::min(piece.size, size - address);
    const auto protection = (piece.readable ? PROT_READ : 0) | (piece.writable ? PROT_WRITE : 0);
    const bool merge = !mappings.empty() && continuous(mappings.back(), piece, protection);
    if (merge) {
      mappings.back().size += count;
    } else {
      mappings.push_back({.address = address,
                          .size = count,
                          .offset = piece.offset,
                          .descriptor = piece.descriptor,
                          .protection = protection});
    }
    address += count;
  }
  const auto page = page_size();
  for (const auto &mapping : mappings) {
    if (!aligned(mapping, page)) { return false; }
  }
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
  flags |= MAP_NORESERVE;
#endif
  auto *reservation = mmap(nullptr, size, PROT_NONE, flags, -1, 0);
  if (reservation == MAP_FAILED) {
    throw std::system_error(errno, std::generic_category(), "reserve ARM host view");
  }
  base_ = static_cast<std::byte *>(reservation);
  for (const auto &mapping : mappings) {
    if (mapping.protection == 0) { continue; }
    auto *result = mmap(base_ + mapping.address,
                        mapping.size,
                        mapping.protection,
                        MAP_FIXED | MAP_SHARED,
                        mapping.descriptor,
                        static_cast<off_t>(mapping.offset));
    if (result == MAP_FAILED) {
      const auto error = errno;
      clear();
      throw std::system_error(error, std::generic_category(), "map ARM shared backing");
    }
  }
  return true;
}
} // namespace armv5::detail
