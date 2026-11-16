// SPDX-License-Identifier: GPL-3.0-only
#include "reference.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace benchmark {
Control control(std::uint32_t iterations, Control initial) {
  auto state = initial.state;
  auto accumulator = initial.accumulator;
  std::uint32_t last = 0;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    state &= 7;
    last = state;
    accumulator += state;
    accumulator ^= state + 1;
    state = (state + 3) & 7;
    accumulator ^= std::rotr(state, 1);
    accumulator += 3;
  }
  return {.state = state, .accumulator = accumulator, .last_handler = last};
}

std::uint32_t copy(const std::uint32_t *source, std::uint32_t *destination, CopyShape shape) {
  std::uint32_t checksum = 0;
  for (unsigned pass = 0; pass < shape.repetitions; ++pass) {
    for (std::size_t index = 0; index < shape.words; ++index) {
      const auto value = source[index];
      destination[index] = value;
      checksum += value;
    }
  }
  return checksum;
}

void fir(const std::uint32_t *input,
         const std::uint32_t *coefficients,
         std::uint32_t *output,
         FirShape shape) {
  for (unsigned repetition = 0; repetition < shape.repetitions; ++repetition) {
    for (std::size_t sample = 0; sample < shape.outputs; ++sample) {
      std::uint32_t sum = 0;
      for (std::size_t tap = 0; tap < shape.taps; ++tap) {
        sum += input[sample + tap] * coefficients[tap];
      }
      output[sample] = sum;
    }
  }
}
}
