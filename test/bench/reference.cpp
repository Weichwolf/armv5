// SPDX-License-Identifier: GPL-3.0-only
#include "reference.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

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

std::int32_t quantize(std::int64_t value) {
  constexpr std::int64_t scale = 32768;
  // Round the magnitude, then restore its sign. This differs from the guest's
  // arithmetic shift plus parity-dependent bias, including negative ties.
  const auto magnitude = value < 0 ? -value : value;
  auto whole = magnitude / scale;
  const auto fraction = magnitude % scale;
  if (fraction > scale / 2 || (fraction == scale / 2 && whole % 2 != 0)) { ++whole; }
  return std::int32_t(std::clamp(value < 0 ? -whole : whole, -scale, scale - 1));
}

void fir(std::span<const std::int32_t> input,
         std::span<const std::int32_t, 64> coefficients,
         std::span<std::int32_t> output) {
  for (std::size_t sample = 0; sample < output.size(); ++sample) {
    std::int64_t accumulator = 0;
    for (std::size_t tap = 0; tap < coefficients.size(); ++tap) {
      accumulator +=
          std::int64_t(input[sample + coefficients.size() - 1 - tap]) * coefficients[tap];
    }
    output[sample] = quantize(accumulator);
  }
}

BiquadState biquad(std::span<const std::int32_t> input,
                   std::span<const std::int32_t, biquad_coefficient_count> coefficients,
                   std::span<std::int32_t> output) {
  constexpr std::int64_t scale = 32768;
  BiquadState result{.delays = {scale - 1,
                                -scale,
                                -scale,
                                scale - 1,
                                3 * scale / 4,
                                -scale / 2,
                                -3 * scale / 4,
                                scale / 2}};
  for (std::size_t sample = 0; sample < input.size(); ++sample) {
    auto value = input[sample];
    for (std::size_t stage = 0; stage < result.delays.size() / 2; ++stage) {
      const auto *c = coefficients.data() + (stage * 5);
      auto *state = result.delays.data() + (stage * 2);
      const auto next = quantize((state[0] * scale) + (std::int64_t(c[0]) * value));
      const auto first =
          (state[1] * scale) + (std::int64_t(c[1]) * value) - (std::int64_t(c[3]) * next);
      const auto second = (std::int64_t(c[2]) * value) - (std::int64_t(c[4]) * next);
      result.last_input = value;
      result.last_parity = unsigned((second / scale) -
                                    static_cast<std::int64_t>(second < 0 && second % scale != 0)) &
                           1;
      state[0] = quantize(first);
      state[1] = quantize(second);
      value = next;
    }
    output[sample] = value;
  }
  return result;
}

FftResult fft(const std::uint32_t *input,
              std::span<const std::uint32_t> twiddles,
              std::uint32_t *output,
              unsigned repetitions) {
  constexpr unsigned count = 256;
  constexpr unsigned half_bits = 16;
  constexpr std::uint32_t half_mask = 0xffff;
  constexpr std::int64_t scale = 32768;
  constexpr std::int64_t rounding = scale / 2;
  auto component = [](std::uint32_t word, unsigned offset) {
    const auto bits = (word >> offset) & half_mask;
    return std::int32_t(bits) - ((bits & 0x8000) != 0 ? 0x10000 : 0);
  };
  auto floor_divide = [](std::int64_t value, std::int64_t divisor) {
    return std::int32_t((value / divisor) - (value < 0 && value % divisor != 0 ? 1 : 0));
  };
  auto pack = [](std::int32_t real, std::int32_t imag) {
    return (std::uint32_t(real) & half_mask) | (std::uint32_t(imag) << half_bits);
  };
  FftResult result;
  for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
    for (unsigned i = 0; i < count; ++i) {
      unsigned reverse = 0;
      for (unsigned bit = 0; bit < 8; ++bit) { reverse = (reverse << 1) | ((i >> bit) & 1); }
      output[reverse] = input[i];
    }
    for (unsigned size = 2; size <= count; size *= 2) {
      for (unsigned group = 0; group < count; group += size) {
        for (unsigned j = 0; j < size / 2; ++j) {
          const auto upper = output[group + j];
          const auto lower = output[group + j + (size / 2)];
          const auto coefficient = twiddles[j * count / size];
          const auto real =
              (std::int64_t(component(lower, 0)) * component(coefficient, 0)) -
              (std::int64_t(component(lower, half_bits)) * component(coefficient, half_bits));
          const auto imag =
              (std::int64_t(component(lower, 0)) * component(coefficient, half_bits)) +
              (std::int64_t(component(lower, half_bits)) * component(coefficient, 0));
          result = {.temporary_real = floor_divide(real + rounding, scale),
                    .temporary_imaginary = floor_divide(imag + rounding, scale),
                    .old_real = component(upper, 0),
                    .old_imaginary = component(upper, half_bits)};
          output[group + j] =
              pack(floor_divide(result.old_real + result.temporary_real, 2),
                   floor_divide(result.old_imaginary + result.temporary_imaginary, 2));
          output[group + j + (size / 2)] =
              pack(floor_divide(result.old_real - result.temporary_real, 2),
                   floor_divide(result.old_imaginary - result.temporary_imaginary, 2));
        }
      }
    }
  }
  return result;
}

}
