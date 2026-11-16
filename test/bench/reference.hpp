// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>

namespace benchmark {
struct Control {
  std::uint32_t state, accumulator, last_handler;
};

Control control(std::uint32_t iterations, Control initial);

struct FirShape {
  std::size_t outputs, taps;
  unsigned repetitions;
};

void fir(const std::uint32_t *input,
         const std::uint32_t *coefficients,
         std::uint32_t *output,
         FirShape shape);

struct CopyShape {
  std::size_t words;
  unsigned repetitions;
};

std::uint32_t copy(const std::uint32_t *source, std::uint32_t *destination, CopyShape shape);
}
