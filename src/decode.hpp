// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string_view>

namespace armv5::detail {

// Preparation-only classification; the caller retains the original word.
[[nodiscard]] std::string_view decode_arm(std::uint32_t word);

} // namespace armv5::detail
