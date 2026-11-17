// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "fft.hpp"
#include "machine.hpp"
#include "reference.hpp"
#include "support/isa.hpp"
#include "support/modes.hpp"

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
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/SHA256.h>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
using BenchmarkMachine = benchmark::Machine;

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

struct Workload {
  std::uint64_t cycles, samples;
};

std::string_view mapping_name() {
  switch (benchmark::configuration.mapping) {
    case benchmark::Mapping::off: return "off";
    case benchmark::Mapping::identity: return "identity";
    case benchmark::Mapping::remapped: return "remapped";
  }
  throw std::logic_error("invalid benchmark mapping");
}

void report(std::string_view kernel,
            std::uint64_t guests,
            std::size_t words,
            const BenchmarkMachine &machine,
            std::vector<Measurement> native,
            std::vector<Measurement> reference,
            Workload workload) {
  const auto &prepared = machine.prepared().report();
  std::ranges::sort(native, {}, &Measurement::seconds);
  std::ranges::sort(reference, {}, &Measurement::seconds);
  const auto &value = native[native.size() / 2];
  const auto &cpp = reference[reference.size() / 2];
  constexpr double nanoseconds = 1e9;
  std::cout << kernel << ',' << guests << ',' << value.instructions / double(guests) << ','
            << value.cycles / double(guests) << ',' << double(guests) / value.seconds << ','
            << value.seconds / cpp.seconds << ',' << prepared.seconds << ','
            << prepared.native_bytes << ',' << double(prepared.native_bytes) / double(words) << ','
            << double(guests) / double(workload.samples) << ','
            << double(workload.cycles) / double(workload.samples) << ','
            << nanoseconds * value.seconds / double(guests) << ',' << 1.0 << ','
            << (benchmark::configuration.memory == armv5::CompileOptions::MemoryMode::direct
                    ? "direct"
                    : "exact")
            << ',' << mapping_name() << ',' << machine.installation_seconds << '\n';
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
  BenchmarkMachine machine(program);
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
  auto expected_registers = machine.expected_registers();
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
    machine.verify_storage();
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
  report("K5",
         guests,
         program.bytes().size() / 4,
         machine,
         native,
         reference,
         {.cycles = cycles, .samples = std::uint64_t(words) * repetitions});
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
  BenchmarkMachine machine(program);
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
  auto expected_registers = machine.expected_registers();
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
    machine.verify_storage();
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
  report("K4",
         guests,
         program.bytes().size() / 4,
         machine,
         native,
         reference,
         {.cycles = cycles, .samples = iterations});
}

constexpr std::uint32_t event_return = 0xe25ef004, irq_vector = 0x18, supervisor = 0x13,
                        irq_processor_mode = 0x12, fir_completed_status = 0x60000013;
constexpr unsigned taps = 64, frames = 1024, blocks = 256, outputs = frames * blocks;
constexpr std::uint32_t input_address = 0x4000, coefficient_address = 0x110000,
                        state_address = 0x111000, output_address = 0x120000;
constexpr std::size_t numeric_ram_size = output_address - input_address + (outputs * 4);
constexpr std::uint64_t quantum = 256, interval = 4096;
constexpr std::uint32_t multiply_base = 0xe0000090, mla_r3 = 0xe0233594;
constexpr std::int32_t scale = 32768, rounding_bias = 16383;
constexpr std::array<std::int32_t, 20> filter_coefficients{
    8192, 4096, 2048, -16384, 4096, 16384, -8192, 4096, 8192,   4096,
    4096, 8192, 4096, -24576, 8192, 16384, 8192,  4096, -16384, 8192};
constexpr std::array<std::int32_t, 8> initial_delays{
    32767, -32768, -32768, 32767, 24576, -16384, -24576, 16384};

std::vector<std::int32_t> numeric_input() {
  constexpr std::uint32_t seed = 0x243f6a88;
  constexpr std::uint32_t multiplier = 1664525;
  constexpr std::uint32_t increment = 1013904223;
  constexpr unsigned pulse_period = 97;
  std::vector<std::int32_t> result(outputs);
  auto random = seed;
  for (unsigned index = 0; index < outputs; ++index) {
    random = random * multiplier + increment;
    result[index] = (std::int32_t(random >> 16) - scale) / 2;
    if (index % pulse_period == 0) { result[index] = scale - 1; }
    if (index % pulse_period == 1) { result[index] = -scale; }
  }
  return result;
}

void require_digest(std::span<const std::int32_t> words, std::string_view expected) {
  llvm::SHA256 digest;
  for (const auto word : words) {
    const auto bits = std::uint32_t(word);
    const std::array<std::uint8_t, 4> bytes{std::uint8_t(bits),
                                            std::uint8_t(bits >> 8),
                                            std::uint8_t(bits >> 16),
                                            std::uint8_t(bits >> 24)};
    digest.update(bytes);
  }
  fixture::require(llvm::toHex(digest.final(), true) == expected,
                   "complete numerical result differs from contract v6 SHA-256");
}

void put_numeric(std::span<std::byte> ram,
                 std::uint32_t address,
                 std::span<const std::int32_t> words) {
  for (std::size_t index = 0; index < words.size(); ++index) {
    fixture::put(ram.subspan(address - input_address + (index * 4), 4),
                 std::uint32_t(words[index]));
  }
}

// Q1.15 nearest-even and saturation, using the caller's bias and one scratch.
// The fixture coefficients bound the signed accumulator and biased sum to int32.
void quantize_program(fixture::Program &code, unsigned value, unsigned scratch, unsigned bias) {
  code.word(fixture::data(13, false, scratch, 0, false, value | (15U << 7) | (1U << 5)));
  code.word(fixture::data(0, false, scratch, scratch, true, 1));
  code.word(fixture::data(4, false, value, value, false, scratch));
  code.word(fixture::data(4, false, value, value, false, bias));
  code.word(fixture::data(13, false, value, 0, false, value | (15U << 7) | (2U << 5)));
  constexpr unsigned rail = 0x902; // ARM immediate 32768.
  code.word(fixture::data(10, true, 0, value, true, rail));
  code.word(fixture::data(13, false, value, 0, true, rail, 10));
  code.word(fixture::data(2, false, value, value, true, 1, 10));
  code.word(fixture::data(11, true, 0, value, true, rail));
  code.word(fixture::data(13, false, value, 0, true, rail, 11));
  code.word(fixture::data(3, false, value, value, true, 0, 11));
}

fixture::Program fir_program() {
  fixture::Program code;
  constexpr unsigned frame_immediate = 0xb01;
  code.word(fixture::data(13, false, 6, 0, true, frame_immediate));
  const auto output = code.pc();
  code.word(fixture::data(13, false, 0, 0, false, 8));
  code.word(fixture::data(13, false, 1, 0, false, 9));
  code.word(fixture::data(13, false, 2, 0, true, taps));
  code.word(fixture::data(13, false, 3, 0, true, 0));
  const auto tap = code.pc();
  code.word(fixture::transfer(true, false, 4, 0, 4, false, false));
  code.word(fixture::transfer(true, false, 5, 1, 4, false));
  code.word(mla_r3);
  code.word(fixture::data(2, true, 2, 2, true, 1));
  code.word(fixture::branch(code.pc(), {.address = tap}, 1));
  quantize_program(code, 3, 11, 14);
  code.word(fixture::transfer(false, false, 3, 10, 4, false));
  code.word(fixture::data(4, false, 8, 8, true, 4));
  code.word(fixture::data(2, true, 6, 6, true, 1));
  code.word(fixture::branch(code.pc(), {.address = output}, 1));
  code.word(fixture::data(2, true, 7, 7, true, 1));
  code.word(fixture::branch(code.pc(), {.address = code.address()}, 1));
  code.word(fixture::branch(code.pc(), {.address = code.pc()}));
  return code;
}

void run_events(armv5::Core &core, std::uint64_t cycles) {
  std::uint64_t next_event = interval;
  for (auto bound = quantum;; bound += quantum) {
    const auto result = core.run_until(std::min(bound, cycles));
    fixture::require(result.execution == armv5::ExecutionState::ready, result.fault.reason);
    const auto now = core.state().progress.base_cycles;
    if (now >= cycles) { return; }
    if (now >= next_event) {
      fixture::require(core.interrupt_ready({}), "K6 missed interrupt boundary");
      core.interrupt({});
      next_event += interval;
    }
  }
}

void fir_kernel(Counters &counters, bool event_load) {
  const auto program = fir_program();
  fixture::Program handler(irq_vector);
  handler.word(fixture::data(4, false, 13, 13, true, 1)); // Banked IRQ SP counts events.
  handler.word(event_return);
  std::vector<fixture::ModeProgram> code{{.program = program, .modes = {supervisor}}};
  std::vector<std::uint32_t> vectors;
  if (event_load) {
    code.push_back({.program = handler, .modes = {irq_processor_mode}});
    vectors.push_back(irq_vector);
  }
  BenchmarkMachine machine(std::move(code), vectors, true, numeric_ram_size);
  const auto signal = numeric_input();
  std::vector<std::int32_t> input(taps - 1, 0);
  input.insert(input.end(), signal.begin(), signal.end());
  std::array<std::int32_t, taps> coefficients{};
  constexpr unsigned coefficient_step = 17;
  constexpr unsigned coefficient_period = 31;
  for (unsigned tap = 0; tap < taps; ++tap) {
    coefficients[tap] = (int(tap * coefficient_step % coefficient_period) - 15) * 32;
  }
  std::vector<std::int32_t> output(outputs);
  std::vector<std::int32_t> answer(outputs);
  benchmark::fir(input, coefficients, answer);
  constexpr std::string_view checksum =
      "01e342abb432c2b44339220d04d3376c55ec4ae30cb71ca783c7db9d3639c81b";
  require_digest(answer, checksum);
  put_numeric(machine.ram, input_address, input);
  put_numeric(machine.ram, coefficient_address, coefficients);
  const auto initial_ram = machine.ram;
  auto expected = initial_ram;
  put_numeric(expected, output_address, answer);
  constexpr std::uint64_t algorithm_guests = std::uint64_t(blocks) * (3 + frames * 339);
  constexpr std::uint64_t algorithm_cycles = (std::uint64_t(blocks) * (frames * 531 + 3)) - 2;
  std::uint64_t events = 0;
  while (event_load && events != (algorithm_cycles + 7 * events - 1) / interval) {
    events = (algorithm_cycles + 7 * events - 1) / interval;
  }
  const auto cycles = algorithm_cycles + (7 * events);
  const auto guests = algorithm_guests + (2 * events);
  auto initialize = [&] {
    std::ranges::copy(initial_ram, machine.ram.begin());
    machine.core->reset(program.address());
    auto registers = machine.core->registers();
    registers.cpsr = supervisor;
    registers.r[7] = blocks;
    registers.r[8] = input_address + (taps - 1) * 4;
    registers.r[9] = coefficient_address;
    registers.r[10] = output_address;
    registers.r[14] = rounding_bias;
    machine.registers(registers);
  };
  auto work = [&] {
    if (event_load) {
      run_events(*machine.core, cycles);
    } else {
      (void)machine.core->run(guests);
    }
  };
  auto expected_registers = machine.expected_registers();
  expected_registers.cpsr = fir_completed_status;
  expected_registers.r[0] = input_address + (outputs - 2) * 4;
  expected_registers.r[1] = coefficient_address + taps * 4;
  expected_registers.r[3] = std::uint32_t(answer.back());
  expected_registers.r[4] = std::uint32_t(input[outputs - 1]);
  expected_registers.r[5] = std::uint32_t(coefficients.back());
  expected_registers.r[8] = input_address + (outputs + taps - 1) * 4;
  expected_registers.r[9] = coefficient_address;
  expected_registers.r[10] = output_address + outputs * 4;
  std::int64_t last_sum = 0;
  for (unsigned tap = 0; tap < taps; ++tap) {
    last_sum += std::int64_t(input[input.size() - 1 - tap]) * coefficients[tap];
  }
  expected_registers.r[11] =
      unsigned((last_sum / scale) -
               static_cast<std::int64_t>(last_sum < 0 && last_sum % scale != 0)) &
      1;
  expected_registers.r[14] = rounding_bias;
  expected_registers.r[15] = program.pc() - 4;
  auto check = [&] {
    machine.verify_storage();
    const auto state = machine.core->state();
    fixture::require(machine.ram == expected && state.registers == expected_registers &&
                         state.banks.irq[0] == events &&
                         state.progress ==
                             armv5::Progress{.instructions = guests, .base_cycles = cycles},
                     "K1/K6 full Q1.15 output/registers/RAM, event count and progress");
  };
  initialize();
  work();
  check();
  std::vector<Measurement> native;
  std::vector<Measurement> reference;
  for (unsigned trial = 0; trial < 3; ++trial) {
    initialize();
    native.push_back(measure(counters, work));
    check();
  }
  for (unsigned trial = 0; trial < 3; ++trial) {
    reference.push_back(measure(counters, [&] { benchmark::fir(input, coefficients, output); }));
    fixture::require(output == answer, "K1/K6 C++ complete output");
  }
  report(event_load ? "K6" : "K1",
         guests,
         (program.bytes().size() + (event_load ? handler.bytes().size() : 0)) / 4,
         machine,
         native,
         reference,
         {.cycles = cycles, .samples = outputs});
}

fixture::Program biquad_program() {
  fixture::Program code;
  auto alu = [&](unsigned opcode,
                 unsigned destination,
                 unsigned source,
                 unsigned operand,
                 bool immediate = false,
                 bool flags = false) {
    code.word(fixture::data(opcode, flags, destination, source, immediate, operand));
  };
  auto mul = [&](unsigned destination, unsigned first, unsigned second) {
    code.word(multiply_base | (destination << 16) | (second << 8) | first);
  };
  auto load = [&](unsigned destination, unsigned base, unsigned offset = 4, bool post = true) {
    code.word(fixture::transfer(true, false, destination, base, offset, !post));
  };
  auto store = [&](unsigned source, unsigned base) {
    code.word(fixture::transfer(false, false, source, base, 4, false));
  };
  constexpr unsigned frame_immediate = 0xb01;
  alu(13, 2, 0, frame_immediate, true);
  const auto sample = code.pc();
  load(3, 0);
  alu(13, 4, 0, 8); // R8/R9 hold the immutable coefficient/state addresses between samples.
  alu(13, 5, 0, 9);
  alu(13, 6, 0, 4, true);
  const auto stage = code.pc();
  alu(13, 11, 0, 3);
  load(8, 4);
  mul(7, 3, 8);
  load(9, 5, 0, false);
  alu(4, 7, 7, 9 | (15U << 7));
  quantize_program(code, 7, 14, 12);
  load(8, 4);
  mul(10, 11, 8);
  load(8, 4, 4, false);
  mul(8, 7, 8);
  alu(2, 10, 10, 8);
  load(9, 5, 4, false);
  alu(4, 10, 10, 9 | (15U << 7));
  quantize_program(code, 10, 14, 12);
  store(10, 5);
  load(8, 4);
  mul(10, 11, 8);
  load(8, 4, 4, false);
  mul(8, 7, 8);
  alu(2, 10, 10, 8);
  quantize_program(code, 10, 14, 12);
  store(10, 5);
  alu(4, 4, 4, 8, true);
  alu(13, 3, 0, 7);
  alu(2, 6, 6, 1, true, true);
  code.word(fixture::branch(code.pc(), {.address = stage}, 1));
  store(3, 1);
  // Recover the two constant bases without reserving registers in the hot stage.
  alu(2, 8, 4, unsigned(filter_coefficients.size() * 4), true);
  alu(2, 9, 5, 32, true);
  alu(2, 2, 2, 1, true, true);
  code.word(fixture::branch(code.pc(), {.address = sample}, 1));
  alu(2, 13, 13, 1, true, true);
  code.word(fixture::branch(code.pc(), {.address = code.address()}, 1));
  code.word(fixture::branch(code.pc(), {.address = code.pc()}));
  return code;
}

void biquad_kernel(Counters &counters) {
  const auto program = biquad_program();
  BenchmarkMachine machine(program, numeric_ram_size);
  const auto input = numeric_input();
  std::vector<std::int32_t> output(outputs);
  std::vector<std::int32_t> answer(outputs);
  const auto oracle = benchmark::biquad(input, filter_coefficients, answer);
  require_digest(answer, "b598303fc1e7736b8ee0bc5175afe8baf38d6d9ef81f833f79724ab5a0a785ba");
  require_digest(oracle.delays, "c013f34a4563d1948b54c15f1d7ebc48337af2792c24968c43f1a98e1e063c35");
  put_numeric(machine.ram, input_address, input);
  put_numeric(machine.ram, coefficient_address, filter_coefficients);
  put_numeric(machine.ram, state_address, initial_delays);
  const auto initial_ram = machine.ram;
  auto expected_ram = initial_ram;
  put_numeric(expected_ram, output_address, answer);
  put_numeric(expected_ram, state_address, oracle.delays);
  machine.prepare();
  constexpr std::uint64_t guests = std::uint64_t(blocks) * (frames * 233 + 3);
  constexpr std::uint64_t cycles = (std::uint64_t(blocks) * (frames * 261 + 3)) - 2;
  auto expected = machine.expected_registers();
  constexpr std::uint32_t final_status = 0x600000d3;
  expected.cpsr = final_status;
  expected.r[0] = input_address + outputs * 4;
  expected.r[1] = output_address + outputs * 4;
  expected.r[3] = std::uint32_t(answer.back());
  expected.r[4] = coefficient_address + filter_coefficients.size() * 4;
  expected.r[5] = state_address + initial_delays.size() * 4;
  expected.r[7] = expected.r[3];
  expected.r[8] = coefficient_address;
  expected.r[9] = state_address;
  expected.r[10] = std::uint32_t(oracle.delays.back());
  expected.r[11] = std::uint32_t(oracle.last_input);
  expected.r[12] = rounding_bias;
  expected.r[14] = oracle.last_parity;
  expected.r[15] = program.pc() - 4;
  auto initialize = [&] {
    std::ranges::copy(initial_ram, machine.ram.begin());
    machine.core->reset(program.address());
    auto registers = machine.core->registers();
    registers.r[0] = input_address;
    registers.r[1] = output_address;
    registers.r[8] = coefficient_address;
    registers.r[9] = state_address;
    registers.r[12] = rounding_bias;
    registers.r[13] = blocks;
    machine.registers(registers);
  };
  auto work = [&] { (void)machine.core->run(guests); };
  auto check = [&] {
    machine.verify_storage();
    fixture::require(machine.core->registers() == expected && machine.ram == expected_ram &&
                         machine.core->state().progress ==
                             armv5::Progress{.instructions = guests, .base_cycles = cycles},
                     "K2 complete Q1.15 output, feedback states, registers, RAM and progress");
  };
  initialize();
  work();
  check();
  std::vector<Measurement> native;
  std::vector<Measurement> reference;
  for (unsigned trial = 0; trial < 3; ++trial) {
    initialize();
    native.push_back(measure(counters, work));
    check();
  }
  for (unsigned trial = 0; trial < 3; ++trial) {
    benchmark::BiquadState result;
    reference.push_back(
        measure(counters, [&] { result = benchmark::biquad(input, filter_coefficients, output); }));
    fixture::require(result == oracle && output == answer, "K2 C++ complete output and states");
  }
  report("K2",
         guests,
         program.bytes().size() / 4,
         machine,
         native,
         reference,
         {.cycles = cycles, .samples = outputs});
}

constexpr std::uint32_t fft_half_mask = 0xffff, fft_sign_extension = 0xffff0000;
constexpr unsigned fft_points = 256, fft_repetitions = 512;
constexpr std::uint32_t fft_input = 0x4000, fft_coefficients = 0x6000, fft_permutation = 0x7000,
                        fft_output = 0x8000;

fixture::Program fft_program() {
  fixture::Program code;
  auto alu = [&](unsigned opcode,
                 unsigned destination,
                 unsigned source,
                 bool immediate,
                 unsigned operand,
                 bool flags = false) {
    code.word(fixture::data(opcode, flags, destination, source, immediate, operand));
  };
  auto move = [&](unsigned destination, unsigned operand, bool immediate = false) {
    alu(13, destination, 0, immediate, operand);
  };
  auto shift = [&](unsigned destination, unsigned source, unsigned kind, unsigned amount) {
    move(destination, source | (kind << 5) | (amount << 7));
  };
  auto load = [&](unsigned destination, unsigned base, bool post = false) {
    code.word(fixture::transfer(true, false, destination, base, post ? 4 : 0, !post));
  };
  auto store = [&](unsigned source, unsigned base) {
    code.word(fixture::transfer(false, false, source, base, 4, false));
  };
  auto multiply = [&](unsigned destination, bool top_first, bool top_second) {
    constexpr std::uint32_t signed_half = 0xe1600080;
    code.word(signed_half | (destination << 16) | (10U << 8) | (unsigned(top_first) << 5) |
              (unsigned(top_second) << 6) | 7);
  };
  constexpr unsigned input_immediate = 0x901;
  constexpr unsigned output_immediate = 0x902;
  constexpr unsigned permutation_immediate = 0xa07;
  constexpr unsigned count_immediate = 0xc01;
  constexpr unsigned stride_immediate = 0xc02;
  constexpr unsigned coefficient_immediate = 0xa06;
  move(0, input_immediate, true);
  move(1, output_immediate, true);
  move(2, permutation_immediate, true);
  move(3, count_immediate, true);
  const auto copy = code.pc();
  load(6, 0, true);
  load(7, 2, true);
  code.word(fixture::transfer(false, false, 6, 1, 7, true, true, false, true));
  alu(2, 3, 3, true, 1, true);
  code.word(fixture::branch(code.pc(), {.address = copy}, 1));
  move(5, 8, true);
  move(12, 1, true);
  move(13, stride_immediate, true);
  const auto stage = code.pc();
  move(0, output_immediate, true);
  alu(4, 1, 0, false, 12 | (2U << 7));
  shift(4, 13, 1, 2);
  const auto group = code.pc();
  move(3, 12);
  move(2, coefficient_immediate, true);
  const auto butterfly = code.pc();
  load(6, 0);
  load(7, 1);
  load(10, 2);
  multiply(8, false, false);
  multiply(9, true, true);
  alu(2, 8, 8, false, 9);
  multiply(9, false, true);
  multiply(11, true, false);
  alu(4, 9, 9, false, 11);
  alu(4, 8, 8, true, input_immediate);
  shift(8, 8, 2, 15);
  alu(4, 9, 9, true, input_immediate);
  shift(9, 9, 2, 15);
  shift(10, 6, 0, 16);
  shift(10, 10, 2, 16);
  shift(11, 6, 2, 16);
  for (const unsigned opcode : {4U, 2U}) {
    alu(opcode, 6, 10, false, 8);
    shift(6, 6, 2, 1);
    alu(opcode, 7, 11, false, 9);
    shift(7, 7, 2, 1);
    shift(6, 6, 0, 16);
    shift(6, 6, 1, 16);
    alu(12, 6, 6, false, 7 | (16U << 7));
    store(6, opcode == 4 ? 0 : 1);
  }
  alu(4, 2, 2, false, 13);
  alu(2, 3, 3, true, 1, true);
  code.word(fixture::branch(code.pc(), {.address = butterfly}, 1));
  alu(4, 0, 0, false, 12 | (2U << 7));
  alu(4, 1, 1, false, 12 | (2U << 7));
  alu(2, 4, 4, true, 1, true);
  code.word(fixture::branch(code.pc(), {.address = group}, 1));
  alu(4, 12, 12, false, 12);
  shift(13, 13, 1, 1);
  alu(2, 5, 5, true, 1, true);
  code.word(fixture::branch(code.pc(), {.address = stage}, 1));
  alu(2, 14, 14, true, 1, true);
  code.word(fixture::branch(code.pc(), {.address = code.address()}, 1));
  code.word(fixture::branch(code.pc(), {.address = code.pc()}));
  return code;
}

void fft_kernel(Counters &counters) {
  const auto program = fft_program();
  BenchmarkMachine machine(program);
  std::array<std::uint32_t, fft_points> input{};
  std::array<std::uint32_t, fft_points> answer{};
  std::array<std::uint32_t, fft_points> output{};
  constexpr std::uint32_t input_mask = 4095;
  constexpr std::uint32_t real_multiplier = 97;
  constexpr std::uint32_t real_offset = 13;
  constexpr std::uint32_t imag_multiplier = 53;
  constexpr std::uint32_t imag_offset = 7;
  constexpr std::int32_t input_bias = 2048;
  for (unsigned index = 0; index < fft_points; ++index) {
    const auto real =
        std::int32_t((index * real_multiplier + real_offset) & input_mask) - input_bias;
    const auto imag =
        std::int32_t((index * imag_multiplier + imag_offset) & input_mask) - input_bias;
    input[index] = (std::uint32_t(real) & fft_half_mask) | (std::uint32_t(imag) << 16);
    fixture::put(std::span(machine.ram).subspan(std::size_t(index) * 4, 4), input[index]);
    unsigned reverse = 0;
    for (unsigned bit = 0; bit < 8; ++bit) { reverse |= ((index >> bit) & 1) << (7 - bit); }
    fixture::put(std::span(machine.ram).subspan(fft_permutation - fft_input + (index * 4), 4),
                 reverse * 4);
  }
  for (std::size_t index = 0; index < benchmark::fft_twiddles.size(); ++index) {
    fixture::put(std::span(machine.ram).subspan(fft_coefficients - fft_input + (index * 4), 4),
                 benchmark::fft_twiddles[index]);
  }
  const auto oracle = benchmark::fft(input.data(), benchmark::fft_twiddles, answer.data(), 1);
  const auto initial_ram = machine.ram;
  auto expected_ram = initial_ram;
  for (std::size_t index = 0; index < answer.size(); ++index) {
    fixture::put(std::span(expected_ram).subspan(fft_output - fft_input + (index * 4), 4),
                 answer[index]);
  }
  machine.prepare();
  constexpr std::uint64_t guests = std::uint64_t(fft_repetitions) * 38715;
  constexpr std::uint64_t cycles = (std::uint64_t(fft_repetitions) * 41273) - 2;
  auto expected = machine.expected_registers();
  constexpr std::uint32_t final_status = 0x600000d3;
  expected.cpsr = final_status;
  expected.r[0] = fft_output + fft_points * 4;
  expected.r[1] = fft_output + (fft_points + fft_points / 2) * 4;
  expected.r[2] = fft_coefficients + fft_points * 2;
  expected.r[6] = answer.back();
  const auto high = answer.back() >> 16;
  expected.r[7] = high | ((high & 0x8000) != 0 ? fft_sign_extension : 0);
  expected.r[8] = std::uint32_t(oracle.temporary_real);
  expected.r[9] = std::uint32_t(oracle.temporary_imaginary);
  expected.r[10] = std::uint32_t(oracle.old_real);
  expected.r[11] = std::uint32_t(oracle.old_imaginary);
  expected.r[12] = fft_points;
  expected.r[13] = 2;
  expected.r[15] = program.pc() - 4;
  auto initialize = [&] {
    std::ranges::copy(initial_ram, machine.ram.begin());
    machine.core->reset(program.address());
    auto registers = machine.core->registers();
    registers.r[14] = fft_repetitions;
    machine.registers(registers);
  };
  auto work = [&] { (void)machine.core->run(guests); };
  auto check = [&] {
    machine.verify_storage();
    fixture::require(machine.core->registers() == expected && machine.ram == expected_ram &&
                         machine.core->state().progress ==
                             armv5::Progress{.instructions = guests, .base_cycles = cycles},
                     "K3 complete FFT output, registers, RAM and progress");
  };
  initialize();
  work();
  check();
  std::vector<Measurement> native;
  std::vector<Measurement> reference;
  for (unsigned trial = 0; trial < 3; ++trial) {
    initialize();
    native.push_back(measure(counters, work));
    check();
  }
  for (unsigned trial = 0; trial < 3; ++trial) {
    benchmark::FftResult result;
    reference.push_back(measure(counters, [&] {
      result =
          benchmark::fft(input.data(), benchmark::fft_twiddles, output.data(), fft_repetitions);
    }));
    fixture::require(result == oracle && output == answer, "K3 C++ complete FFT output");
  }
  report("K3",
         guests,
         program.bytes().size() / 4,
         machine,
         native,
         reference,
         {.cycles = cycles, .samples = std::uint64_t(fft_points) * fft_repetitions});
}

}

int main(int argc, char **argv) {
  try {
    const std::string_view mapping = argc > 2 ? argv[2] : "off";
    const std::string_view memory = argc > 3 ? argv[3] : "direct";
    fixture::require(argc <= 4 &&
                         (mapping == "off" || mapping == "identity" || mapping == "remapped") &&
                         (memory == "direct" || memory == "exact"),
                     "unknown benchmark memory declaration");
    if (mapping == "identity") { benchmark::configuration.mapping = benchmark::Mapping::identity; }
    if (mapping == "remapped") { benchmark::configuration.mapping = benchmark::Mapping::remapped; }
    if (memory == "exact") {
      benchmark::configuration.memory = armv5::CompileOptions::MemoryMode::exact;
    }
    Counters counters;
    std::cout << std::setprecision(9)
              << "kernel,guest_instructions,host_instructions_per_guest,host_cycles_per_guest,"
                 "guest_per_second,native_over_clang_O3_seconds,preparation_seconds,native_text_"
                 "bytes,text_bytes_per_prepared_instruction,guest_instructions_per_sample,"
                 "guest_cycles_per_sample,ns_per_guest,native_guest_share,memory_mode,mapping,"
                 "installation_seconds\n";
    const std::string_view selected = argc > 1 ? argv[1] : "all";
    fixture::require(selected == "all" || selected == "K1" || selected == "K2" ||
                         selected == "K3" || selected == "K4" || selected == "K5" ||
                         selected == "K6",
                     "unknown benchmark kernel");
    if (selected == "all" || selected == "K5") { copy_kernel(counters); }
    if (selected == "all" || selected == "K4") { control_kernel(counters); }
    if (selected == "all" || selected == "K6") { fir_kernel(counters, true); }
    if (selected == "all" || selected == "K1") { fir_kernel(counters, false); }
    if (selected == "all" || selected == "K2") { biquad_kernel(counters); }
    if (selected == "all" || selected == "K3") { fft_kernel(counters); }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
