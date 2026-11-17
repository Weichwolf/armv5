// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"

#include "armv5/memory.hpp"
#include "escape.hpp"
#include "identity.hpp"
#include "image.hpp"
#include "mmu.hpp"
#include "runtime.hpp"
#include "system.hpp"
#include "view.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <tuple>
#include <utility>
#include <vector>

namespace armv5 {
struct SemanticImage::Impl : detail::Image {};

SemanticImage::SemanticImage(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

std::span<const std::byte> SemanticImage::bitcode() const {
  return impl_->bitcode;
}

std::span<const std::byte> SemanticImage::artifact() const {
  return impl_->artifact;
}

std::span<const std::string> SemanticImage::exports() const {
  return impl_->exports;
}

std::string_view SemanticImage::identity() const {
  return impl_->identity;
}

struct NativeImage::Impl : detail::Native {};

NativeImage::NativeImage(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

const PreparationReport &NativeImage::report() const {
  return impl_->report;
}

struct Snapshot::Impl {
  State state;
  bool external_entry;
  std::string pending_image;
  detail::Mmu mmu;
};

Snapshot::Snapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

namespace {
constexpr std::uint64_t access_failure = std::uint64_t{1} << 32;
constexpr std::uint32_t mode_bits = 0x1f, execution_bits = 0x01000020, thumb_bit = 0x20,
                        jazelle_bit = 0x01000000, byte_mask = 255;

struct HostRange {
  std::uintptr_t address;
  std::size_t size;
};

bool overlaps(HostRange first, HostRange second) {
  return first.address <= second.address ? second.address - first.address < first.size
                                         : first.address - second.address < second.size;
}

std::uint32_t read_bytes(std::span<const std::byte> bytes, bool big_endian) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    value |= std::to_integer<std::uint32_t>(bytes[index])
             << ((big_endian ? bytes.size() - 1 - index : index) * 8);
  }
  return value;
}

void write_bytes(std::span<std::byte> bytes, std::uint32_t value, bool big_endian) {
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] =
        std::byte((value >> ((big_endian ? bytes.size() - 1 - index : index) * 8)) & byte_mask);
  }
}
} // namespace

struct Core::Impl {
  detail::Runtime runtime;
  std::vector<Memory::Region> regions;
  Io io;
  std::uint64_t generation = 0;
  State continuation;
  bool running = false, external_entry = true, revoked = false, poisoned = false;
  detail::Escape escape;
  std::shared_ptr<const detail::Native> binding;
  std::size_t variant_index = 0;
  std::uint32_t revocation_address = 0;
  std::array<detail::Entry, 64> mode_entries{};
  std::vector<detail::DirectRegion> direct;
  std::vector<HostRange> code_ranges;
  std::string identity, pending_image;
  detail::Mmu mmu;
  detail::View view;
  bool view_ready = false, view_privileged = true;

  struct Backing {
    dev_t device;
    ino_t inode;
    int descriptor;
  };

  std::vector<Backing> backings;

  struct BackingRange {
    int descriptor;
    std::uint64_t first, end;
  };

  std::vector<BackingRange> code_backings, protected_backings;

  struct CodeBinding {
    std::uint32_t address, physical, size;
  };

  std::vector<CodeBinding> code_bindings;

  Impl(Memory memory, Io callbacks) : io(callbacks) {
    runtime.owner = this;
    runtime.access = &access;
    runtime.virtual_access = &virtual_access;
    runtime.fetch = &fetch;
    runtime.invalidate = &invalidate;
    runtime.invalidate_code = &invalidate_code;
    rebind(memory);
  }

  void stopped() const {
    if (running) { throw std::logic_error("Core mutation/execution reentry from callback"); }
    if (poisoned) { throw std::logic_error("Core is unusable after a fatal Direct host fault"); }
  }

  static std::size_t saved_index(std::uint32_t mode) {
    switch (mode) {
      case detail::fiq_mode: return 0;
      case detail::irq_mode: return 1;
      case detail::supervisor_mode: return 2;
      case detail::abort_mode: return 3;
      case detail::undefined_mode: return 4;
      default: return 5;
    }
  }

  static std::span<std::uint32_t> pair(State::Banks &banks, std::uint32_t mode) {
    switch (mode) {
      case detail::fiq_mode: return std::span(banks.fiq).subspan(5);
      case detail::irq_mode: return banks.irq;
      case detail::supervisor_mode: return banks.supervisor;
      case detail::abort_mode: return banks.abort;
      case detail::undefined_mode: return banks.undefined;
      default: return std::span(banks.user).subspan(5);
    }
  }

  void save_bank(State::Banks &banks) const {
    const auto mode = runtime.cpsr & mode_bits;
    auto &high = mode == detail::fiq_mode ? banks.fiq : banks.user;
    std::copy_n(runtime.r.begin() + 8, 5, high.begin());
    auto bank = pair(banks, mode);
    std::copy_n(runtime.r.begin() + 13, 2, bank.begin());
    const auto saved = saved_index(mode);
    if (saved < banks.spsr.size()) { banks.spsr[saved] = runtime.spsr; }
  }

  void change_status(std::uint32_t status) {
    save_bank(runtime.banks);
    const auto mode = status & mode_bits;
    const auto &high = mode == detail::fiq_mode ? runtime.banks.fiq : runtime.banks.user;
    std::copy_n(high.begin(), 5, runtime.r.begin() + 8);
    const auto bank = pair(runtime.banks, mode);
    std::copy_n(bank.begin(), 2, runtime.r.begin() + 13);
    const auto saved = saved_index(mode);
    runtime.spsr = saved < runtime.banks.spsr.size() ? runtime.banks.spsr[saved] : 0;
    runtime.cpsr = status;
  }

  [[nodiscard]] bool declared_vector(std::uint32_t vector, std::uint32_t mode) const {
    if (!binding || mode_entries[mode] == nullptr) { return false; }
    const auto &image = *binding->image;
    if (std::ranges::find(image.vectors, vector) == image.vectors.end()) { return false; }
    return std::ranges::any_of(image.variants[variant_index].spans,
                               [vector, mode](const detail::Span &span) {
                                 return !span.thumb && vector >= span.address &&
                                        vector - span.address < span.bytes.size() &&
                                        std::ranges::find(span.modes, mode) != span.modes.end();
                               });
  }

  struct Exception {
    std::uint32_t vector, mode;
  };

  bool exception(Exception target, std::uint32_t link, bool fast, bool charge) {
    const auto mode = target.mode;
    const auto vector =
        target.vector | ((runtime.system.mapping.control & 0x2000) != 0 ? 0xffff0000U : 0U);
    if (!declared_vector(vector, mode)) {
      failure(Fault::Kind::unprepared, vector, "exception vector/mode was not declared");
      return false;
    }
    const auto status = runtime.cpsr;
    constexpr std::uint32_t irq_mask = 0x80;
    constexpr std::uint32_t fiq_mask = 0x40;
    change_status((status & ~(mode_bits | execution_bits)) | mode | irq_mask |
                  (fast ? fiq_mask : 0));
    runtime.spsr = status;
    runtime.r[14] = link;
    runtime.r[15] = vector;
    if (charge) { runtime.cycles += 3; }
    runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
    external_entry = false;
    return true;
  }

  bool internal_event() {
    const auto signal = static_cast<detail::Signal>(runtime.signal);
    if (signal == detail::Signal::status || signal == detail::Signal::exception_return ||
        signal == detail::Signal::exchange) {
      if (signal == detail::Signal::exchange) {
        runtime.cpsr = runtime.address;
      } else {
        change_status(runtime.address);
      }
      const auto mask = (runtime.cpsr & 0x20) != 0 ? ~std::uint32_t{1} : ~std::uint32_t{3};
      runtime.r[15] &= mask;
      // A state-changing branch validates its destination even at a run bound.
      // Exception returns may resume the second half of a Thumb BL pair.
      runtime.signal = static_cast<std::uint32_t>(
          signal == detail::Signal::exchange ? detail::Signal::exchange : detail::Signal::none);
      return true;
    }
    switch (signal) {
      case detail::Signal::yielded_exchange: {
        runtime.cpsr = runtime.address;
        runtime.r[15] &= (runtime.cpsr & thumb_bit) != 0 ? ~std::uint32_t{1} : ~std::uint32_t{3};
        // Validate the new state/target exactly as an ordinary interworking
        // branch does at a bound, without executing a destination instruction.
        const auto entry = mode_entries[runtime.cpsr & (mode_bits | thumb_bit)];
        if (entry == nullptr) {
          runtime.signal = static_cast<std::uint32_t>(detail::Signal::unsupported);
          runtime.address = runtime.r[15];
          return false;
        }
        const auto instruction_bound = runtime.instruction_bound;
        const auto cycle_bound = runtime.cycle_bound;
        runtime.instruction_bound = runtime.instructions;
        runtime.cycle_bound = runtime.cycles;
        runtime.signal = static_cast<std::uint32_t>(detail::Signal::exchange);
        entry(&runtime);
        runtime.instruction_bound = instruction_bound;
        runtime.cycle_bound = cycle_bound;
        if (runtime.signal == static_cast<std::uint32_t>(detail::Signal::none)) {
          runtime.signal = static_cast<std::uint32_t>(detail::Signal::yielded);
        }
        return false;
      }
      case detail::Signal::yielded_exception_return:
        change_status(runtime.address);
        runtime.r[15] &= (runtime.cpsr & thumb_bit) != 0 ? ~std::uint32_t{1} : ~std::uint32_t{3};
        runtime.signal = static_cast<std::uint32_t>(detail::Signal::yielded);
        return false;
      case detail::Signal::mapping_change:
        if (!detail::same_mapping(runtime.system.mapping,
                                  binding->image->variants[variant_index].mapping)) {
          revoked = true;
          revocation_address = runtime.address;
          return false;
        }
        runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
        return true;
      case detail::Signal::redispatch:
        reconcile_view();
        runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
        return true;
      case detail::Signal::software_interrupt:
        return exception(
            {.vector = 8, .mode = detail::supervisor_mode}, runtime.r[15], false, false);
      case detail::Signal::undefined:
        return exception(
            {.vector = 4, .mode = detail::undefined_mode}, runtime.r[15], false, false);
      case detail::Signal::prefetch_abort:
        return exception(
            {.vector = 12, .mode = detail::abort_mode}, runtime.address + 4, false, false);
      case detail::Signal::mmu_prefetch_abort:
        return exception(
            {.vector = 12, .mode = detail::abort_mode}, runtime.r[15] + 4, false, true);
      case detail::Signal::mmu_change:
        if ((runtime.system.mapping.control & 1) != 0) {
          revoked = true;
          revocation_address = runtime.address;
          runtime.signal = static_cast<std::uint32_t>(detail::Signal::mapping_change);
          return false;
        }
        runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
        return true;
      case detail::Signal::alignment_abort:
        runtime.system.data_fault_status = 1;
        runtime.system.fault_address =
            detail::Mmu::modified(runtime.system.mapping, runtime.address);
        [[fallthrough]];
      case detail::Signal::data_abort:
        return exception(
            {.vector = 16, .mode = detail::abort_mode}, runtime.r[15] + 8, false, true);
      default: return false;
    }
  }

  [[nodiscard]] Backing shared_backing(const Memory::Region &region) const {
    if (region.mapping_fd < -1 || (region.mapping_fd < 0 && region.mapping_offset != 0)) {
      throw std::invalid_argument("invalid shared memory backing declaration");
    }
    Backing backing{.device = 0, .inode = 0, .descriptor = region.mapping_fd};
    if (region.mapping_fd >= 0) {
      struct stat status{};
      if (fstat(region.mapping_fd, &status) != 0 || status.st_size < 0 ||
          region.mapping_offset > static_cast<std::uint64_t>(status.st_size) ||
          region.bytes.size() >
              static_cast<std::uint64_t>(status.st_size) - region.mapping_offset) {
        throw std::invalid_argument("shared memory backing does not cover lent bytes");
      }
      backing.device = status.st_dev;
      backing.inode = status.st_ino;
      for (const auto &prior : backings) {
        if (prior.descriptor >= 0 && prior.device == backing.device &&
            prior.inode == backing.inode) {
          backing.descriptor = prior.descriptor;
          break;
        }
      }
    }
    return backing;
  }

  void rebind(Memory memory) {
    stopped();
    binding.reset();
    direct.clear();
    code_ranges.clear();
    code_bindings.clear();
    code_backings.clear();
    runtime.direct = nullptr;
    view.clear();
    view_ready = false;
    backings.clear();
    regions.assign(memory.regions.begin(), memory.regions.end());
    std::ranges::sort(regions, [](const Memory::Region &first, const Memory::Region &second) {
      return std::pair{first.space, first.address} < std::pair{second.space, second.address};
    });
    std::uint64_t end = 0;
    auto space = Memory::Space::bus;
    runtime.tcm_status = 0;
    constexpr std::uint32_t tcm_address_enable = 0xfffff001;
    runtime.system.mapping.instruction_tcm &= tcm_address_enable;
    runtime.system.mapping.data_tcm &= tcm_address_enable;
    for (const auto &region : regions) {
      if (space != region.space) { end = 0; }
      space = region.space;
      if (static_cast<unsigned>(space) > static_cast<unsigned>(Memory::Space::dtcm) ||
          region.bytes.empty() || region.bytes.size() > (std::uint64_t{1} << 32) - region.address ||
          region.address < end) {
        throw std::invalid_argument("invalid/overlapping guest memory regions");
      }
      if (space != Memory::Space::bus) {
        constexpr std::size_t minimum_tcm = 4096;
        constexpr std::size_t maximum_tcm = 1U << 20;
        if (region.address != 0 || end != 0 || !std::has_single_bit(region.bytes.size()) ||
            region.bytes.size() < minimum_tcm || region.bytes.size() > maximum_tcm) {
          throw std::invalid_argument("TCM needs one zero-based power-of-two region of 4KB to 1MB");
        }
        const auto size = static_cast<std::uint32_t>(std::countr_zero(region.bytes.size()) - 9)
                          << 2;
        if (space == Memory::Space::itcm) {
          runtime.system.mapping.instruction_tcm |= size;
          runtime.tcm_status |= 1;
        } else {
          runtime.system.mapping.data_tcm |= size;
          runtime.tcm_status |= 0x10000;
        }
      }
      backings.push_back(shared_backing(region));
      end = region.address + region.bytes.size();
    }
    generation = memory.generation;
  }

  [[nodiscard]] const Memory::Region *
  region(std::uint32_t address, unsigned width, Memory::Space space = Memory::Space::bus) const {
    for (const auto &candidate : regions) {
      if (candidate.space == space && address >= candidate.address &&
          std::uint64_t(address - candidate.address) + width <= candidate.bytes.size()) {
        return &candidate;
      }
    }
    return nullptr;
  }

  struct StorageAddress {
    Memory::Space space;
    std::uint32_t relative;
    std::uint64_t available;
  };

  [[nodiscard]] StorageAddress storage_address(std::uint32_t address, bool fetch) const {
    StorageAddress result{.space = Memory::Space::bus,
                          .relative = address,
                          .available = (std::uint64_t{1} << 32) - address};
    for (const auto space : {Memory::Space::itcm, Memory::Space::dtcm}) {
      if (fetch && space == Memory::Space::dtcm) { continue; }
      const auto range = detail::tcm_range(detail::tcm_register(runtime.system.mapping, space));
      if (range.contains(address)) {
        return {.space = space,
                .relative = address - range.base,
                .available = range.size - (address - range.base)};
      }
      if (range.size != 0 && address < range.base) {
        result.available = std::min(result.available, std::uint64_t(range.base - address));
      }
    }
    return result;
  }

  [[nodiscard]] bool code_overlap(std::span<const std::byte> bytes) const {
    const HostRange access{.address = reinterpret_cast<std::uintptr_t>(bytes.data()),
                           .size = bytes.size()};
    if (std::ranges::any_of(code_ranges,
                            [access](HostRange code) { return overlaps(access, code); })) {
      return true;
    }
    for (std::size_t index = 0; index < regions.size(); ++index) {
      const auto &mapping = regions[index];
      if (mapping.mapping_fd < 0) { continue; }
      const HostRange storage{.address = reinterpret_cast<std::uintptr_t>(mapping.bytes.data()),
                              .size = mapping.bytes.size()};
      if (!overlaps(access, storage)) { continue; }
      const auto first =
          mapping.mapping_offset + std::max(access.address, storage.address) - storage.address;
      const auto end = mapping.mapping_offset +
                       std::min(access.address + access.size, storage.address + storage.size) -
                       storage.address;
      if (std::ranges::any_of(code_backings, [&](const BackingRange &code) {
            return code.descriptor == backings[index].descriptor && first < code.end &&
                   code.first < end;
          })) {
        return true;
      }
    }
    return false;
  }

  struct CallbackAccess {
    std::uint32_t address;
    unsigned width, operation;
    std::uint32_t value;
    std::uintptr_t replacement;
  };

  std::uint64_t callback(CallbackAccess request) {
    const auto address = request.address;
    const auto width = request.width;
    const auto operation = request.operation;
    auto value = request.value;
    const auto replacement = request.replacement;
    bool accepted = false;
    bool present = false;
    if (operation == 0) {
      const auto callback =
          replacement == 0 ? io.read : std::bit_cast<decltype(io.read)>(replacement);
      present = callback != nullptr;
      accepted = present && callback(io.owner, address, width, value);
    } else if (operation == 1) {
      const auto callback =
          replacement == 0 ? io.write : std::bit_cast<decltype(io.write)>(replacement);
      present = callback != nullptr;
      accepted = present && callback(io.owner, address, width, value);
    } else {
      const auto callback =
          replacement == 0 ? io.swap : std::bit_cast<decltype(io.swap)>(replacement);
      present = callback != nullptr;
      accepted = present && callback(io.owner, address, width, value);
    }
    if (accepted) { return value; }
    runtime.signal = static_cast<std::uint32_t>(present ? detail::Signal::memory
                                                        : detail::Signal::external_abort);
    runtime.address = address;
    return access_failure;
  }

  std::uint64_t transaction(CallbackAccess request) {
    const auto [address, width, operation, value, replacement] = request;
    const auto storage = storage_address(address, false);
    const auto *region = this->region(storage.relative, width, storage.space);
    if (region == nullptr) {
      return callback({.address = address,
                       .width = width,
                       .operation = operation,
                       .value = value,
                       .replacement = replacement});
    }
    if ((operation != 1 && !region->readable) || (operation != 0 && !region->writable)) {
      // ARM926 DDI 0198E section 3.6: an externally aborted SWP read still
      // attempts the write. Keep its code-alias effects before latching Abort.
      if (operation == 2 && region->writable) {
        request.operation = 1;
        (void)transaction(request);
      }
      runtime.signal = static_cast<std::uint32_t>(detail::Signal::external_abort);
      runtime.address = address;
      return access_failure;
    }
    auto bytes = region->bytes.subspan(storage.relative - region->address, width);
    const bool big_endian = (binding->image->variants[variant_index].mapping.control & 0x80) != 0;
    const bool aliases_code = operation != 0 && code_overlap(bytes);
    const auto old = operation != 1 || aliases_code ? read_bytes(bytes, big_endian) : 0;
    if (operation != 0) {
      write_bytes(bytes, value, big_endian);
      const auto mask =
          width == 4 ? std::numeric_limits<std::uint32_t>::max() : (1U << (width * 8)) - 1;
      if (aliases_code && old != (value & mask)) {
        if (!revoked) { revocation_address = address; }
        revoked = true;
        runtime.signal = static_cast<std::uint32_t>(detail::Signal::changed);
        runtime.address = address;
      }
    }
    return operation == 0 || operation == 2 ? old : 0;
  }

  static std::uint64_t access(detail::Runtime *runtime,
                              std::uint32_t address,
                              unsigned width,
                              unsigned operation,
                              std::uint32_t value,
                              std::uintptr_t replacement) noexcept {
    const detail::Escape::Suspend suspension;
    auto &owner = *static_cast<Impl *>(runtime->owner);
    return owner.transaction({.address = address,
                              .width = width,
                              .operation = operation,
                              .value = value,
                              .replacement = replacement});
  }

  struct TableReader {
    Impl *owner;
    std::uintptr_t replacement;
    bool callbacks;

    static detail::Mmu::ReadResult read(void *context, std::uint32_t address) {
      const auto &reader = *static_cast<TableReader *>(context);
      auto &owner = *reader.owner;
      const auto *region = owner.region(address, 4);
      if (region != nullptr) {
        if (!region->readable) {
          return {.status = detail::Mmu::ReadStatus::external_abort, .value = 0};
        }
        return {.status = detail::Mmu::ReadStatus::ready,
                .value = read_bytes(region->bytes.subspan(address - region->address, 4),
                                    (owner.runtime.system.mapping.control & 0x80) != 0)};
      }
      const auto callback = reader.replacement == 0
                                ? owner.io.read
                                : std::bit_cast<decltype(owner.io.read)>(reader.replacement);
      if (callback == nullptr) {
        return {.status = detail::Mmu::ReadStatus::external_abort, .value = 0};
      }
      if (!reader.callbacks) { return {.status = detail::Mmu::ReadStatus::callback, .value = 0}; }
      std::uint32_t value = 0;
      if (!callback(owner.io.owner, address, 4, value)) {
        return {.status = detail::Mmu::ReadStatus::host_fault, .value = 0};
      }
      return {.status = detail::Mmu::ReadStatus::ready, .value = value};
    }
  };

  bool translation_failure(const detail::Mmu::Translation &translated,
                           bool instruction,
                           std::uint32_t address) {
    using Status = detail::Mmu::Status;
    const auto status = translated.status;
    if (status == Status::ready) { return false; }
    auto signal = detail::Signal::unsupported;
    runtime.address = address;
    if (status == Status::callback) { signal = detail::Signal::mmu_callback; }
    if (status == Status::host_fault) {
      signal = detail::Signal::memory;
      runtime.address = translated.fault_address;
    }
    if (status == Status::fault) {
      if (instruction) {
        runtime.system.instruction_fault_status = translated.fault_status;
        signal = detail::Signal::mmu_prefetch_abort;
      } else {
        runtime.system.data_fault_status = translated.fault_status;
        runtime.system.fault_address = translated.mva;
        signal = detail::Signal::data_abort;
      }
    }
    runtime.signal = static_cast<std::uint32_t>(signal);
    return true;
  }

  static void invalidate_code(detail::Runtime *runtime,
                              std::uint32_t operand,
                              detail::Runtime::CodeInvalidation operation,
                              std::uint32_t pc) noexcept {
    const detail::Escape::Suspend suspension;
    auto &owner = *static_cast<Impl *>(runtime->owner);
    constexpr std::uint32_t line_mask = 0xffffffe0;
    constexpr std::uint32_t set_mask = 0xfe0; // c0: 16KB ICache, four ways, 32-byte lines.
    const auto line = operand & line_mask;
    const bool affected =
        operation == detail::Runtime::CodeInvalidation::all ||
        std::ranges::any_of(owner.code_bindings, [&](CodeBinding code) {
          const auto mva = detail::Mmu::modified(runtime->system.mapping, code.address);
          const auto first = mva & line_mask;
          const auto last = (std::uint64_t(mva) + code.size - 1) & line_mask;
          if (operation == detail::Runtime::CodeInvalidation::line) {
            return line >= first && line <= last;
          }
          const auto offset = ((operand & set_mask) - (first & set_mask)) & set_mask;
          return std::uint64_t(first) + offset <= last;
        });
    if (!affected) { return; }
    owner.revoked = true;
    owner.revocation_address = pc;
    runtime->signal = static_cast<std::uint32_t>(detail::Signal::changed);
    runtime->address = pc;
  }

  static std::uint64_t virtual_access(detail::Runtime *runtime,
                                      std::uint32_t address,
                                      detail::Runtime::Width access_width,
                                      unsigned operation,
                                      std::uint32_t value,
                                      detail::Runtime::DataCallback data_callback,
                                      detail::Runtime::PageCallback page_callback,
                                      std::uint32_t first,
                                      unsigned count,
                                      detail::Runtime::Privilege privilege,
                                      detail::Runtime::Callbacks callbacks) noexcept {
    const detail::Escape::Suspend suspension;
    auto &owner = *static_cast<Impl *>(runtime->owner);
    const auto width = static_cast<unsigned>(access_width);
    const auto page_read = static_cast<std::uintptr_t>(page_callback);
    TableReader reader{.owner = &owner,
                       .replacement = page_read,
                       .callbacks = callbacks == detail::Runtime::Callbacks::invoke};
    const auto replacement = static_cast<std::uintptr_t>(data_callback);
    auto translated =
        owner.mmu.translate(runtime->system,
                            address,
                            {.privileged = privilege == detail::Runtime::Privilege::privileged,
                             .writing = operation != 0,
                             .commit = true},
                            &TableReader::read,
                            &reader);
    if (width == 4) { translated.mva |= first & 3; }
    if (owner.translation_failure(translated, false, address)) { return access_failure; }
    const auto storage = owner.storage_address(translated.physical, false);
    if (owner.region(storage.relative, width, storage.space) == nullptr &&
        callbacks == detail::Runtime::Callbacks::probe) {
      bool present = replacement != 0;
      switch (operation) {
        case 0: present |= owner.io.read != nullptr; break;
        case 1: present |= owner.io.write != nullptr; break;
        case 2: present |= owner.io.swap != nullptr; break;
        default: break;
      }
      if (present) {
        runtime->signal = static_cast<std::uint32_t>(detail::Signal::mmu_callback);
        return access_failure;
      }
    }
    const bool was_revoked = owner.revoked;
    const auto result = access(runtime, translated.physical, width, operation, value, replacement);
    if (!was_revoked && owner.revoked) {
      owner.revocation_address = address;
      runtime->address = address;
    }
    if ((result & access_failure) != 0 &&
        runtime->signal == static_cast<std::uint32_t>(detail::Signal::external_abort)) {
      if (operation == 1 && storage.space == Memory::Space::bus &&
          (translated.attributes & 1) != 0) {
        runtime->signal = static_cast<std::uint32_t>(detail::Signal::memory);
        return result;
      }
      const auto start = detail::Mmu::modified(runtime->system.mapping, first);
      const auto last = start + ((count - 1) * 4);
      constexpr std::uint32_t page_mask = 0xfffffc00;
      constexpr std::uint32_t last_word = 0x3fc;
      const auto first_end = (start & page_mask) == (last & page_mask) ? last : start | last_word;
      runtime->system.data_fault_status = 8;
      runtime->system.fault_address =
          (translated.mva & page_mask) == (start & page_mask) ? first_end : last;
      runtime->signal = static_cast<std::uint32_t>(detail::Signal::data_abort);
    }
    return result;
  }

  static std::uint32_t fetch(detail::Runtime *runtime,
                             std::uint32_t address,
                             detail::Runtime::Width access_width,
                             unsigned mode,
                             detail::Runtime::PageCallback page_callback,
                             detail::Runtime::Callbacks callbacks) noexcept {
    const detail::Escape::Suspend suspension;
    auto &owner = *static_cast<Impl *>(runtime->owner);
    const auto width = static_cast<unsigned>(access_width);
    const auto page_read = static_cast<std::uintptr_t>(page_callback);
    TableReader reader{.owner = &owner,
                       .replacement = page_read,
                       .callbacks = callbacks == detail::Runtime::Callbacks::invoke};
    auto translated = owner.mmu.translate(
        runtime->system,
        address,
        {.privileged = mode != detail::user_mode, .writing = false, .commit = true},
        &TableReader::read,
        &reader);
    if (translated.status == detail::Mmu::Status::ready) {
      const auto storage = owner.storage_address(translated.physical, true);
      const auto *region = owner.region(storage.relative, width, storage.space);
      if (region == nullptr || !region->readable || !region->executable) {
        translated.status = detail::Mmu::Status::fault;
        translated.fault_status = 8;
      }
    }
    if (owner.translation_failure(translated, true, address)) { return runtime->signal; }
    for (const auto &bound : owner.code_bindings) {
      if (address >= bound.address &&
          std::uint64_t(address - bound.address) + width <= bound.size) {
        if (translated.physical != bound.physical + (address - bound.address)) {
          owner.revoked = true;
          owner.revocation_address = address;
          runtime->signal = static_cast<std::uint32_t>(detail::Signal::changed);
          runtime->address = address;
          return runtime->signal;
        }
        return runtime->signal;
      }
    }
    runtime->signal = static_cast<std::uint32_t>(detail::Signal::unprepared);
    runtime->address = address;
    return runtime->signal;
  }

  static void
  invalidate(detail::Runtime *runtime, std::uint32_t address, unsigned single) noexcept {
    const detail::Escape::Suspend suspension;
    static_cast<Impl *>(runtime->owner)->mmu.invalidate(single != 0, address);
  }

  void validate_code(const detail::Variant &variant) {
    for (const auto &span : variant.spans) {
      std::size_t offset = 0;
      while (offset < span.bytes.size()) {
        const auto address = span.address + static_cast<std::uint32_t>(offset);
        auto physical = address;
        auto available = (std::uint64_t{1} << 32) - address;
        if ((variant.mapping.control & 1) != 0) {
          TableReader reader{.owner = this, .replacement = 0, .callbacks = false};
          auto system = runtime.system;
          const auto translated =
              mmu.translate(system,
                            address,
                            {.privileged = true, .writing = false, .commit = false, .check = false},
                            &TableReader::read,
                            &reader);
          if (translated.status != detail::Mmu::Status::ready) {
            detail::refuse(identity, address, "declared instruction mapping cannot be validated");
          }
          physical = translated.physical;
          available = translated.available;
        }
        const auto storage = storage_address(physical, true);
        const auto *mapping = region(storage.relative, 1, storage.space);
        if (mapping == nullptr || !mapping->readable || !mapping->executable) {
          detail::refuse(identity, address, "declared code is not readable executable memory");
        }
        const auto count =
            std::min({std::uint64_t(span.bytes.size() - offset),
                      std::uint64_t(mapping->bytes.size() - (storage.relative - mapping->address)),
                      storage.available,
                      available});
        const auto actual = mapping->bytes.subspan(storage.relative - mapping->address, count);
        if (!std::ranges::equal(actual, std::span(span.bytes).subspan(offset, count))) {
          detail::refuse(identity, address, "installed code bytes differ from declared variant");
        }
        code_bindings.push_back(
            {.address = address, .physical = physical, .size = static_cast<std::uint32_t>(count)});
        code_ranges.push_back(
            {.address = reinterpret_cast<std::uintptr_t>(actual.data()), .size = actual.size()});
        offset += count;
      }
    }
  }

  void validate_layout(const detail::Variant &variant) {
    for (const auto &layout : variant.layout) {
      const auto *mapping = region(layout.address, 1, layout.space);
      if (mapping == nullptr || mapping->address != layout.address ||
          mapping->bytes.size() != layout.size || mapping->readable != layout.readable ||
          mapping->writable != layout.writable || mapping->executable != layout.executable ||
          (layout.shared_mapping && mapping->mapping_fd < 0)) {
        detail::refuse(identity, layout.address, "memory layout differs from prepared declaration");
      }
      const auto base =
          layout.space == Memory::Space::bus
              ? 0U
              : detail::tcm_register(variant.mapping, layout.space) & detail::tcm_base_mask;
      direct.push_back(
          {.address_bias =
               reinterpret_cast<std::uintptr_t>(mapping->bytes.data()) - (base + layout.address),
           .safe_write = unsigned(mapping->writable && !code_overlap(mapping->bytes))});
    }
    runtime.direct = direct.data();
  }

  struct ViewPlan {
    Impl *owner;
    detail::Mmu fresh, cached;
    Registers::System system;
    bool privileged;

    static std::uint64_t retained_boundary(std::span<const detail::Mmu::Entry> entries,
                                           std::uint32_t mva,
                                           std::uint64_t available) {
      for (const auto &entry : entries) {
        if (entry.size == 0) { continue; }
        if (entry.virtual_base > mva) {
          available = std::min(available, std::uint64_t(entry.virtual_base - mva));
        } else if (mva - entry.virtual_base < entry.size) {
          available = std::min(available, std::uint64_t(entry.size - (mva - entry.virtual_base)));
        }
      }
      return available;
    }

    static bool same(const detail::Mmu::Translation &first,
                     const detail::Mmu::Translation &second) {
      return std::tie(
                 first.status, first.physical, first.fault_status, first.attributes, first.tiny) ==
             std::tie(second.status,
                      second.physical,
                      second.fault_status,
                      second.attributes,
                      second.tiny);
    }

    detail::View::Piece virtual_piece(std::uint32_t address) {
      std::uint32_t physical = address;
      auto available = detail::View::size - address;
      bool readable = true;
      bool writable = true;
      if ((system.mapping.control & 1) != 0) {
        TableReader reader{.owner = owner, .replacement = 0, .callbacks = false};
        available = 0;
        for (const bool writing : {false, true}) {
          const detail::Mmu::Access access{
              .privileged = privileged, .writing = writing, .commit = false};
          const auto current =
              fresh.translate(system, address, access, &TableReader::read, &reader);
          const auto retained =
              cached.translate(system, address, access, &TableReader::read, &reader);
          if (!same(current, retained) || current.tiny ||
              (current.status != detail::Mmu::Status::ready &&
               current.status != detail::Mmu::Status::fault)) {
            return {.exact = true};
          }
          const bool allowed = current.status == detail::Mmu::Status::ready;
          (writing ? writable : readable) = allowed;
          if (current.available != 0) {
            available = available == 0 ? current.available
                                       : std::min(available, std::uint64_t(current.available));
          }
          if (allowed) { physical = current.physical; }
        }
        if (available == 0) { available = 1024 - (address & (1024 - 1)); }
        const auto mva = detail::Mmu::modified(system.mapping, address);
        available = retained_boundary(cached.locked, mva, available);
        available = retained_boundary(cached.normal, mva, available);
      }
      return {.size = available, .offset = physical, .readable = readable, .writable = writable};
    }

    detail::View::Piece translated(std::uint32_t address) {
      const auto piece = virtual_piece(address);
      if (piece.exact || (!piece.readable && !piece.writable)) { return piece; }
      auto &core = *owner;
      const auto physical = static_cast<std::uint32_t>(piece.offset);
      auto available = piece.size;
      bool readable = piece.readable;
      bool writable = piece.writable;
      const auto storage = core.storage_address(physical, false);
      available = std::min(available, storage.available);
      const auto *region = core.region(storage.relative, 1, storage.space);
      if (region == nullptr) {
        for (const auto &next : core.regions) {
          if (next.space == storage.space && next.address > storage.relative) {
            available = std::min(available, std::uint64_t(next.address - storage.relative));
          }
        }
        return {.size = available};
      }
      const auto relative = storage.relative - region->address;
      available = std::min(available, region->bytes.size() - relative);
      readable &= region->readable;
      writable &= region->writable;
      if (!readable && !writable) { return {.size = available}; }
      const auto index = static_cast<std::size_t>(region - core.regions.data());
      const auto descriptor = core.backings[index].descriptor;
      if (descriptor < 0) { return {.exact = true}; }
      const auto offset = region->mapping_offset + relative;
      for (const auto &code : core.protected_backings) {
        if (code.descriptor != descriptor || offset >= code.end) { continue; }
        if (offset >= code.first) {
          writable = false;
          available = std::min(available, code.end - offset);
        } else {
          available = std::min(available, code.first - offset);
        }
      }
      return {.size = available,
              .offset = offset,
              .descriptor = descriptor,
              .readable = readable,
              .writable = writable};
    }

    static detail::View::Piece resolve(void *context, std::uint32_t address) {
      return static_cast<ViewPlan *>(context)->translated(address);
    }
  };

  void collect_code_backings() {
    code_backings.clear();
    for (std::size_t index = 0; index < regions.size(); ++index) {
      const auto &region = regions[index];
      if (region.mapping_fd < 0) { continue; }
      const HostRange storage{.address = reinterpret_cast<std::uintptr_t>(region.bytes.data()),
                              .size = region.bytes.size()};
      for (const auto code : code_ranges) {
        if (!overlaps(code, storage)) { continue; }
        const auto first =
            region.mapping_offset + std::max(code.address, storage.address) - storage.address;
        const auto end = region.mapping_offset +
                         std::min(code.address + code.size, storage.address + storage.size) -
                         storage.address;
        code_backings.push_back(
            {.descriptor = backings[index].descriptor, .first = first, .end = end});
      }
    }
  }

  void build_view(const detail::Image &image, const detail::Variant &variant) {
    view.clear();
    view_ready = false;
    protected_backings.clear();
    runtime.view_base = 0;
    if (image.options.memory_mode != CompileOptions::MemoryMode::direct ||
        (runtime.system.tlb_lockdown & 1) != 0 || io.read != nullptr || io.write != nullptr ||
        io.swap != nullptr || !image.options.callbacks.empty()) {
      return;
    }
    for (const auto &region : regions) {
      if (std::ranges::none_of(variant.layout, [&](const RegionLayout &layout) {
            return layout.address == region.address && layout.space == region.space &&
                   layout.shared_mapping;
          })) {
        return;
      }
    }
    const auto page = detail::View::page_size();
    for (const auto &code : code_backings) {
      protected_backings.push_back({.descriptor = code.descriptor,
                                    .first = code.first / page * page,
                                    .end = (code.end + page - 1) / page * page});
    }
    view_privileged = (runtime.cpsr & mode_bits) != detail::user_mode;
    ViewPlan plan{.owner = this,
                  .fresh = {},
                  .cached = mmu,
                  .system = runtime.system,
                  .privileged = view_privileged};
    view_ready = view.build(&ViewPlan::resolve, &plan);
    for (const auto &span : variant.spans) {
      if (!view_ready) { break; }
      for (std::size_t offset = 0; offset < span.bytes.size();) {
        const auto piece = plan.translated(span.address + static_cast<std::uint32_t>(offset));
        if (piece.exact || !piece.readable) {
          view_ready = false;
          break;
        }
        offset += std::min(piece.size, span.bytes.size() - offset);
      }
    }
    if (!view_ready) { view.clear(); }
    runtime.view_base = view.base();
  }

  void select_entries() {
    mode_entries.fill(nullptr);
    const auto &units = binding->image->units;
    for (std::size_t index = 0; index < units.size(); ++index) {
      const bool selected_direct =
          view_ready && ((units[index].mode != detail::user_mode) == view_privileged);
      if (units[index].variant == variant_index && units[index].direct == selected_direct) {
        mode_entries[units[index].mode | (units[index].thumb ? thumb_bit : 0)] =
            binding->entries[index];
      }
    }
  }

  void reconcile_view() {
    if (binding->image->options.memory_mode != CompileOptions::MemoryMode::direct) { return; }
    if ((runtime.system.tlb_lockdown & 1) != 0) {
      view.clear();
      view_ready = false;
      runtime.view_base = 0;
    } else if (!view_ready) {
      build_view(*binding->image, binding->image->variants[variant_index]);
    }
    select_entries();
  }

  [[nodiscard]] State inspect() const {
    auto result = continuation;
    result.registers = {
        .r = runtime.r, .cpsr = runtime.cpsr, .spsr = runtime.spsr, .system = runtime.system};
    result.banks = runtime.banks;
    save_bank(result.banks);
    result.progress = {.instructions = runtime.instructions, .base_cycles = runtime.cycles};
    return result;
  }

  void failure(Fault::Kind kind, std::uint32_t address, std::string reason) {
    continuation.execution = ExecutionState::fault;
    continuation.fault = {
        .kind = kind, .address = address, .identity = identity, .reason = std::move(reason)};
  }

  [[nodiscard]] bool transition_matches(const CodeTransition &transition) const {
    if (transition.source_variant != binding->image->variants[variant_index].id ||
        transition.address != runtime.r[15]) {
      return false;
    }
    const auto destination = std::ranges::find(
        binding->image->variants, transition.destination_variant, &detail::Variant::id);
    return destination != binding->image->variants.end() &&
           detail::same_mapping(runtime.system.mapping, destination->mapping);
  }

  void finish() {
    if (continuation.execution == ExecutionState::fault) { return; }
    if (revoked && runtime.signal != static_cast<std::uint32_t>(detail::Signal::memory) &&
        runtime.signal != static_cast<std::uint32_t>(detail::Signal::mapping_change)) {
      runtime.signal = static_cast<std::uint32_t>(detail::Signal::changed);
      runtime.address = revocation_address;
    }
    if (runtime.signal == static_cast<std::uint32_t>(detail::Signal::none)) { return; }
    if (runtime.signal == static_cast<std::uint32_t>(detail::Signal::wait)) {
      continuation.execution = ExecutionState::stopped;
      return;
    }
    if (runtime.signal == static_cast<std::uint32_t>(detail::Signal::yielded)) {
      continuation.execution = ExecutionState::yielded;
      return;
    }
    if (runtime.signal == static_cast<std::uint32_t>(detail::Signal::mapping_change) ||
        runtime.signal == static_cast<std::uint32_t>(detail::Signal::changed) ||
        runtime.signal == static_cast<std::uint32_t>(detail::Signal::transfer)) {
      for (const auto &transition : binding->image->transitions) {
        if (transition_matches(transition)) {
          continuation.execution = ExecutionState::transfer;
          continuation.transition_pending = true;
          continuation.transition = transition;
          pending_image = binding->image->key;
          return;
        }
      }
    }
    switch (static_cast<detail::Signal>(runtime.signal)) {
      case detail::Signal::mapping_change:
        failure(Fault::Kind::changed_code,
                runtime.address,
                "CP15 mapping changed the prepared memory context");
        break;
      case detail::Signal::changed:
        failure(Fault::Kind::changed_code,
                runtime.address,
                "prepared code invalidated or executable bytes changed through a guest mapping or "
                "alias");
        break;
      case detail::Signal::memory:
        failure(
            Fault::Kind::memory, runtime.address, "memory access or synchronous callback refused");
        break;
      case detail::Signal::unsupported:
        failure(Fault::Kind::unsupported,
                runtime.address,
                "unsupported execution-state transition or unpredictable access");
        break;
      default:
        failure(Fault::Kind::unprepared, runtime.address, "undeclared instruction target");
        break;
    }
  }

  bool invoke(detail::Entry entry) {
    const bool direct_mode =
        view_ready && ((runtime.cpsr & mode_bits) != detail::user_mode) == view_privileged;
    if (direct_mode) {
      if (!escape.invoke(entry, runtime, binding->text_ranges, runtime.view_base)) {
        const auto fault = escape.fault();
        poisoned = true;
        const auto site =
            std::ranges::find_if(binding->fault_sites, [&](const detail::FaultSite &candidate) {
              return candidate.variant == binding->image->variants[variant_index].id &&
                     fault.native_pc - candidate.address < candidate.size;
            });
        std::ostringstream reason;
        reason << "fatal Direct " << (fault.writing ? "write" : "read");
        if (site == binding->fault_sites.end()) {
          reason << " at native PC 0x" << std::hex << fault.native_pc << " (guest PC unavailable)";
        } else {
          reason << " at guest PC 0x" << std::hex << site->guest_pc;
        }
        reason << "; architectural state and progress unavailable; discard Core";
        failure(Fault::Kind::memory, fault.address, reason.str());
        if (site != binding->fault_sites.end()) { continuation.fault.variant = site->variant; }
        return false;
      }
    } else {
      entry(&runtime);
    }
    return true;
  }

  [[nodiscard]] detail::Entry current_entry() const {
    return (runtime.cpsr & jazelle_bit) == 0 ? mode_entries[runtime.cpsr & (mode_bits | thumb_bit)]
                                             : nullptr;
  }

  RunResult execute(std::uint64_t instructions, std::uint64_t deadline) {
    stopped();
    const Progress before{.instructions = runtime.instructions, .base_cycles = runtime.cycles};
    if (instructions == 0 || deadline <= runtime.cycles ||
        continuation.execution == ExecutionState::fault ||
        continuation.execution == ExecutionState::stopped) {
      return {.execution = continuation.execution, .progress = {}, .fault = continuation.fault};
    }
    if (continuation.transition_pending) {
      if (!binding || binding->image->key != pending_image ||
          binding->image->variants[variant_index].id !=
              continuation.transition.destination_variant) {
        return {.execution = ExecutionState::transfer, .progress = {}, .fault = {}};
      }
      continuation.transition_pending = false;
    }
    continuation.execution = ExecutionState::ready;
    if (!binding) {
      failure(Fault::Kind::unprepared,
              runtime.r[15],
              "no installed native image for memory generation " + std::to_string(generation));
    } else if ((runtime.cpsr & jazelle_bit) != 0 ||
               mode_entries[runtime.cpsr & (mode_bits | thumb_bit)] == nullptr) {
      failure(Fault::Kind::unsupported,
              runtime.r[15],
              "processor mode/state differs from prepared unit");
    } else {
      const auto &entries = binding->image->variants[variant_index].entries;
      if (external_entry && std::ranges::find(entries, runtime.r[15]) == entries.end()) {
        failure(Fault::Kind::unprepared, runtime.r[15], "entry was not declared");
      }
    }
    if (continuation.execution == ExecutionState::fault) {
      return {.execution = continuation.execution, .progress = {}, .fault = continuation.fault};
    }
    runtime.instruction_bound =
        runtime.instructions +
        std::min(instructions, std::numeric_limits<std::uint64_t>::max() - runtime.instructions);
    runtime.cycle_bound = deadline;
    runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
    revoked = false;
    running = true;
    do {
      const auto entry = current_entry();
      if (entry == nullptr) {
        runtime.signal = static_cast<std::uint32_t>(detail::Signal::unsupported);
        runtime.address = runtime.r[15];
        break;
      }
      if (!invoke(entry)) { break; }
      const bool resume = internal_event();
      if (revoked || !resume) { break; }
    } while (
        runtime.signal == static_cast<std::uint32_t>(detail::Signal::exchange) ||
        (runtime.instructions < runtime.instruction_bound && runtime.cycles < runtime.cycle_bound));
    running = false;
    external_entry = false;
    finish();
    if (revoked) {
      binding.reset();
      runtime.direct = nullptr;
    }
    return {.execution = continuation.execution,
            .progress = {.instructions = runtime.instructions - before.instructions,
                         .base_cycles = runtime.cycles - before.base_cycles},
            .fault = continuation.fault};
  }
};

Core::Core(Memory memory, Io io) : impl_(std::make_unique<Impl>(memory, io)) {}

Core::~Core() = default;

bool Core::host_fault(std::uintptr_t address, bool writing, std::uintptr_t native_pc) noexcept {
  return detail::Escape::host_fault(address, writing, native_pc);
}

std::string_view Core::compiler_identity() noexcept {
  return detail::compiler_identity;
}

SemanticImage Core::lower(const CodeImage &image, const CompileOptions &options) {
  auto result = std::make_shared<SemanticImage::Impl>();
  static_cast<detail::Image &>(*result) = detail::describe(image, options);
  detail::lower_llvm(*result);
  detail::pack(*result, image);
  return SemanticImage(std::move(result));
}

SemanticImage Core::load(const CodeImage &image,
                         const CompileOptions &options,
                         std::span<const std::byte> artifact) {
  auto result = std::make_shared<SemanticImage::Impl>();
  static_cast<detail::Image &>(*result) = detail::unpack(image, options, artifact);
  return SemanticImage(std::move(result));
}

NativeImage Core::bind(const SemanticImage &image, const NativeSymbols &symbols) {
  auto result = std::make_shared<NativeImage::Impl>();
  static_cast<detail::Native &>(*result) = detail::native_bind(image.impl_, symbols);
  return NativeImage(std::move(result));
}

NativeImage Core::compile(const CodeImage &image, const CompileOptions &options) {
  const auto start = std::chrono::steady_clock::now();
  auto semantic = lower(image, options);
  auto result = std::make_shared<NativeImage::Impl>();
  try {
    static_cast<detail::Native &>(*result) = detail::native_compile(semantic.impl_);
  } catch (const std::runtime_error &error) { detail::refuse(image.identity, 0, error.what()); }
  result->report.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return NativeImage(std::move(result));
}

PreparationReport Core::install(const NativeImage &image, std::uint32_t variant) {
  impl_->stopped();
  impl_->binding.reset();
  impl_->direct.clear();
  impl_->code_ranges.clear();
  impl_->code_bindings.clear();
  impl_->code_backings.clear();
  impl_->runtime.direct = nullptr;
  impl_->identity = image.impl_->image->identity;
  const auto &variants = image.impl_->image->variants;
  const auto found = std::ranges::find(variants, variant, &detail::Variant::id);
  if (found == variants.end()) {
    detail::refuse(impl_->identity, impl_->runtime.r[15], "unknown installed variant");
  }
  if (!detail::same_mapping(impl_->runtime.system.mapping, found->mapping)) {
    detail::refuse(
        impl_->identity, impl_->runtime.r[15], "CP15 mapping differs from prepared variant");
  }
  if (!detail::valid_tcms(impl_->runtime.system.mapping) || !detail::valid_tcms(found->mapping)) {
    detail::refuse(
        impl_->identity, impl_->runtime.r[15], "unpredictable TCM geometry/base/overlap");
  }
  constexpr std::uint32_t table_reserved = 0x3fff;
  constexpr std::uint32_t process_reserved = 0x01ffffff;
  if ((found->mapping.translation_table & table_reserved) != 0 ||
      (found->mapping.process_id & process_reserved) != 0) {
    detail::refuse(
        impl_->identity, impl_->runtime.r[15], "reserved CP15 translation/PID mapping bits");
  }
  constexpr std::uint32_t implemented_control = 0xf387; // M, A, C, B, S, R, I, V, RR and L4.
  if ((found->mapping.control & ~implemented_control) !=
      Registers::System::Mapping::reset_control) {
    detail::refuse(impl_->identity,
                   impl_->runtime.r[15],
                   "CP15 memory-control behavior is not implemented for this configuration yet");
  }
  impl_->validate_code(*found);
  impl_->collect_code_backings();
  impl_->validate_layout(*found);
  impl_->build_view(*image.impl_->image, *found);
  impl_->binding = image.impl_;
  impl_->variant_index = static_cast<std::size_t>(found - variants.begin());
  impl_->select_entries();
  return image.report();
}

Registers Core::registers() const {
  if (impl_->poisoned) {
    throw std::logic_error("register state unavailable after fatal Direct fault");
  }
  return {.r = impl_->runtime.r,
          .cpsr = impl_->runtime.cpsr,
          .spsr = impl_->runtime.spsr,
          .system = impl_->runtime.system};
}

void Core::set_registers(const Registers &registers) {
  impl_->stopped();
  if (!detail::valid_mode(registers.cpsr & mode_bits)) {
    throw std::invalid_argument("invalid processor mode");
  }
  impl_->save_bank(impl_->runtime.banks);
  impl_->runtime.r = registers.r;
  impl_->runtime.cpsr = registers.cpsr & ~jazelle_bit;
  impl_->runtime.spsr = registers.spsr & ~jazelle_bit;
  constexpr std::uint32_t tcm_size_bits = 0x3c;
  auto system = registers.system;
  system.mapping.instruction_tcm = (system.mapping.instruction_tcm & ~tcm_size_bits) |
                                   (impl_->runtime.system.mapping.instruction_tcm & tcm_size_bits);
  system.mapping.data_tcm = (system.mapping.data_tcm & ~tcm_size_bits) |
                            (impl_->runtime.system.mapping.data_tcm & tcm_size_bits);
  if (!detail::same_mapping(impl_->runtime.system.mapping, system.mapping)) {
    impl_->binding.reset();
    impl_->runtime.direct = nullptr;
  }
  impl_->runtime.system = system;
  if (impl_->binding) { impl_->reconcile_view(); }
  impl_->external_entry = true;
}

State Core::state() const {
  if (impl_->poisoned) {
    throw std::logic_error("processor state unavailable after fatal Direct fault");
  }
  return impl_->inspect();
}

Snapshot Core::save() const {
  impl_->stopped();
  return Snapshot(std::make_shared<Snapshot::Impl>(
      impl_->inspect(), impl_->external_entry, impl_->pending_image, impl_->mmu));
}

void Core::restore(const Snapshot &snapshot) {
  impl_->stopped();
  constexpr std::uint32_t tcm_size_bits = 0x3c;
  const auto &saved_mapping = snapshot.impl_->state.registers.system.mapping;
  const auto &current_mapping = impl_->runtime.system.mapping;
  if ((((saved_mapping.instruction_tcm ^ current_mapping.instruction_tcm) |
        (saved_mapping.data_tcm ^ current_mapping.data_tcm)) &
       tcm_size_bits) != 0) {
    throw std::invalid_argument("snapshot TCM geometry differs from the current memory profile");
  }
  impl_->continuation = snapshot.impl_->state;
  impl_->runtime.r = snapshot.impl_->state.registers.r;
  impl_->runtime.cpsr = snapshot.impl_->state.registers.cpsr;
  impl_->runtime.spsr = snapshot.impl_->state.registers.spsr;
  impl_->runtime.system = snapshot.impl_->state.registers.system;
  if (impl_->binding &&
      !detail::same_mapping(impl_->runtime.system.mapping,
                            impl_->binding->image->variants[impl_->variant_index].mapping)) {
    impl_->binding.reset();
    impl_->runtime.direct = nullptr;
  }
  impl_->runtime.banks = snapshot.impl_->state.banks;
  impl_->mmu = snapshot.impl_->mmu;
  impl_->external_entry = snapshot.impl_->external_entry;
  impl_->pending_image = snapshot.impl_->pending_image;
  impl_->runtime.instructions = snapshot.impl_->state.progress.instructions;
  impl_->runtime.cycles = snapshot.impl_->state.progress.base_cycles;
  impl_->runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
  if (impl_->binding &&
      impl_->binding->image->options.memory_mode == CompileOptions::MemoryMode::direct) {
    const auto variant = impl_->binding->image->variants[impl_->variant_index].id;
    const NativeImage native(std::static_pointer_cast<const NativeImage::Impl>(impl_->binding));
    (void)install(native, variant);
  }
}

void Core::set_memory(Memory memory) {
  impl_->rebind(memory);
}

RunResult Core::run(std::uint64_t instruction_budget) {
  return impl_->execute(instruction_budget, std::numeric_limits<std::uint64_t>::max());
}

RunResult Core::run_until(std::uint64_t base_cycle_deadline) {
  return impl_->execute(std::numeric_limits<std::uint64_t>::max(), base_cycle_deadline);
}

void Core::request_yield() {
  if (!impl_->running) {
    throw std::logic_error("request_yield requires a synchronous execution callback");
  }
  impl_->runtime.signal = static_cast<std::uint32_t>(detail::Signal::yielded);
}

bool Core::interrupt_ready(InterruptRequest request) const {
  impl_->stopped();
  const auto mask = request.kind == InterruptRequest::Kind::fiq ? 0x40U : 0x80U;
  return (impl_->runtime.cpsr & mask) == 0 && !impl_->continuation.transition_pending &&
         impl_->continuation.execution != ExecutionState::fault;
}

void Core::interrupt(InterruptRequest request) {
  impl_->stopped();
  if (!interrupt_ready(request)) { return; }
  resume_from_stop();
  constexpr std::uint32_t irq_vector = 24;
  constexpr std::uint32_t fiq_vector = 28;
  const bool fast = request.kind == InterruptRequest::Kind::fiq;
  if (impl_->exception({.vector = fast ? fiq_vector : irq_vector,
                        .mode = fast ? detail::fiq_mode : detail::irq_mode},
                       impl_->runtime.r[15] + 4,
                       fast,
                       true)) {
    impl_->continuation.execution = ExecutionState::ready;
  }
}

void Core::reset(std::uint32_t vector) {
  impl_->stopped();
  if ((vector & 3) != 0) { throw std::invalid_argument("reset vector must be word aligned"); }
  impl_->continuation = {};
  impl_->pending_image.clear();
  impl_->runtime.r = {};
  impl_->runtime.r[15] = vector;
  impl_->runtime.cpsr = Registers::reset_cpsr;
  impl_->runtime.spsr = 0;
  constexpr std::uint32_t tcm_size_bits = 0x3c;
  const auto instruction_tcm = impl_->runtime.system.mapping.instruction_tcm & tcm_size_bits;
  const auto data_tcm = impl_->runtime.system.mapping.data_tcm & tcm_size_bits;
  impl_->runtime.system = {};
  impl_->mmu = {};
  impl_->runtime.system.mapping.instruction_tcm = instruction_tcm;
  impl_->runtime.system.mapping.data_tcm = data_tcm;
  if (impl_->binding &&
      !detail::same_mapping(impl_->runtime.system.mapping,
                            impl_->binding->image->variants[impl_->variant_index].mapping)) {
    impl_->binding.reset();
    impl_->runtime.direct = nullptr;
  }
  impl_->runtime.banks = {};
  impl_->runtime.instructions = 0;
  impl_->runtime.cycles = 0;
  impl_->runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
  impl_->external_entry = true;
}

void Core::resume_from_stop() {
  impl_->stopped();
  if (impl_->continuation.execution != ExecutionState::stopped) { return; }
  impl_->runtime.r[15] += 4;
  ++impl_->runtime.instructions;
  ++impl_->runtime.cycles;
  impl_->runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
  impl_->continuation.execution = ExecutionState::ready;
}
} // namespace armv5
