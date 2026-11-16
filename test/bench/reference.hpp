// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace benchmark {
inline constexpr std::size_t biquad_coefficient_count = std::size_t{4} * 5;

struct Control {
  std::uint32_t state, accumulator, last_handler;
};

Control control(std::uint32_t iterations, Control initial);

// Independent portable Q1.15 algorithms; their complete results must also match
// the common contract's SHA-256 values. No shared generator is compiled here.
std::int32_t quantize(std::int64_t value);
void fir(std::span<const std::int32_t> input,
         std::span<const std::int32_t, 64> coefficients,
         std::span<std::int32_t> output);

struct BiquadState {
  std::array<std::int32_t, 8> delays{};
  std::int32_t last_input = 0;
  unsigned last_parity = 0;
  bool operator==(const BiquadState &) const = default;
};

BiquadState biquad(std::span<const std::int32_t> input,
                   std::span<const std::int32_t, biquad_coefficient_count> coefficients,
                   std::span<std::int32_t> output);

struct FftResult {
  std::int32_t temporary_real = 0, temporary_imaginary = 0, old_real = 0, old_imaginary = 0;
  bool operator==(const FftResult &) const = default;
};

FftResult fft(const std::uint32_t *input,
              std::span<const std::uint32_t> twiddles,
              std::uint32_t *output,
              unsigned repetitions);

struct CopyShape {
  std::size_t words;
  unsigned repetitions;
};

std::uint32_t copy(const std::uint32_t *source, std::uint32_t *destination, CopyShape shape);
}
