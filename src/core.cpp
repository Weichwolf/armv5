// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"

#include "armv5/memory.hpp"
#include "identity.hpp"
#include "image.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
};

Snapshot::Snapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

namespace {
constexpr std::uint64_t access_failure = std::uint64_t{1} << 32;
constexpr std::uint32_t mode_bits = 0x1f, execution_bits = 0x01000020, byte_mask = 255;

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
  bool running = false, external_entry = true, revoked = false;
  std::shared_ptr<const detail::Native> binding;
  std::size_t variant_index = 0;
  std::vector<detail::DirectRegion> direct;
  std::vector<HostRange> code_ranges;
  std::string identity, pending_image;

  Impl(Memory memory, Io callbacks) : io(callbacks) {
    runtime.owner = this;
    runtime.access = &access;
    rebind(memory);
  }

  void stopped() const {
    if (running) { throw std::logic_error("Core mutation/execution reentry from callback"); }
  }

  void rebind(Memory memory) {
    stopped();
    binding.reset();
    direct.clear();
    code_ranges.clear();
    runtime.direct = nullptr;
    regions.assign(memory.regions.begin(), memory.regions.end());
    std::ranges::sort(regions, {}, &Memory::Region::address);
    std::uint64_t end = 0;
    for (const auto &region : regions) {
      if (region.bytes.empty() || region.bytes.size() > (std::uint64_t{1} << 32) - region.address ||
          region.address < end) {
        throw std::invalid_argument("invalid/overlapping guest memory regions");
      }
      end = region.address + region.bytes.size();
    }
    generation = memory.generation;
  }

  [[nodiscard]] const Memory::Region *region(std::uint32_t address, unsigned width) const {
    for (const auto &candidate : regions) {
      if (address >= candidate.address &&
          std::uint64_t(address - candidate.address) + width <= candidate.bytes.size()) {
        return &candidate;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool code_overlap(std::span<const std::byte> bytes) const {
    const HostRange access{.address = reinterpret_cast<std::uintptr_t>(bytes.data()),
                           .size = bytes.size()};
    return std::ranges::any_of(code_ranges,
                               [access](HostRange code) { return overlaps(access, code); });
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
    if (operation == 0) {
      const auto callback =
          replacement == 0 ? io.read : std::bit_cast<decltype(io.read)>(replacement);
      accepted = callback != nullptr && callback(io.owner, address, width, value);
    } else if (operation == 1) {
      const auto callback =
          replacement == 0 ? io.write : std::bit_cast<decltype(io.write)>(replacement);
      accepted = callback != nullptr && callback(io.owner, address, width, value);
    } else {
      const auto callback =
          replacement == 0 ? io.swap : std::bit_cast<decltype(io.swap)>(replacement);
      accepted = callback != nullptr && callback(io.owner, address, width, value);
    }
    if (accepted) { return value; }
    runtime.signal = static_cast<std::uint32_t>(detail::Signal::memory);
    runtime.address = address;
    return access_failure;
  }

  static std::uint64_t access(detail::Runtime *runtime,
                              std::uint32_t address,
                              unsigned width,
                              unsigned operation,
                              std::uint32_t value,
                              std::uintptr_t replacement) noexcept {
    auto &owner = *static_cast<Impl *>(runtime->owner);
    const auto *region = owner.region(address, width);
    if (region == nullptr) {
      return owner.callback({.address = address,
                             .width = width,
                             .operation = operation,
                             .value = value,
                             .replacement = replacement});
    }
    if ((operation != 1 && !region->readable) || (operation != 0 && !region->writable)) {
      runtime->signal = static_cast<std::uint32_t>(detail::Signal::memory);
      runtime->address = address;
      return access_failure;
    }
    auto bytes = region->bytes.subspan(address - region->address, width);
    const bool big_endian = owner.binding->image->options.big_endian;
    const bool aliases_code = operation != 0 && owner.code_overlap(bytes);
    const auto old = operation != 1 || aliases_code ? read_bytes(bytes, big_endian) : 0;
    if (operation != 0) {
      write_bytes(bytes, value, big_endian);
      const auto mask =
          width == 4 ? std::numeric_limits<std::uint32_t>::max() : (1U << (width * 8)) - 1;
      if (aliases_code && old != (value & mask)) {
        owner.revoked = true;
        runtime->signal = static_cast<std::uint32_t>(detail::Signal::changed);
        runtime->address = address;
      }
    }
    return operation == 0 || operation == 2 ? old : 0;
  }

  void validate_code(const detail::Variant &variant) {
    for (const auto &span : variant.spans) {
      std::size_t offset = 0;
      while (offset < span.bytes.size()) {
        const auto address = span.address + static_cast<std::uint32_t>(offset);
        const auto *mapping = region(address, 1);
        if (mapping == nullptr || !mapping->readable || !mapping->executable) {
          detail::refuse(identity, address, "declared code is not readable executable memory");
        }
        const auto count = std::min(span.bytes.size() - offset,
                                    mapping->bytes.size() - (address - mapping->address));
        const auto actual = mapping->bytes.subspan(address - mapping->address, count);
        if (!std::ranges::equal(actual, std::span(span.bytes).subspan(offset, count))) {
          detail::refuse(identity, address, "installed code bytes differ from declared variant");
        }
        code_ranges.push_back(
            {.address = reinterpret_cast<std::uintptr_t>(actual.data()), .size = actual.size()});
        offset += count;
      }
    }
  }

  void validate_layout(const detail::Variant &variant) {
    for (const auto &layout : variant.layout) {
      const auto *mapping = region(layout.address, 1);
      if (mapping == nullptr || mapping->address != layout.address ||
          mapping->bytes.size() != layout.size || mapping->readable != layout.readable ||
          mapping->writable != layout.writable || mapping->executable != layout.executable) {
        detail::refuse(identity, layout.address, "memory layout differs from prepared declaration");
      }
      direct.push_back(
          {.data = mapping->bytes.data(),
           .safe_write = unsigned(mapping->writable && !code_overlap(mapping->bytes))});
    }
    runtime.direct = direct.data();
  }

  [[nodiscard]] State inspect() const {
    auto result = continuation;
    result.registers = {.r = runtime.r, .cpsr = runtime.cpsr, .spsr = runtime.spsr};
    result.progress = {.instructions = runtime.instructions, .base_cycles = runtime.cycles};
    return result;
  }

  void failure(Fault::Kind kind, std::uint32_t address, std::string reason) {
    continuation.execution = ExecutionState::fault;
    continuation.fault = {
        .kind = kind, .address = address, .identity = identity, .reason = std::move(reason)};
  }

  void finish() {
    if (revoked && runtime.signal != static_cast<std::uint32_t>(detail::Signal::memory)) {
      runtime.signal = static_cast<std::uint32_t>(detail::Signal::changed);
    }
    if (runtime.signal == static_cast<std::uint32_t>(detail::Signal::none)) { return; }
    if (runtime.signal == static_cast<std::uint32_t>(detail::Signal::yielded)) {
      continuation.execution = ExecutionState::yielded;
      return;
    }
    if (runtime.signal == static_cast<std::uint32_t>(detail::Signal::changed) ||
        runtime.signal == static_cast<std::uint32_t>(detail::Signal::transfer)) {
      for (const auto &transition : binding->image->transitions) {
        if (transition.source_variant == binding->image->variants[variant_index].id &&
            transition.address == runtime.r[15]) {
          continuation.execution = ExecutionState::transfer;
          continuation.transition_pending = true;
          continuation.transition = transition;
          pending_image = binding->image->key;
          return;
        }
      }
    }
    switch (static_cast<detail::Signal>(runtime.signal)) {
      case detail::Signal::changed:
        failure(Fault::Kind::changed_code,
                runtime.address,
                "executable bytes changed through a guest mapping or alias");
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
    } else if ((runtime.cpsr & execution_bits) != 0 ||
               (runtime.cpsr & mode_bits) != binding->image->options.processor_mode) {
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
    binding->entries[variant_index](&runtime);
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
  impl_->runtime.direct = nullptr;
  impl_->identity = image.impl_->image->identity;
  const auto &variants = image.impl_->image->variants;
  const auto found = std::ranges::find(variants, variant, &detail::Variant::id);
  if (found == variants.end()) {
    detail::refuse(impl_->identity, impl_->runtime.r[15], "unknown installed variant");
  }
  impl_->validate_code(*found);
  impl_->validate_layout(*found);
  impl_->binding = image.impl_;
  impl_->variant_index = static_cast<std::size_t>(found - variants.begin());
  return image.report();
}

Registers Core::registers() const {
  return {.r = impl_->runtime.r, .cpsr = impl_->runtime.cpsr, .spsr = impl_->runtime.spsr};
}

void Core::set_registers(const Registers &registers) {
  impl_->stopped();
  impl_->runtime.r = registers.r;
  impl_->runtime.cpsr = registers.cpsr;
  impl_->runtime.spsr = registers.spsr;
  impl_->external_entry = true;
}

State Core::state() const {
  return impl_->inspect();
}

Snapshot Core::save() const {
  impl_->stopped();
  return Snapshot(std::make_shared<Snapshot::Impl>(
      impl_->inspect(), impl_->external_entry, impl_->pending_image));
}

void Core::restore(const Snapshot &snapshot) {
  impl_->stopped();
  impl_->continuation = snapshot.impl_->state;
  set_registers(snapshot.impl_->state.registers);
  impl_->external_entry = snapshot.impl_->external_entry;
  impl_->pending_image = snapshot.impl_->pending_image;
  impl_->runtime.instructions = snapshot.impl_->state.progress.instructions;
  impl_->runtime.cycles = snapshot.impl_->state.progress.base_cycles;
  impl_->runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
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

bool Core::interrupt_ready(InterruptRequest /*request*/) const {
  impl_->stopped();
  detail::refuse(impl_->identity, impl_->runtime.r[15], "interrupt eligibility requires A3");
}

void Core::interrupt(InterruptRequest /*request*/) {
  impl_->stopped();
  detail::refuse(impl_->identity, impl_->runtime.r[15], "interrupt entry requires A3");
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
  impl_->runtime.instructions = 0;
  impl_->runtime.cycles = 0;
  impl_->runtime.signal = static_cast<std::uint32_t>(detail::Signal::none);
  impl_->external_entry = true;
}

void Core::resume_from_stop() {
  impl_->stopped();
  detail::refuse(impl_->identity, impl_->runtime.r[15], "architectural stop requires A3/A5");
}
} // namespace armv5
