// SPDX-License-Identifier: GPL-3.0-only
#include "mmu.hpp"

#include "armv5/core.hpp"

#include <array>
#include <cstdint>

namespace armv5::detail {
namespace {
constexpr std::uint32_t process_window = 0x02000000, table_mask = 0xffffc000,
                        section_mask = 0xfff00000, coarse_mask = 0xfffffc00, fine_mask = 0xfffff000,
                        coarse_index = 0x3fc, fine_index = 0xffc, first_index = 0x3ffc,
                        repeated_ap = 0x55, tiny_offset = 1023, large_mask = 0xffff0000,
                        small_mask = 0xfffff000, tiny_mask = 0xfffffc00;
constexpr unsigned victim_shift = 26;

bool contains(const Mmu::Entry &entry, std::uint32_t address) {
  return entry.size != 0 && address - entry.virtual_base < entry.size;
}

Mmu::Translation
failure(std::uint32_t mva, std::uint32_t status, std::uint32_t available = 0, bool tiny = false) {
  return {.status = Mmu::Status::fault,
          .mva = mva,
          .available = available,
          .fault_status = status,
          .tiny = tiny};
}

Mmu::Translation
read_failure(Mmu::ReadResult read, Mmu::Translation result, std::uint32_t descriptor_address) {
  if (read.status == Mmu::ReadStatus::host_fault) { result.status = Mmu::Status::host_fault; }
  if (read.status == Mmu::ReadStatus::callback) { result.status = Mmu::Status::callback; }
  result.fault_address = descriptor_address;
  return result;
}

struct Descriptor {
  std::uint32_t value;
  unsigned kind, domain;
};

Mmu::Entry page(std::uint32_t mva, Descriptor input) {
  const auto [descriptor, kind, domain] = input;
  constexpr std::array<std::uint32_t, 4> sizes{0, 65536, 4096, 1024};
  constexpr std::array<std::uint32_t, 4> masks{0, large_mask, small_mask, tiny_mask};
  auto size = sizes[kind];
  const auto mask = masks[kind];
  const auto permissions = (descriptor >> 4) & 255;
  auto permission = permissions & 3;
  if (kind != 3) {
    const auto quarter = kind == 1 ? (mva >> 14) & 3 : (mva >> 10) & 3;
    permission = (permissions >> (quarter * 2)) & 3;
    if (permissions != (permission * repeated_ap)) { size /= 4; }
  }
  return {.virtual_base = mva & ~(size - 1),
          .physical_base = (descriptor & mask) | (mva & ~mask & ~(size - 1)),
          .size = size,
          .domain = static_cast<std::uint8_t>(domain),
          .permission = static_cast<std::uint8_t>(permission),
          .attributes = static_cast<std::uint8_t>((descriptor >> 2) & 3),
          .section = false,
          .tiny = kind == 3};
}
} // namespace

std::uint32_t Mmu::modified(const Registers::System::Mapping &mapping, std::uint32_t address) {
  return (mapping.control & 1) != 0 && address < process_window ? address | mapping.process_id
                                                                : address;
}

const Mmu::Entry *Mmu::find(std::uint32_t mva) const {
  for (const auto &entry : locked) {
    if (contains(entry, mva)) { return &entry; }
  }
  for (const auto &entry : normal) {
    if (contains(entry, mva)) { return &entry; }
  }
  return nullptr;
}

void Mmu::insert(const Entry &entry, Registers::System &system) {
  auto &lockdown = system.tlb_lockdown;
  if ((lockdown & 1) != 0) {
    const auto selected = (lockdown >> victim_shift) & 7;
    locked[selected] = entry;
    lockdown = ((selected + 1) % 8 << victim_shift) | 1;
  } else {
    normal[victim] = entry;
    victim = (victim + 1) % 64;
  }
}

Mmu::Walk Mmu::walk(const Registers::System &system, std::uint32_t mva, Read read, void *context) {
  const auto first_address =
      (system.mapping.translation_table & table_mask) | ((mva >> 18) & first_index);
  const auto first = read(context, first_address);
  if (first.status != ReadStatus::ready) {
    return {.result = read_failure(first, failure(mva, 12), first_address), .entry = {}};
  }
  const auto kind = first.value & 3;
  if (kind == 0) {
    return {.result = failure(mva, 5, 0x100000 - (mva & ~section_mask)), .entry = {}};
  }
  const auto domain = (first.value >> 5) & 15;
  if (kind == 2) {
    return {.result = {.mva = mva},
            .entry = {.virtual_base = mva & section_mask,
                      .physical_base = first.value & section_mask,
                      .size = 0x100000,
                      .domain = static_cast<std::uint8_t>(domain),
                      .permission = static_cast<std::uint8_t>((first.value >> 10) & 3),
                      .attributes = static_cast<std::uint8_t>((first.value >> 2) & 3),
                      .section = true}};
  }
  const bool fine = kind == 3;
  const auto second_address = (first.value & (fine ? fine_mask : coarse_mask)) |
                              ((mva >> (fine ? 8 : 10)) & (fine ? fine_index : coarse_index));
  const auto second = read(context, second_address);
  if (second.status != ReadStatus::ready) {
    return {.result = read_failure(second, failure(mva, (domain << 4) | 14), second_address),
            .entry = {}};
  }
  const auto page_kind = second.value & 3;
  if (page_kind == 0) {
    const auto size = fine ? 1024U : 4096U;
    return {.result = failure(mva, (domain << 4) | 7, size - (mva & (size - 1))), .entry = {}};
  }
  if (!fine && page_kind == 3) {
    return {.result = {.status = Status::unpredictable, .mva = mva}, .entry = {}};
  }
  return {.result = {.mva = mva},
          .entry = page(mva, {.value = second.value, .kind = page_kind, .domain = domain})};
}

Mmu::Translation Mmu::permissions(const Registers::System &system,
                                  const Entry &entry,
                                  std::uint32_t mva,
                                  Access access) {
  const auto domain = (system.mapping.domains >> (entry.domain * 2)) & 3;
  const auto fault_domain = std::uint32_t(entry.domain) << 4;
  if (access.check && (domain == 0 || domain == 2)) {
    return failure(mva,
                   fault_domain | (entry.section ? 9 : 11),
                   entry.size - (mva - entry.virtual_base),
                   entry.tiny);
  }
  bool allowed = !access.check || domain == 3;
  if (!allowed) {
    switch (entry.permission) {
      case 0: {
        const auto protection = (system.mapping.control >> 8) & 3;
        if (protection == 3) { return {.status = Status::unpredictable, .mva = mva}; }
        allowed = !access.writing && (protection == 2 || (protection == 1 && access.privileged));
        break;
      }
      case 1: allowed = access.privileged; break;
      case 2: allowed = access.privileged || !access.writing; break;
      case 3: allowed = true; break;
      default: break;
    }
  }
  if (!allowed) {
    return failure(mva,
                   fault_domain | (entry.section ? 13 : 15),
                   entry.size - (mva - entry.virtual_base),
                   entry.tiny);
  }
  return {.mva = mva,
          .physical = entry.physical_base + (mva - entry.virtual_base),
          .available = entry.size - (mva - entry.virtual_base),
          .attributes = entry.attributes,
          .tiny = entry.tiny};
}

Mmu::Translation Mmu::translate(
    Registers::System &system, std::uint32_t address, Access access, Read read, void *context) {
  const auto mva = modified(system.mapping, address);
  if ((system.mapping.control & 1) == 0) {
    return {.mva = mva, .physical = mva, .available = 1024 - (mva & tiny_offset)};
  }
  if (const auto *entry = find(mva)) { return permissions(system, *entry, mva, access); }
  const auto walked = walk(system, mva, read, context);
  if (walked.result.status != Status::ready) { return walked.result; }
  if (access.commit) { insert(walked.entry, system); }
  return permissions(system, walked.entry, mva, access);
}

void Mmu::invalidate(bool single, std::uint32_t mva) {
  for (auto &entry : normal) {
    if (!single || contains(entry, mva)) { entry = {}; }
  }
  if (single) {
    for (auto &entry : locked) {
      if (contains(entry, mva)) { entry = {}; }
    }
  }
}
} // namespace armv5::detail
