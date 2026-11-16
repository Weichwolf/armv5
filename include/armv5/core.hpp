// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "armv5/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace armv5 {

struct Registers {
  static constexpr std::uint32_t reset_cpsr = 0x000000d3;
  std::array<std::uint32_t, 16> r{};
  std::uint32_t cpsr = reset_cpsr; // ARM, supervisor, IRQ and FIQ masked.
  std::uint32_t spsr = 0;
  bool operator==(const Registers &) const = default;
};

struct Progress {
  std::uint64_t instructions = 0, base_cycles = 0;
  bool operator==(const Progress &) const = default;
};

enum class ExecutionState : std::uint8_t { ready, stopped, yielded, transfer, fault };

struct Fault {
  enum class Kind : std::uint8_t { none, unprepared, changed_code, memory, unsupported };
  Kind kind = Kind::none;
  std::uint32_t address = 0;
  std::string identity, reason;
  bool operator==(const Fault &) const = default;
};

struct CodeTransition {
  std::uint32_t source_variant = 0, destination_variant = 0, address = 0;
  bool operator==(const CodeTransition &) const = default;
};

struct State {
  struct Banks {
    std::array<std::uint32_t, 7> user{}, fiq{}; // R8..R14; user also serves System mode.
    std::array<std::uint32_t, 2> irq{}, supervisor{}, abort{}, undefined{}; // R13..R14.
    std::array<std::uint32_t, 5> spsr{}; // FIQ, IRQ, Supervisor, Abort, Undefined.
    bool operator==(const Banks &) const = default;
  };

  Registers registers;
  Banks banks;
  Progress progress;
  ExecutionState execution = ExecutionState::ready;
  Fault fault;
  // Explicit continuation, independent of which native image is installed.
  bool transition_pending = false;
  CodeTransition transition;
  bool operator==(const State &) const = default;
};

struct RunResult {
  ExecutionState execution = ExecutionState::ready;
  Progress progress; // Completed work in this call; State holds cumulative work.
  Fault fault;
};

struct CodeSpan {
  std::uint32_t address = 0;
  std::span<const std::byte> bytes;
  // Empty inherits CompileOptions::processor_mode; shared code lists every mode.
  std::span<const std::uint32_t> processor_modes;
};

struct CodeVariant {
  std::uint32_t id = 0;
  std::span<const CodeSpan> spans;
  std::span<const std::uint32_t> entries;
};

struct RegionLayout {
  std::uint32_t address = 0;
  std::uint64_t size = 0;
  bool readable = true, writable = false, executable = false;
};

struct VariantLayout {
  std::uint32_t variant = 0;
  std::span<const RegionLayout> regions;
};

struct CodeImage {
  std::string_view identity;
  std::span<const CodeVariant> variants;
  std::span<const VariantLayout> layouts;
  std::span<const CodeTransition> transitions;
  std::span<const std::uint32_t> interrupt_vectors;
};

struct InterruptRequest {
  enum class Kind : std::uint8_t { irq, fiq };
  Kind kind = Kind::irq;
};

struct CallbackBinding {
  enum class Role : std::uint8_t { read, write, swap };
  Role role = Role::read;
  std::string symbol;
};

struct CompileOptions {
  static constexpr std::uint32_t supervisor_mode = 0x13;
  enum class InstructionSet : std::uint8_t { arm, thumb };
  InstructionSet instruction_set = InstructionSet::arm;
  bool big_endian = false;
  std::uint32_t processor_mode = supervisor_mode; // One CPSR mode per prepared unit.
  std::vector<CallbackBinding> callbacks;
};

struct PreparationReport {
  std::string identity;
  double seconds = 0;
  std::uint64_t native_bytes = 0;
};

// Opaque immutable values own their data. No consumer storage or Core pointer is
// retained by preparation. No native entry ABI is exposed here.
class SemanticImage {
public:
  [[nodiscard]] std::span<const std::byte> bitcode() const;
  [[nodiscard]] std::span<const std::byte> artifact() const;
  [[nodiscard]] std::span<const std::string> exports() const;
  [[nodiscard]] std::string_view identity() const;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  explicit SemanticImage(std::shared_ptr<const Impl> impl);
  friend class Core;
};

struct NativeSymbols {
  struct Symbol {
    std::string name;
    std::uintptr_t address = 0;
  };

  std::span<const Symbol> symbols;
  // Keeps consumer-owned code alive for the lifetime of every installed image.
  std::shared_ptr<const void> lifetime;
  PreparationReport report;
};

class NativeImage {
public:
  [[nodiscard]] const PreparationReport &report() const;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  explicit NativeImage(std::shared_ptr<const Impl> impl);
  friend class Core;
};

class Snapshot {
private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  explicit Snapshot(std::shared_ptr<const Impl> impl);
  friend class Core;
};

// One owner thread; mutators run while stopped, except request_yield from a
// synchronous callback. Preparation is static and independent of any instance.
class Core {
public:
  explicit Core(Memory memory = {}, Io io = {});
  ~Core();
  Core(const Core &) = delete;
  Core &operator=(const Core &) = delete;
  Core(Core &&) = delete;
  Core &operator=(Core &&) = delete;

  [[nodiscard]] static std::string_view compiler_identity() noexcept;
  [[nodiscard]] static SemanticImage lower(const CodeImage &image,
                                           const CompileOptions &options = {});
  [[nodiscard]] static SemanticImage
  load(const CodeImage &image, const CompileOptions &options, std::span<const std::byte> artifact);
  [[nodiscard]] static NativeImage bind(const SemanticImage &image, const NativeSymbols &symbols);
  [[nodiscard]] static NativeImage compile(const CodeImage &image,
                                           const CompileOptions &options = {});
  [[nodiscard]] PreparationReport install(const NativeImage &image, std::uint32_t variant);

  [[nodiscard]] Registers registers() const;
  void set_registers(const Registers &registers);
  [[nodiscard]] State state() const;
  [[nodiscard]] Snapshot save() const;
  void restore(const Snapshot &snapshot);
  void set_memory(Memory memory);
  [[nodiscard]] RunResult run(std::uint64_t instruction_budget);
  [[nodiscard]] RunResult run_until(std::uint64_t base_cycle_deadline);
  void request_yield();
  [[nodiscard]] bool interrupt_ready(InterruptRequest request) const;
  void interrupt(InterruptRequest request);
  void reset(std::uint32_t vector = 0);
  void resume_from_stop();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace armv5
