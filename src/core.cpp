// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"

#include "armv5/memory.hpp"
#include "identity.hpp"
#include "image.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
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
  bool running = false, external_entry = true, revoked = false;
  std::shared_ptr<const detail::Native> binding;
  std::size_t variant_index = 0;
  std::uint32_t revocation_address = 0;
  std::array<detail::Entry, 64> mode_entries{};
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
    const auto [vector, mode] = target;
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
      case detail::Signal::software_interrupt:
        return exception(
            {.vector = 8, .mode = detail::supervisor_mode}, runtime.r[15], false, false);
      case detail::Signal::undefined:
        return exception(
            {.vector = 4, .mode = detail::undefined_mode}, runtime.r[15], false, false);
      case detail::Signal::prefetch_abort:
        return exception(
            {.vector = 12, .mode = detail::abort_mode}, runtime.address + 4, false, false);
      case detail::Signal::data_abort:
        return exception(
            {.vector = 16, .mode = detail::abort_mode}, runtime.r[15] + 8, false, true);
      default: return false;
    }
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
    runtime.signal =
        static_cast<std::uint32_t>(present ? detail::Signal::memory : detail::Signal::data_abort);
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
      runtime->signal = static_cast<std::uint32_t>(detail::Signal::data_abort);
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
        if (!owner.revoked) { owner.revocation_address = address; }
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
          {.address_bias = reinterpret_cast<std::uintptr_t>(mapping->bytes.data()) - layout.address,
           .safe_write = unsigned(mapping->writable && !code_overlap(mapping->bytes))});
    }
    runtime.direct = direct.data();
  }

  [[nodiscard]] State inspect() const {
    auto result = continuation;
    result.registers = {.r = runtime.r, .cpsr = runtime.cpsr, .spsr = runtime.spsr};
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

  void finish() {
    if (continuation.execution == ExecutionState::fault) { return; }
    if (revoked && runtime.signal != static_cast<std::uint32_t>(detail::Signal::memory)) {
      runtime.signal = static_cast<std::uint32_t>(detail::Signal::changed);
      runtime.address = revocation_address;
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
      const auto entry = mode_entries[runtime.cpsr & (mode_bits | thumb_bit)];
      if (entry == nullptr || (runtime.cpsr & jazelle_bit) != 0) {
        runtime.signal = static_cast<std::uint32_t>(detail::Signal::unsupported);
        runtime.address = runtime.r[15];
        break;
      }
      entry(&runtime);
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
  impl_->mode_entries.fill(nullptr);
  const auto &units = image.impl_->image->units;
  for (std::size_t index = 0; index < units.size(); ++index) {
    if (units[index].variant == impl_->variant_index) {
      impl_->mode_entries[units[index].mode | (units[index].thumb ? thumb_bit : 0)] =
          image.impl_->entries[index];
    }
  }
  return image.report();
}

Registers Core::registers() const {
  return {.r = impl_->runtime.r, .cpsr = impl_->runtime.cpsr, .spsr = impl_->runtime.spsr};
}

void Core::set_registers(const Registers &registers) {
  impl_->stopped();
  if (!detail::valid_mode(registers.cpsr & mode_bits)) {
    throw std::invalid_argument("invalid processor mode");
  }
  impl_->save_bank(impl_->runtime.banks);
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
  impl_->runtime.r = snapshot.impl_->state.registers.r;
  impl_->runtime.cpsr = snapshot.impl_->state.registers.cpsr;
  impl_->runtime.spsr = snapshot.impl_->state.registers.spsr;
  impl_->runtime.banks = snapshot.impl_->state.banks;
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

bool Core::interrupt_ready(InterruptRequest request) const {
  impl_->stopped();
  const auto mask = request.kind == InterruptRequest::Kind::fiq ? 0x40U : 0x80U;
  return (impl_->runtime.cpsr & mask) == 0 && !impl_->continuation.transition_pending &&
         impl_->continuation.execution != ExecutionState::fault;
}

void Core::interrupt(InterruptRequest request) {
  impl_->stopped();
  if (!interrupt_ready(request)) { return; }
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
  impl_->runtime.banks = {};
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
