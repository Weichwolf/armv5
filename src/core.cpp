// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"

#include "armv5/memory.hpp"
#include "decode.hpp"
#include "identity.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace armv5 {
namespace {

[[noreturn]] void unavailable(std::string_view action, std::uint32_t pc = 0) {
  std::ostringstream message;
  message << action << " at 0x" << std::hex << pc
          << ": not implemented in the A1 preparation milestone";
  throw std::runtime_error(message.str());
}

[[noreturn]] void refuse(const CodeImage &image, std::uint32_t address, std::string_view reason) {
  std::ostringstream message;
  message << image.identity << " at 0x" << std::hex << std::setw(8) << std::setfill('0') << address
          << ": " << reason;
  throw std::runtime_error(message.str());
}

std::uint32_t instruction(std::span<const std::byte> bytes, bool big_endian) {
  std::uint32_t word = 0;
  for (unsigned index = 0; index < 4; ++index) {
    const auto shift = (big_endian ? 3 - index : index) * 8;
    word |= std::to_integer<std::uint32_t>(bytes[index]) << shift;
  }
  return word;
}

void validate_spans(const CodeImage &image, const CodeVariant &variant) {
  if (variant.spans.empty()) { refuse(image, 0, "variant has no executable spans"); }
  if (variant.entries.empty()) { refuse(image, 0, "variant has no declared entries"); }
  for (const auto &span : variant.spans) {
    if (span.bytes.empty() || (span.address & 3) != 0 || (span.bytes.size() & 3) != 0) {
      refuse(image, span.address, "ARM executable span must contain aligned complete words");
    }
    if (span.bytes.size() > (std::uint64_t{1} << 32) - span.address) {
      refuse(image, span.address, "executable span crosses the address-space boundary");
    }
  }
  for (std::size_t index = 0; index < variant.spans.size(); ++index) {
    const auto &first = variant.spans[index];
    for (std::size_t other = index + 1; other < variant.spans.size(); ++other) {
      const auto &second = variant.spans[other];
      if (first.address < second.address + second.bytes.size() &&
          second.address < first.address + first.bytes.size()) {
        refuse(image, second.address, "overlapping executable spans in one variant");
      }
    }
  }
  for (const auto entry : variant.entries) {
    if ((entry & 3) != 0) { refuse(image, entry, "unaligned ARM entry"); }
    if (std::ranges::none_of(variant.spans, [entry](const CodeSpan &span) {
          return entry >= span.address && entry - span.address < span.bytes.size();
        })) {
      refuse(image, entry, "entry is outside declared executable spans");
    }
  }
}

} // namespace

struct SemanticImage::Impl {
  std::vector<std::byte> bitcode, artifact;
  std::vector<std::string> exports;
  std::string identity;
};

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

struct NativeImage::Impl {
  PreparationReport report;
};

NativeImage::NativeImage(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

const PreparationReport &NativeImage::report() const {
  return impl_->report;
}

struct Snapshot::Impl {
  State state;
};

Snapshot::Snapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

struct Core::Impl {
  Memory memory;
  Io io;
  State state;
};

Core::Core(Memory memory, Io io)
    : impl_(std::make_unique<Impl>(Impl{.memory = memory, .io = io, .state = {}})) {}

Core::~Core() = default;

std::string_view Core::compiler_identity() noexcept {
  return detail::compiler_identity;
}

SemanticImage Core::lower(const CodeImage &image, const CompileOptions &options) {
  if (image.identity.empty()) { refuse(image, 0, "code identity is empty"); }
  if (image.variants.empty()) { refuse(image, 0, "no declared code variants"); }
  if (options.instruction_set != CompileOptions::InstructionSet::arm) {
    refuse(image, 0, "Thumb decoding is not implemented (A4)");
  }
  std::string_view first_operation;
  for (std::size_t index = 0; index < image.variants.size(); ++index) {
    const auto &variant = image.variants[index];
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (image.variants[previous].id == variant.id) {
        refuse(image, 0, "duplicate code variant id");
      }
    }
    validate_spans(image, variant);
    for (const auto &span : variant.spans) {
      for (std::size_t offset = 0; offset < span.bytes.size(); offset += 4) {
        const auto address = span.address + static_cast<std::uint32_t>(offset);
        detail::Decoded decoded{};
        try {
          decoded =
              detail::decode_arm(instruction(span.bytes.subspan(offset, 4), options.big_endian));
        } catch (const std::runtime_error &error) { refuse(image, address, error.what()); }
        if (first_operation.empty()) { first_operation = decoded.operation; }
      }
    }
  }
  // A1 deliberately has no executable semantic image. Recognizing an encoding
  // must never turn into a no-op, interpreter dispatch or a fake native entry.
  std::string reason("decoded ARM ");
  reason.append(first_operation).append("; LLVM lowering is not implemented (A2)");
  refuse(image, image.variants.front().spans.front().address, reason);
}

SemanticImage Core::load(const CodeImage &image,
                         const CompileOptions & /*options*/,
                         std::span<const std::byte> /*artifact*/) {
  refuse(image, 0, "artifact loading requires LLVM lowering (A2)");
}

NativeImage Core::bind(const SemanticImage & /*image*/, const NativeSymbols & /*symbols*/) {
  unavailable("native binding");
}

NativeImage Core::compile(const CodeImage &image, const CompileOptions &options) {
  return bind(lower(image, options), {});
}

PreparationReport Core::install(const NativeImage & /*image*/, std::uint32_t /*variant*/) {
  unavailable("native installation", impl_->state.registers.r.back());
}

Registers Core::registers() const {
  return impl_->state.registers;
}

void Core::set_registers(const Registers &registers) {
  impl_->state.registers = registers;
}

State Core::state() const {
  return impl_->state;
}

Snapshot Core::save() const {
  return Snapshot(std::make_shared<Snapshot::Impl>(impl_->state));
}

void Core::restore(const Snapshot &snapshot) {
  impl_->state = snapshot.impl_->state;
}

void Core::set_memory(Memory memory) {
  impl_->memory = memory;
}

RunResult Core::run(std::uint64_t instruction_budget) {
  if (instruction_budget == 0) {
    return {.execution = impl_->state.execution, .progress = {}, .fault = impl_->state.fault};
  }
  impl_->state.execution = ExecutionState::fault;
  impl_->state.fault = {.kind = Fault::Kind::unprepared,
                        .address = impl_->state.registers.r.back(),
                        .identity = {},
                        .reason =
                            "no installed native image; LLVM lowering is not implemented (A2)"};
  return {.execution = impl_->state.execution, .progress = {}, .fault = impl_->state.fault};
}

RunResult Core::run_until(std::uint64_t base_cycle_deadline) {
  return run(base_cycle_deadline > impl_->state.progress.base_cycles ? 1 : 0);
}

void Core::request_yield() {
  impl_->state.execution = ExecutionState::yielded;
}

bool Core::interrupt_ready(InterruptRequest /*request*/) const {
  unavailable("interrupt eligibility (A3)", impl_->state.registers.r.back());
}

void Core::interrupt(InterruptRequest /*request*/) {
  unavailable("interrupt entry (A3)", impl_->state.registers.r.back());
}

void Core::reset(std::uint32_t vector) {
  if ((vector & 3) != 0) { throw std::invalid_argument("reset vector must be word aligned"); }
  impl_->state = {};
  impl_->state.registers.r.back() = vector;
}

void Core::resume_from_stop() {
  unavailable("resume from architectural stop (A3)", impl_->state.registers.r.back());
}

} // namespace armv5
