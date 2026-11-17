// SPDX-License-Identifier: GPL-3.0-only
#include "image.hpp"

#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "identity.hpp"
#include "system.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/Support/SHA256.h>
#include <set>
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
    out.number(static_cast<std::uint64_t>(variant.mapping.has_value()));
    if (variant.mapping) {
      const auto &mapping = *variant.mapping;
      out.number(mapping.control);
      out.number(mapping.translation_table);
      out.number(mapping.domains);
      out.number(mapping.process_id);
      out.number(mapping.instruction_tcm);
      out.number(mapping.data_tcm);
    }
    out.number(variant.spans.size());
    for (const auto &span : variant.spans) {
      out.number(span.address);
      out.blob(span.bytes);
      out.number(static_cast<unsigned>(span.instruction_set));
      out.number(span.processor_modes.size());
      for (const auto mode : span.processor_modes) { out.number(mode); }
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
      out.number(static_cast<unsigned>(region.space));
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

void validate_overlap(std::string_view identity, const Span &first, const Span &second) {
  const auto begin = std::max(first.address, second.address);
  const auto end =
      std::min(first.address + first.bytes.size(), second.address + second.bytes.size());
  if (end <= begin) { return; }
  if (first.thumb == second.thumb && std::ranges::any_of(first.modes, [&](std::uint32_t mode) {
        return std::ranges::find(second.modes, mode) != second.modes.end();
      })) {
    refuse(identity, begin, "overlapping executable spans in the same mode/state");
  }
  const auto size = end - begin;
  if (!std::ranges::equal(std::span(first.bytes).subspan(begin - first.address, size),
                          std::span(second.bytes).subspan(begin - second.address, size))) {
    refuse(identity, begin, "overlapping executable declarations contain different bytes");
  }
}

void validate_variant(std::string_view identity, const Variant &variant) {
  if (variant.spans.empty() || variant.entries.empty()) {
    refuse(identity, 0, "variant needs code and entries");
  }
  for (std::size_t index = 0; index < variant.spans.size(); ++index) {
    const auto &span = variant.spans[index];
    const auto alignment = span.thumb ? 1U : 3U;
    if (span.bytes.empty() || (span.address & alignment) != 0 ||
        (span.bytes.size() & alignment) != 0) {
      refuse(identity, span.address, "executable span must contain aligned complete instructions");
    }
    if (span.bytes.size() > (std::uint64_t{1} << 32) - span.address) {
      refuse(identity, span.address, "executable span crosses the address-space boundary");
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      validate_overlap(identity, variant.spans[previous], span);
    }
  }
  for (const auto entry : variant.entries) {
    if (std::ranges::none_of(variant.spans, [entry](const Span &span) {
          return entry >= span.address && entry - span.address < span.bytes.size() &&
                 (entry & (span.thumb ? 1U : 3U)) == 0;
        })) {
      refuse(identity, entry, "entry is outside declared executable spans or is unaligned");
    }
  }
  std::uint64_t end = 0;
  auto space = Memory::Space::bus;
  for (const auto &region : variant.layout) {
    if (region.space != space) { end = 0; }
    space = region.space;
    if (region.size == 0 || region.size > (std::uint64_t{1} << 32) - region.address ||
        region.address < end ||
        static_cast<unsigned>(space) > static_cast<unsigned>(Memory::Space::dtcm)) {
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

Span copy_span(std::string_view identity, const CodeSpan &span, const CompileOptions &options) {
  auto modes = std::vector<std::uint32_t>(span.processor_modes.begin(), span.processor_modes.end());
  if (modes.empty()) { modes.push_back(options.processor_mode); }
  std::ranges::sort(modes);
  for (std::size_t index = 0; index < modes.size(); ++index) {
    if (!valid_mode(modes[index]) || (index != 0 && modes[index] == modes[index - 1])) {
      refuse(identity, span.address, "invalid or duplicate span processor mode");
    }
  }
  if (static_cast<unsigned>(span.instruction_set) >
      static_cast<unsigned>(CodeSpan::InstructionSet::thumb)) {
    refuse(identity, span.address, "invalid span instruction set");
  }
  const bool thumb = span.instruction_set == CodeSpan::InstructionSet::thumb ||
                     (span.instruction_set == CodeSpan::InstructionSet::inherit &&
                      options.instruction_set == CompileOptions::InstructionSet::thumb);
  return {.address = span.address,
          .bytes = {span.bytes.begin(), span.bytes.end()},
          .modes = std::move(modes),
          .thumb = thumb};
}

Registers::System::Mapping prepared_mapping(std::string_view identity,
                                            const CodeVariant &variant,
                                            const CompileOptions &options) {
  auto mapping = variant.mapping.value_or(Registers::System::Mapping{});
  if (!variant.mapping && options.big_endian) { mapping.control |= 0x80; }
  if (!valid_tcms(mapping)) { refuse(identity, 0, "unpredictable TCM geometry/base/overlap"); }
  return mapping;
}

bool ambiguous_transition(const Image &image,
                          const CodeTransition &current,
                          const CodeTransition &previous) {
  if (current.source_variant != previous.source_variant || current.address != previous.address) {
    return false;
  }
  const auto first = std::ranges::find(image.variants, current.destination_variant, &Variant::id);
  const auto second = std::ranges::find(image.variants, previous.destination_variant, &Variant::id);
  return first != image.variants.end() && second != image.variants.end() &&
         same_mapping(first->mapping, second->mapping);
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
      if (ambiguous_transition(image, transition, other)) {
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

bool valid_mode(std::uint32_t mode) {
  constexpr std::array modes{
      user_mode, fiq_mode, irq_mode, supervisor_mode, abort_mode, undefined_mode, system_mode};
  return std::ranges::find(modes, mode) != modes.end();
}

Image describe(const CodeImage &input, const CompileOptions &options) {
  if (input.identity.empty()) { refuse(input.identity, 0, "code identity is empty"); }
  if (input.variants.empty()) { refuse(input.identity, 0, "no declared code variants"); }
  if (static_cast<unsigned>(options.instruction_set) >
      static_cast<unsigned>(CompileOptions::InstructionSet::thumb)) {
    refuse(input.identity, 0, "invalid prepared instruction set");
  }
  if (!valid_mode(options.processor_mode)) {
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
                 .layout = {},
                 .mapping = prepared_mapping(input.identity, variant, options)};
    for (const auto &span : variant.spans) {
      copy.spans.push_back(copy_span(input.identity, span, options));
    }
    std::ranges::sort(copy.spans, {}, &Span::address);
    bool found_layout = false;
    for (const auto &layout : input.layouts) {
      if (layout.variant != variant.id) { continue; }
      if (found_layout) { refuse(input.identity, 0, "duplicate variant layout"); }
      found_layout = true;
      copy.layout.assign(layout.regions.begin(), layout.regions.end());
    }
    std::ranges::sort(copy.layout, [](const RegionLayout &first, const RegionLayout &second) {
      return std::pair{first.space, first.address} < std::pair{second.space, second.address};
    });
    validate_variant(input.identity, copy);
    std::set<std::pair<std::uint32_t, bool>> modes;
    for (const auto &span : copy.spans) {
      for (const auto mode : span.modes) { modes.emplace(mode, span.thumb); }
    }
    for (const auto &[mode, thumb] : modes) {
      result.units.push_back({result.variants.size(), mode, thumb});
    }
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
  out.text("armv5-artifact-v5");
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
    if (reader.text() != "armv5-artifact-v5" || reader.text() != compiler_identity) {
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
    if (count != image.units.size()) { throw std::runtime_error("artifact export count mismatch"); }
    for (std::uint64_t index = 0; index < count; ++index) { image.exports.push_back(data.text()); }
    if (!data.bytes.empty() || image.bitcode.empty()) {
      throw std::runtime_error("invalid artifact payload");
    }
    image.artifact.assign(artifact.begin(), artifact.end());
    return image;
  } catch (const std::runtime_error &error) { refuse(input.identity, 0, error.what()); }
}
} // namespace armv5::detail
