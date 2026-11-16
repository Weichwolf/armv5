// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string_view>

namespace armv5::detail {

// Decoding is preparation-only. A name identifies a semantic operation, not an
// execution helper. Register fields remain in the original instruction word.
struct Decoded {
  std::uint32_t word;
  std::string_view operation;
};

[[nodiscard]] Decoded decode_arm(std::uint32_t word);

} // namespace armv5::detail
