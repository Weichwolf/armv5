// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"
#include "support/isa.hpp"
#include "support/mapped.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace benchmark {
enum class Mapping : std::uint8_t { off, identity, remapped };

struct Configuration {
  Mapping mapping = Mapping::off;
  armv5::CompileOptions::MemoryMode memory = armv5::CompileOptions::MemoryMode::direct;
};

inline Configuration configuration;

class Machine : public fixture::MappedMachine {
public:
  explicit Machine(const fixture::Program &program, std::size_t size = 0x10000)
      : MappedMachine(program, storage_size(size), armv5::CompileOptions::MemoryMode::direct) {
    configure(size);
  }

  Machine(std::vector<fixture::ModeProgram> programs,
          std::vector<std::uint32_t> vectors,
          bool fixed,
          std::size_t size)
      : MappedMachine(std::move(programs),
                      std::move(vectors),
                      fixed,
                      storage_size(size),
                      armv5::CompileOptions::MemoryMode::direct,
                      Preparation::deferred) {
    configure(size);
    prepare();
  }

  [[nodiscard]] armv5::Registers expected_registers() const {
    armv5::Registers result;
    if (mapping_) { result.system.mapping = *mapping_; }
    return result;
  }

  void registers(armv5::Registers values) {
    if (mapping_) { values.system.mapping = *mapping_; }
    core->set_registers(values);
    if (mapping_) { install(); }
  }

private:
  static constexpr std::size_t table_size = 0x4000;
  static constexpr std::uint32_t remap_bit = 0x80000000;
  static constexpr unsigned section_shift = 20;

  [[nodiscard]] static std::size_t table_address(std::size_t working_size) {
    return (fixture::AddressSpace::data_address + working_size + table_size - 1) / table_size *
           table_size;
  }

  [[nodiscard]] static std::size_t storage_size(std::size_t working_size) {
    return configuration.mapping == Mapping::off
               ? working_size
               : table_address(working_size) + table_size - fixture::AddressSpace::data_address;
  }

  void configure(std::size_t working_size) {
    options_.memory_mode = configuration.memory;
    if (configuration.mapping == Mapping::off) { return; }
    const auto table = static_cast<std::uint32_t>(table_address(working_size));
    const auto bias = configuration.mapping == Mapping::remapped ? remap_bit : 0;
    constexpr std::uint32_t section_ap3 = (3U << 10) | 2;
    for (std::uint32_t section = 0; section < table_size / 4; ++section) {
      fixture::put(std::span(ram).subspan(
                       table - fixture::AddressSpace::data_address + (std::size_t(section) * 4), 4),
                   ((section << section_shift) ^ bias) | section_ap3);
    }
    if (bias != 0) {
      std::vector<armv5::Memory::Region> physical;
      for (const auto &region : regions_) {
        for (std::size_t offset = 0; offset < region.bytes.size();) {
          const auto logical = region.address + static_cast<std::uint32_t>(offset);
          const auto count = std::min(region.bytes.size() - offset,
                                      std::size_t(remap_bit - (logical & (remap_bit - 1))));
          auto part = region;
          part.address = logical ^ bias;
          part.bytes = region.bytes.subspan(offset, count);
          part.mapping_offset += offset;
          physical.push_back(part);
          offset += count;
        }
      }
      regions_ = std::move(physical);
      layout_.clear();
      for (const auto &region : regions_) {
        layout_.push_back({.address = region.address,
                           .size = region.bytes.size(),
                           .readable = region.readable,
                           .writable = region.writable,
                           .executable = region.executable,
                           .shared_mapping = true});
      }
      core->set_memory({.regions = regions_, .generation = 1});
    }
    mapping_ = armv5::Registers::System::Mapping{};
    mapping_->control |= 1;
    mapping_->translation_table = table ^ bias;
    mapping_->domains = 1;
    auto values = core->registers();
    values.system.mapping = *mapping_;
    core->set_registers(values);
  }
};
} // namespace benchmark
