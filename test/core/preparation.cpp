// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "support/isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::uint32_t unaligned_address = 0x1001, last_word = 0xfffffffc, reset_status = 0xd3,
                        changed_value = 42;

void refused(const armv5::CodeImage &image, std::string_view expected) {
  try {
    (void)armv5::Core::lower(image);
  } catch (const std::runtime_error &error) {
    fixture::require(std::string_view(error.what()).find(expected) != std::string_view::npos,
                     error.what());
    return;
  }
  throw std::runtime_error("invalid declaration accepted");
}

void declarations() {
  refused({}, "code identity is empty");
  refused({.identity = "empty",
           .variants = {},
           .layouts = {},
           .transitions = {},
           .interrupt_vectors = {}},
          "no declared code variants");
  std::array<std::byte, 8> bytes{};
  std::array spans{armv5::CodeSpan{.address = 0x1000, .bytes = bytes}};
  std::array<std::uint32_t, 1> entries{0x1000};
  std::array variants{armv5::CodeVariant{.id = 0, .spans = spans, .entries = entries}};
  armv5::CodeImage image{.identity = "declarations",
                         .variants = variants,
                         .layouts = {},
                         .transitions = {},
                         .interrupt_vectors = {}};
  entries.front() = unaligned_address;
  refused(image, "unaligned ARM entry");
  entries.front() = 0x2000;
  refused(image, "entry is outside");
  entries.front() = 0x1000;
  spans.front().address = unaligned_address;
  refused(image, "aligned complete words");
  spans.front() = {.address = last_word, .bytes = bytes};
  refused(image, "address-space boundary");
  spans.front() = {.address = 0x1000, .bytes = std::span(bytes).first(3)};
  refused(image, "aligned complete words");
  spans.front() = {.address = 0x1000, .bytes = bytes};
  const std::array overlaps{spans.front(),
                            armv5::CodeSpan{.address = 0x1004, .bytes = std::span(bytes).first(4)}};
  variants.front().spans = overlaps;
  refused(image, "overlapping executable spans");
  variants.front().spans = spans;
  const std::array duplicates{variants.front(), variants.front()};
  image.variants = duplicates;
  refused(image, "duplicate code variant id");
}

void inactive_owner() {
  armv5::Core core;
  fixture::require(core.registers().cpsr == reset_status, "initial ARM supervisor reset state");
  fixture::require(core.state().progress == armv5::Progress{}, "initial progress");
  const auto initial = core.save();
  auto registers = core.registers();
  registers.r[0] = changed_value;
  registers.r[15] = 0x1000;
  core.set_registers(registers);
  fixture::require(core.registers() == registers, "register inspection");
  fixture::require(core.run(0).execution == armv5::ExecutionState::ready,
                   "zero bound changed state");
  const auto result = core.run(1);
  fixture::require(result.execution == armv5::ExecutionState::fault,
                   "unprepared execution accepted");
  fixture::require(result.fault.address == 0x1000, "missing unprepared PC");
  fixture::require(core.state().progress == armv5::Progress{},
                   "unprepared execution fabricated work");
  core.restore(initial);
  fixture::require(core.state() == armv5::State{},
                   "snapshot retained later fault/register residue");
  core.reset(0x1000);
  fixture::require(core.registers().r[15] == 0x1000, "reset vector");
  const auto identity = armv5::Core::compiler_identity();
  fixture::require(identity.starts_with("armv5-") &&
                       identity.find("-llvm-19.") != std::string_view::npos,
                   "compiler identity must identify sources and LLVM");
}

} // namespace

int main() {
  try {
    declarations();
    inactive_owner();
    std::cout << "Core preparation: declarations and non-executing owner lifecycle passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
