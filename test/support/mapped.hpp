// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fixture {
class AddressSpace {
public:
  static constexpr std::size_t size = std::uint64_t{1} << 32;
  static constexpr std::uint32_t data_address = 0x4000;

  AddressSpace() {
    auto path = std::to_array("/tmp/armv5-space-XXXXXX");
    file_ = mkstemp(path.data());
    if (file_ < 0) { fail(); }
    if (unlink(path.data()) != 0 || ftruncate(file_, static_cast<off_t>(size)) != 0) { fail(); }
    int flags = MAP_SHARED;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    auto *mapped = mmap(nullptr, size, PROT_NONE, flags, file_, 0);
    if (mapped == MAP_FAILED) { fail(); }
    base_ = static_cast<std::byte *>(mapped);
    if (mprotect(base_ + data_address, size - data_address, PROT_READ | PROT_WRITE) != 0) {
      fail();
    }
  }

  ~AddressSpace() {
    if (base_ != nullptr) { (void)munmap(base_, size); }
    if (file_ >= 0) { (void)close(file_); }
  }

  AddressSpace(const AddressSpace &) = delete;
  AddressSpace &operator=(const AddressSpace &) = delete;

  [[nodiscard]] std::span<std::byte> data() const {
    return {base_ + data_address, size - data_address};
  }

  [[nodiscard]] int descriptor() const { return file_; }

  [[nodiscard]] std::span<std::byte> code_storage() const {
    if (mprotect(base_, data_address, PROT_READ) != 0) {
      throw std::system_error(errno, std::generic_category(), "protect complete code storage");
    }
    return {base_, data_address};
  }

  void verify_tail(std::size_t used) const {
    fixture::require(used <= data().size(), "benchmark working storage fits address space");
    if (msync(base_, size, MS_SYNC) != 0) {
      throw std::system_error(errno, std::generic_category(), "flush benchmark storage");
    }
    auto offset = static_cast<off_t>(data_address + used);
    while (offset < static_cast<off_t>(size)) {
      const auto first = lseek(file_, offset, SEEK_DATA);
      if (first < 0) {
        if (errno == ENXIO) { break; }
        throw std::system_error(errno, std::generic_category(), "find benchmark data extent");
      }
      const auto end = lseek(file_, first, SEEK_HOLE);
      if (end < 0) {
        throw std::system_error(errno, std::generic_category(), "find benchmark hole");
      }
      fixture::require(first >= offset && end > first && end <= static_cast<off_t>(size),
                       "benchmark sparse extent bounds");
      const auto bytes = std::span(base_ + first, static_cast<std::size_t>(end - first));
      fixture::require(
          std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte{}; }),
          "benchmark storage beyond working buffer remains zero");
      offset = end;
    }
  }

  [[nodiscard]] std::span<std::byte> code(const fixture::Program &program) const {
    const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const auto first = program.address() / page_size * page_size;
    const auto end = (program.pc() + page_size - 1) / page_size * page_size;
    fixture::require(end <= data_address, "benchmark code pages are below data storage");
    if (mprotect(base_ + first, end - first, PROT_READ | PROT_WRITE) != 0) {
      throw std::system_error(errno, std::generic_category(), "initialize benchmark code mapping");
    }
    auto bytes = std::span(base_ + program.address(), program.bytes().size());
    std::ranges::copy(program.bytes(), bytes.begin());
    if (mprotect(base_ + first, end - first, PROT_READ) != 0) {
      throw std::system_error(errno, std::generic_category(), "protect benchmark code mapping");
    }
    return bytes;
  }

private:
  int file_ = -1;
  std::byte *base_ = nullptr;

  [[noreturn]] void fail() {
    const auto error = errno;
    if (base_ != nullptr) { (void)munmap(base_, size); }
    if (file_ >= 0) { (void)close(file_); }
    throw std::system_error(error, std::generic_category(), "create benchmark address space");
  }
};

class MappedMachine {
public:
  enum class Preparation : std::uint8_t { deferred, immediate };
  static constexpr auto default_mode = armv5::CompileOptions::MemoryMode::exact;
  AddressSpace space_;
  std::pmr::monotonic_buffer_resource allocation_{
      space_.data().data(), space_.data().size(), std::pmr::null_memory_resource()};
  std::vector<fixture::ModeProgram> programs_;
  std::vector<std::uint32_t> vectors_, entries_;
  std::vector<armv5::CodeSpan> spans_;
  std::vector<armv5::Memory::Region> regions_;
  std::vector<armv5::RegionLayout> layout_;
  bool fixed_;
  armv5::CompileOptions options_;
  std::optional<armv5::Registers::System::Mapping> mapping_;
  std::optional<armv5::NativeImage> native_;
  double installation_seconds = 0;

  // Copy construction uses the default resource, preserving independent RAM oracles.
  std::pmr::vector<std::byte> ram{&allocation_};
  std::unique_ptr<armv5::Core> core;

  explicit MappedMachine(const fixture::Program &program,
                         std::size_t ram_size = 0x10000,
                         armv5::CompileOptions::MemoryMode mode = default_mode)
      : MappedMachine(
            {{.program = program, .modes = {}}}, {}, true, ram_size, Preparation::deferred, mode) {}

  MappedMachine(std::vector<fixture::ModeProgram> programs,
                std::vector<std::uint32_t> vectors,
                bool fixed,
                std::size_t ram_size,
                armv5::CompileOptions::MemoryMode mode = default_mode,
                Preparation preparation = Preparation::immediate)
      : MappedMachine(std::move(programs), std::move(vectors), fixed, ram_size, preparation, mode) {
  }

  void prepare() {
    const std::array variants{
        armv5::CodeVariant{.id = 0, .spans = spans_, .entries = entries_, .mapping = mapping_}};
    const std::array layouts{armv5::VariantLayout{.variant = 0, .regions = layout_}};
    const armv5::CodeImage image{.identity = "authored-full-space-benchmark",
                                 .variants = variants,
                                 .layouts = fixed_ ? std::span<const armv5::VariantLayout>(layouts)
                                                   : std::span<const armv5::VariantLayout>{},
                                 .transitions = {},
                                 .interrupt_vectors = vectors_};
    native_.emplace(armv5::Core::compile(image, options_));
    install();
  }

  void install() {
    const auto start = std::chrono::steady_clock::now();
    (void)core->install(prepared(), 0);
    installation_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  }

  [[nodiscard]] const armv5::NativeImage &prepared() const {
    if (!native_) { throw std::logic_error("benchmark image has not been compiled"); }
    return *native_;
  }

  void verify_storage() const { space_.verify_tail(ram.size()); }

  void registers(const armv5::Registers &registers) const { core->set_registers(registers); }

  [[nodiscard]] armv5::RunResult run(std::uint64_t budget) const {
    auto result = core->run(budget);
    fixture::require(result.execution == armv5::ExecutionState::ready, result.fault.reason);
    return result;
  }

private:
  MappedMachine(std::vector<fixture::ModeProgram> programs,
                std::vector<std::uint32_t> vectors,
                bool fixed,
                std::size_t ram_size,
                Preparation preparation,
                armv5::CompileOptions::MemoryMode mode)
      : programs_(std::move(programs)), vectors_(std::move(vectors)), fixed_(fixed) {
    options_.memory_mode = mode;
    ram.resize(ram_size);
    for (const auto &item : programs_) {
      spans_.push_back({.address = item.program.address(),
                        .bytes = item.program.bytes(),
                        .processor_modes = item.modes,
                        .instruction_set = item.instruction_set});
      entries_.push_back(item.program.address());
      auto bytes = space_.code(item.program);
      if (mode == armv5::CompileOptions::MemoryMode::exact) {
        regions_.push_back({.address = item.program.address(),
                            .bytes = bytes,
                            .readable = true,
                            .writable = false,
                            .executable = true});
      }
    }
    regions_.push_back({.address = AddressSpace::data_address, .bytes = space_.data()});
    if (mode == armv5::CompileOptions::MemoryMode::direct) {
      regions_.insert(regions_.begin(),
                      {.address = 0,
                       .bytes = space_.code_storage(),
                       .readable = true,
                       .writable = false,
                       .executable = true});
      for (auto &region : regions_) {
        region.mapping_fd = space_.descriptor();
        region.mapping_offset = region.address;
      }
    }
    for (const auto &region : regions_) {
      layout_.push_back({.address = region.address,
                         .size = region.bytes.size(),
                         .readable = region.readable,
                         .writable = region.writable,
                         .executable = region.executable,
                         .shared_mapping = region.mapping_fd >= 0});
    }
    core = std::make_unique<armv5::Core>(armv5::Memory{.regions = regions_, .generation = 0});
    auto registers = core->registers();
    registers.r[15] = programs_.front().program.address();
    core->set_registers(registers);
    if (preparation == Preparation::immediate) { prepare(); }
  }
};

} // namespace fixture
