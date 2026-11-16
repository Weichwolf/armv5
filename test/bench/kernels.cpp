// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "reference.hpp"
#include "support/isa.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>
#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
struct Measurement {
  double seconds = 0, instructions = 0, cycles = 0;
};

class Counters {
#if defined(__linux__)
  struct Values {
    std::uint64_t count, enabled, running;
    std::array<std::uint64_t, 2> values;
  };

  int instructions_ = -1, cycles_ = -1;
  std::uint64_t enabled_before_ = 0, running_before_ = 0;

  [[nodiscard]] Values read_values() const {
    Values values{};
    if (read(instructions_, &values, sizeof(values)) != sizeof(values) || values.count != 2) {
      throw std::runtime_error("counter group read failed");
    }
    return values;
  }
#endif
public:
  Counters(const Counters &) = delete;
  Counters &operator=(const Counters &) = delete;
#if defined(__linux__)
  Counters() {
    perf_event_attr event{};
    event.size = sizeof(event);
    event.type = PERF_TYPE_HARDWARE;
    event.config = PERF_COUNT_HW_INSTRUCTIONS;
    event.disabled = 1;
    event.exclude_kernel = 1;
    event.exclude_hv = 1;
    event.read_format =
        PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    instructions_ = int(syscall(SYS_perf_event_open, &event, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
    if (instructions_ < 0) {
      throw std::system_error(errno, std::generic_category(), "user hardware counters refused");
    }
    event.config = PERF_COUNT_HW_CPU_CYCLES;
    cycles_ = int(syscall(SYS_perf_event_open, &event, 0, -1, instructions_, PERF_FLAG_FD_CLOEXEC));
    if (cycles_ < 0) {
      close(instructions_);
      throw std::runtime_error("user cycle counter refused");
    }
  }

  ~Counters() {
    close(cycles_);
    close(instructions_);
  }

  void begin() {
    if (ioctl(instructions_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0) {
      throw std::runtime_error("counter reset failed");
    }
    const auto before = read_values();
    enabled_before_ = before.enabled;
    running_before_ = before.running;
    if (ioctl(instructions_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
      throw std::runtime_error("counter enable failed");
    }
  }

  [[nodiscard]] Measurement end(double seconds) const {
    if (ioctl(instructions_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) != 0) {
      throw std::runtime_error("counter disable failed");
    }
    const auto values = read_values();
    const auto enabled = values.enabled - enabled_before_;
    const auto running = values.running - running_before_;
    if (running == 0) { throw std::runtime_error("hardware counters did not run"); }
    const auto scale = double(enabled) / double(running);
    return {.seconds = seconds,
            .instructions = double(values.values[0]) * scale,
            .cycles = double(values.values[1]) * scale};
  }
#else
  Counters() { throw std::runtime_error("hardware counters are unqualified on this host"); }

  void begin() {}

  [[nodiscard]] Measurement end(double seconds) const { return {.seconds = seconds}; }
#endif
};

template <class Work> Measurement measure(Counters &counters, Work work) {
  counters.begin();
  const auto start = std::chrono::steady_clock::now();
  work();
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return counters.end(seconds);
}

void report(std::string_view kernel,
            std::uint64_t guests,
            std::size_t words,
            const armv5::PreparationReport &prepared,
            std::vector<Measurement> native,
            std::vector<Measurement> reference) {
  std::ranges::sort(native, {}, &Measurement::seconds);
  std::ranges::sort(reference, {}, &Measurement::seconds);
  const auto &value = native[native.size() / 2];
  const auto &cpp = reference[reference.size() / 2];
  std::cout << kernel << ',' << guests << ',' << value.instructions / double(guests) << ','
            << value.cycles / double(guests) << ',' << double(guests) / value.seconds << ','
            << value.seconds / cpp.seconds << ',' << prepared.seconds << ','
            << prepared.native_bytes << ',' << double(prepared.native_bytes) / double(words)
            << '\n';
}

fixture::Program copy_program() {
  fixture::Program code;
  code.word(fixture::data(13, false, 0, 0, false, 8));
  code.word(fixture::data(13, false, 1, 0, false, 9));
  code.word(fixture::data(13, false, 2, 0, false, 10));
  const auto loop = code.pc();
  code.word(fixture::transfer(true, false, 4, 0, 4, false));
  code.word(fixture::transfer(false, false, 4, 1, 4, false));
  code.word(fixture::data(4, false, 3, 3, false, 4));
  code.word(fixture::data(2, true, 2, 2, true, 1));
  code.word(fixture::branch(code.pc(), {.address = loop}, 1));
  code.word(fixture::data(2, true, 7, 7, true, 1));
  code.word(fixture::branch(code.pc(), {.address = code.address()}, 1));
  code.word(fixture::branch(code.pc(), {.address = code.pc()}));
  return code;
}

void copy_kernel(Counters &counters) {
  constexpr unsigned words = 4096;
  constexpr unsigned repetitions = 512;
  constexpr std::uint32_t multiplier = 0x9e3779b9;
  constexpr std::uint32_t seed = 0x1234567;
  constexpr std::uint32_t shift_mask = 31;
  const auto program = copy_program();
  fixture::Machine machine(program);
  std::vector<std::uint32_t> source(words);
  std::vector<std::uint32_t> destination(words);
  std::uint32_t checksum = 0;
  for (unsigned index = 0; index < words; ++index) {
    source[index] = (index * multiplier) ^ std::rotl(index + seed, int(index & shift_mask));
    checksum += source[index];
    fixture::put(std::span(machine.ram).subspan(std::size_t(index) * 4, 4), source[index]);
  }
  checksum *= repetitions;
  auto expected = machine.ram;
  std::copy_n(expected.begin(), words * 4, expected.begin() + std::ptrdiff_t(words) * 4);
  machine.prepare();
  constexpr std::uint64_t guests = std::uint64_t(repetitions) * (3 + 5 * words + 2);
  constexpr std::uint64_t cycles = (std::uint64_t(repetitions) * (7 * words + 5)) - 2;
  auto initialize = [&] {
    machine.core->reset(program.address());
    auto registers = machine.core->registers();
    registers.r[7] = repetitions;
    registers.r[8] = 0x4000;
    registers.r[9] = 0x8000;
    registers.r[10] = words;
    machine.registers(registers);
  };
  armv5::Registers expected_registers;
  constexpr std::uint32_t completed_status = 0x600000d3;
  expected_registers.cpsr = completed_status;
  expected_registers.r[0] = 0x8000;
  expected_registers.r[1] = expected_registers.r[0] + words * 4;
  expected_registers.r[3] = checksum;
  expected_registers.r[4] = source.back();
  expected_registers.r[8] = 0x4000;
  expected_registers.r[9] = 0x8000;
  expected_registers.r[10] = words;
  expected_registers.r[15] = program.pc() - 4;
  auto check = [&] {
    fixture::require(machine.ram == expected && machine.core->registers() == expected_registers,
                     "K5 complete registers and RAM output");
    fixture::require(machine.core->state().execution == armv5::ExecutionState::ready,
                     "kernel did not return at a normal boundary");
    fixture::require(machine.core->state().progress ==
                         armv5::Progress{.instructions = guests, .base_cycles = cycles},
                     "K5 exact retirement/base cycles");
  };
  initialize();
  (void)machine.run(guests);
  check();
  std::vector<Measurement> native;
  std::vector<Measurement> reference;
  for (unsigned trial = 0; trial < 3; ++trial) {
    initialize();
    native.push_back(measure(counters, [&] { (void)machine.core->run(guests); }));
    check();
  }
  fixture::require(benchmark::copy(source.data(),
                                   destination.data(),
                                   {.words = words, .repetitions = repetitions}) == checksum &&
                       destination == source,
                   "K5 C++ warm output");
  for (unsigned trial = 0; trial < 3; ++trial) {
    std::uint32_t actual = 0;
    reference.push_back(measure(counters, [&] {
      actual = benchmark::copy(
          source.data(), destination.data(), {.words = words, .repetitions = repetitions});
    }));
    fixture::require(actual == checksum && destination == source, "K5 C++ complete output");
  }
  report("K5", guests, program.bytes().size() / 4, machine.prepared().report(), native, reference);
}

fixture::Program control_program() {
  constexpr unsigned handlers = 8;
  constexpr unsigned handler_words = 6;
  constexpr unsigned main_words = 6;
  fixture::Program code;
  code.word(fixture::data(0, false, 0, 0, true, 7));
  constexpr unsigned scaled_index = 2U << 7;
  code.word(fixture::transfer(true, false, 4, 8, scaled_index, true, true, false, true));
  constexpr std::uint32_t blx_r4 = 0xe12fff34;
  code.word(blx_r4);
  code.word(fixture::data(2, true, 7, 7, true, 1));
  code.word(fixture::branch(code.pc(), {.address = code.address()}, 1));
  code.word(fixture::branch(code.pc(), {.address = code.pc()}));
  const auto common = code.address() + ((main_words + handlers * handler_words) * 4);
  for (unsigned handler = 0; handler < handlers; ++handler) {
    code.word(fixture::transfer(false, false, 14, 13, 4, true, false, true));
    code.word(fixture::data(4, false, 1, 1, false, 0));
    code.word(fixture::data(1, false, 1, 1, true, handler + 1));
    code.word(fixture::data(13, false, 0, 0, true, (handler + 3) & 7));
    code.word(fixture::branch(code.pc(), {.address = common}, 14, true));
    code.word(fixture::transfer(true, false, 15, 13, 4, false));
  }
  constexpr unsigned rotated_state = (1U << 7) | (3U << 5);
  code.word(fixture::data(1, false, 1, 1, false, rotated_state));
  code.word(fixture::data(4, false, 1, 1, true, 3));
  constexpr std::uint32_t bx_lr = 0xe12fff1e;
  code.word(bx_lr);
  return code;
}

void control_kernel(Counters &counters) {
  constexpr std::uint32_t iterations = 1U << 20;
  constexpr std::uint32_t initial_accumulator = 0x2468ace0;
  constexpr std::uint32_t stack = 0x12000;
  constexpr std::uint64_t guests = std::uint64_t(iterations) * 14;
  constexpr std::uint64_t cycles = (std::uint64_t(iterations) * 26) - 2;
  const auto program = control_program();
  fixture::Machine machine(program);
  for (unsigned handler = 0; handler < 8; ++handler) {
    fixture::put(std::span(machine.ram).subspan(std::size_t(handler) * 4, 4),
                 program.address() + ((6 + handler * 6) * 4));
  }
  const auto answer = benchmark::control(
      iterations, {.state = 3, .accumulator = initial_accumulator, .last_handler = 0});
  auto expected = machine.ram;
  fixture::put(std::span(expected).subspan(stack - 0x4000 - 4, 4), program.address() + 12);
  machine.prepare();
  auto initialize = [&] {
    machine.core->reset(program.address());
    auto registers = machine.core->registers();
    registers.r[0] = 3;
    registers.r[1] = initial_accumulator;
    registers.r[7] = iterations;
    registers.r[8] = 0x4000;
    registers.r[13] = stack;
    machine.registers(registers);
  };
  armv5::Registers expected_registers;
  constexpr std::uint32_t completed_status = 0x600000d3;
  expected_registers.cpsr = completed_status;
  expected_registers.r[0] = answer.state;
  expected_registers.r[1] = answer.accumulator;
  expected_registers.r[4] = program.address() + (6 + (answer.last_handler * 6)) * 4;
  expected_registers.r[8] = 0x4000;
  expected_registers.r[13] = stack;
  expected_registers.r[14] = expected_registers.r[4] + (5 * 4);
  expected_registers.r[15] = program.address() + (5 * 4);
  auto check = [&] {
    fixture::require(machine.core->registers() == expected_registers && machine.ram == expected,
                     "K4 complete registers/table/stack output");
    fixture::require(machine.core->state().execution == armv5::ExecutionState::ready,
                     "kernel did not return at a normal boundary");
    fixture::require(machine.core->state().progress ==
                         armv5::Progress{.instructions = guests, .base_cycles = cycles},
                     "K4 exact retirement/base cycles");
  };
  initialize();
  (void)machine.run(guests);
  check();
  std::vector<Measurement> native;
  std::vector<Measurement> reference;
  for (unsigned trial = 0; trial < 3; ++trial) {
    initialize();
    native.push_back(measure(counters, [&] { (void)machine.core->run(guests); }));
    check();
  }
  for (unsigned trial = 0; trial < 3; ++trial) {
    benchmark::Control result{};
    reference.push_back(measure(counters, [&] {
      result = benchmark::control(
          iterations, {.state = 3, .accumulator = initial_accumulator, .last_handler = 0});
    }));
    fixture::require(result.state == answer.state && result.accumulator == answer.accumulator &&
                         result.last_handler == answer.last_handler,
                     "K4 C++ complete output");
  }
  report("K4", guests, program.bytes().size() / 4, machine.prepared().report(), native, reference);
}
}

int main(int argc, char **argv) {
  try {
    Counters counters;
    std::cout << std::setprecision(9)
              << "kernel,guest_instructions,host_instructions_per_guest,host_cycles_per_guest,"
                 "guest_per_second,native_over_clang_O3_seconds,preparation_seconds,native_text_"
                 "bytes,text_bytes_per_prepared_instruction\n";
    const std::string_view selected = argc > 1 ? argv[1] : "all";
    fixture::require(selected == "all" || selected == "K4" || selected == "K5",
                     "unknown benchmark kernel");
    if (selected == "all" || selected == "K5") { copy_kernel(counters); }
    if (selected == "all" || selected == "K4") { control_kernel(counters); }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
