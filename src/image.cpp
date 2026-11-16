// SPDX-License-Identifier: GPL-3.0-only
#include "image.hpp"

#include "armv5/core.hpp"
#include "identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace armv5::detail {
namespace {
using Bytes = std::vector<std::byte>;
constexpr unsigned byte_bits = 8;
constexpr std::uint32_t byte_mask = 255;

struct Writer {
  Bytes bytes;

  void number(std::uint64_t value) {
    for (unsigned index = 0; index < sizeof(value); ++index) {
      bytes.push_back(std::byte((value >> (index * byte_bits)) & byte_mask));
    }
  }

  void blob(std::span<const std::byte> data) {
    number(data.size());
    bytes.insert(bytes.end(), data.begin(), data.end());
  }

  void text(std::string_view value) { blob(std::as_bytes(std::span(value))); }
};

struct Reader {
  std::span<const std::byte> bytes;

  std::uint64_t number() {
    if (bytes.size() < sizeof(std::uint64_t)) {
      throw std::runtime_error("truncated artifact integer");
    }
    std::uint64_t result = 0;
    for (unsigned index = 0; index < sizeof(result); ++index) {
      result |= std::to_integer<std::uint64_t>(bytes[index]) << (index * byte_bits);
    }
    bytes = bytes.subspan(sizeof(result));
    return result;
  }

  std::span<const std::byte> blob() {
    const auto size = number();
    if (size > bytes.size()) { throw std::runtime_error("truncated artifact payload"); }
    const auto result = bytes.first(static_cast<std::size_t>(size));
    bytes = bytes.subspan(static_cast<std::size_t>(size));
    return result;
  }

  std::string text() {
    const auto data = blob();
    return {reinterpret_cast<const char *>(data.data()), data.size()};
  }
};

Bytes digest(std::span<const std::byte> bytes) {
  const auto hash =
      llvm::SHA256::hash({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
  const auto view = std::as_bytes(std::span(hash));
  return {view.begin(), view.end()};
}

Bytes input_bytes(const CodeImage &input, const CompileOptions &options) {
  Writer out;
  out.text(compiler_identity);
  out.text(input.identity);
  out.number(static_cast<unsigned>(options.instruction_set));
  out.number(static_cast<std::uint64_t>(options.big_endian));
  out.number(options.processor_mode);
  out.number(options.callbacks.size());
  for (const auto &callback : options.callbacks) {
    out.number(static_cast<unsigned>(callback.role));
    out.text(callback.symbol);
  }
  out.number(input.variants.size());
  for (const auto &variant : input.variants) {
    out.number(variant.id);
    out.number(variant.spans.size());
    for (const auto &span : variant.spans) {
      out.number(span.address);
      out.blob(span.bytes);
    }
    out.number(variant.entries.size());
    for (const auto entry : variant.entries) { out.number(entry); }
  }
  out.number(input.layouts.size());
  for (const auto &layout : input.layouts) {
    out.number(layout.variant);
    out.number(layout.regions.size());
    for (const auto &region : layout.regions) {
      out.number(region.address);
      out.number(region.size);
      out.number(static_cast<std::uint64_t>(region.readable));
      out.number(static_cast<std::uint64_t>(region.writable));
      out.number(static_cast<std::uint64_t>(region.executable));
    }
  }
  out.number(input.transitions.size());
  for (const auto &transfer : input.transitions) {
    out.number(transfer.source_variant);
    out.number(transfer.destination_variant);
    out.number(transfer.address);
  }
  out.number(input.interrupt_vectors.size());
  for (const auto vector : input.interrupt_vectors) { out.number(vector); }
  return std::move(out.bytes);
}

void validate_variant(std::string_view identity, const Variant &variant) {
  if (variant.spans.empty() || variant.entries.empty()) {
    refuse(identity, 0, "variant needs code and entries");
  }
  std::uint64_t end = 0;
  for (const auto &span : variant.spans) {
    if (span.bytes.empty() || (span.address & 3) != 0 || (span.bytes.size() & 3) != 0) {
      refuse(identity, span.address, "ARM executable span must contain aligned complete words");
    }
    if (span.bytes.size() > (std::uint64_t{1} << 32) - span.address) {
      refuse(identity, span.address, "executable span crosses the address-space boundary");
    }
    if (span.address < end) { refuse(identity, span.address, "overlapping executable spans"); }
    end = span.address + span.bytes.size();
  }
  for (const auto entry : variant.entries) {
    if ((entry & 3) != 0) { refuse(identity, entry, "unaligned ARM entry"); }
    if (std::ranges::none_of(variant.spans, [entry](const Span &span) {
          return entry >= span.address && entry - span.address < span.bytes.size();
        })) {
      refuse(identity, entry, "entry is outside declared executable spans");
    }
  }
  end = 0;
  for (const auto &region : variant.layout) {
    if (region.size == 0 || region.size > (std::uint64_t{1} << 32) - region.address ||
        region.address < end) {
      refuse(identity, region.address, "invalid or overlapping fixed memory layout");
    }
    end = region.address + region.size;
  }
}

void validate_callbacks(std::string_view identity, const CompileOptions &options) {
  for (std::size_t index = 0; index < options.callbacks.size(); ++index) {
    const auto &callback = options.callbacks[index];
    if (callback.symbol.empty() ||
        static_cast<unsigned>(callback.role) > static_cast<unsigned>(CallbackBinding::Role::swap)) {
      refuse(identity, 0, "invalid callback binding");
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      const auto &other = options.callbacks[prior];
      if (callback.role == other.role) { refuse(identity, 0, "duplicate callback role"); }
      const bool write = callback.role == CallbackBinding::Role::write;
      const bool other_write = other.role == CallbackBinding::Role::write;
      if (callback.symbol == other.symbol && write != other_write) {
        refuse(identity, 0, "callback symbol has incompatible role ABIs");
      }
    }
  }
}

void validate_links(const CodeImage &input, const Image &image) {
  for (const auto &layout : input.layouts) {
    if (std::ranges::find(image.variants, layout.variant, &Variant::id) == image.variants.end()) {
      refuse(image.identity, 0, "layout names an undeclared variant");
    }
  }
  for (std::size_t index = 0; index < image.transitions.size(); ++index) {
    const auto &transition = image.transitions[index];
    const auto source = std::ranges::find(image.variants, transition.source_variant, &Variant::id);
    const auto destination =
        std::ranges::find(image.variants, transition.destination_variant, &Variant::id);
    if (source == image.variants.end() || destination == image.variants.end()) {
      refuse(image.identity, transition.address, "transition names an undeclared variant");
    }
    if (std::ranges::find(destination->entries, transition.address) == destination->entries.end()) {
      refuse(image.identity,
             transition.address,
             "transition target is not a declared destination entry");
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      const auto &other = image.transitions[prior];
      if (other.source_variant == transition.source_variant &&
          other.address == transition.address) {
        refuse(image.identity, transition.address, "ambiguous code transition");
      }
    }
  }
}

} // namespace

[[noreturn]] void
refuse(std::string_view identity, std::uint32_t address, std::string_view reason) {
  std::ostringstream message;
  message << identity << " at 0x" << std::hex << std::setw(8) << std::setfill('0') << address
          << ": " << reason;
  throw std::runtime_error(message.str());
}

Image describe(const CodeImage &input, const CompileOptions &options) {
  if (input.identity.empty()) { refuse(input.identity, 0, "code identity is empty"); }
  if (input.variants.empty()) { refuse(input.identity, 0, "no declared code variants"); }
  if (options.instruction_set != CompileOptions::InstructionSet::arm) {
    refuse(input.identity, 0, "Thumb is not lowered yet (A4)");
  }
  constexpr std::array modes{0x10U, 0x11U, 0x12U, 0x13U, 0x17U, 0x1bU, 0x1fU};
  if (std::ranges::find(modes, options.processor_mode) == modes.end()) {
    refuse(input.identity, 0, "invalid prepared processor mode");
  }
  validate_callbacks(input.identity, options);
  Image result;
  result.identity = input.identity;
  result.options = options;
  std::ostringstream key;
  for (const auto byte : digest(input_bytes(input, options))) {
    key << std::hex << std::setw(2) << std::setfill('0') << std::to_integer<unsigned>(byte);
  }
  result.key = key.str();
  result.transitions.assign(input.transitions.begin(), input.transitions.end());
  result.vectors.assign(input.interrupt_vectors.begin(), input.interrupt_vectors.end());
  for (const auto &variant : input.variants) {
    if (std::ranges::any_of(result.variants,
                            [&variant](const Variant &other) { return other.id == variant.id; })) {
      refuse(input.identity, 0, "duplicate code variant id");
    }
    Variant copy{.id = variant.id,
                 .spans = {},
                 .entries = {variant.entries.begin(), variant.entries.end()},
                 .layout = {}};
    for (const auto &span : variant.spans) {
      copy.spans.push_back(
          {.address = span.address, .bytes = {span.bytes.begin(), span.bytes.end()}});
    }
    std::ranges::sort(copy.spans, {}, &Span::address);
    bool found_layout = false;
    for (const auto &layout : input.layouts) {
      if (layout.variant != variant.id) { continue; }
      if (found_layout) { refuse(input.identity, 0, "duplicate variant layout"); }
      found_layout = true;
      copy.layout.assign(layout.regions.begin(), layout.regions.end());
    }
    std::ranges::sort(copy.layout, {}, &RegionLayout::address);
    validate_variant(input.identity, copy);
    result.variants.push_back(std::move(copy));
  }
  validate_links(input, result);
  return result;
}

void pack(Image &image, const CodeImage &input) {
  Writer payload;
  payload.blob(input_bytes(input, image.options));
  payload.blob(image.bitcode);
  payload.number(image.exports.size());
  for (const auto &symbol : image.exports) { payload.text(symbol); }
  Writer out;
  out.text("armv5-artifact-v2");
  out.text(compiler_identity);
  out.blob(digest(payload.bytes));
  out.blob(payload.bytes);
  image.artifact = std::move(out.bytes);
}

Image unpack(const CodeImage &input,
             const CompileOptions &options,
             std::span<const std::byte> artifact) {
  try {
    Reader reader{.bytes = artifact};
    if (reader.text() != "armv5-artifact-v2" || reader.text() != compiler_identity) {
      throw std::runtime_error("artifact compiler identity mismatch");
    }
    const auto checksum = reader.blob();
    const auto payload = reader.blob();
    if (!reader.bytes.empty() || !std::ranges::equal(checksum, digest(payload))) {
      throw std::runtime_error("artifact checksum mismatch");
    }
    Reader data{.bytes = payload};
    if (!std::ranges::equal(data.blob(), input_bytes(input, options))) {
      throw std::runtime_error("artifact code/options digest mismatch");
    }
    auto image = describe(input, options); // Copies declarations; never decodes or lowers.
    const auto bitcode = data.blob();
    image.bitcode.assign(bitcode.begin(), bitcode.end());
    const auto count = data.number();
    if (count != image.variants.size()) {
      throw std::runtime_error("artifact export count mismatch");
    }
    for (std::uint64_t index = 0; index < count; ++index) { image.exports.push_back(data.text()); }
    if (!data.bytes.empty() || image.bitcode.empty()) {
      throw std::runtime_error("invalid artifact payload");
    }
    image.artifact.assign(artifact.begin(), artifact.end());
    return image;
  } catch (const std::runtime_error &error) { refuse(input.identity, 0, error.what()); }
}
} // namespace armv5::detail
