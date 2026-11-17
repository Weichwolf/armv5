// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "image.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/DebugInfo/DWARF/DWARFDebugLine.h>
#include <llvm/DebugInfo/DWARF/DWARFUnit.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Triple.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace armv5::detail {
namespace {
template <class Value> Value unwrap(llvm::Expected<Value> result) {
  if (!result) { throw std::runtime_error(llvm::toString(result.takeError())); }
  return std::move(*result);
}

void check(llvm::Error error) {
  if (error) { throw std::runtime_error(llvm::toString(std::move(error))); }
}

struct Engine {
  std::unique_ptr<llvm::orc::LLJIT> jit;
  std::uint64_t bytes = 0;
  std::vector<TextRange> ranges;
  std::vector<FaultSite> sites;
};

std::optional<FaultSite> line_site(const Image &image,
                                   const llvm::DWARFDebugLine::LineTable &lines,
                                   const llvm::DWARFDebugLine::Row &row,
                                   const llvm::DWARFDebugLine::Row &next) {
  if (row.EndSequence || row.Line == 0 || next.Address.SectionIndex != row.Address.SectionIndex ||
      next.Address.Address <= row.Address.Address) {
    return std::nullopt;
  }
  std::string file;
  if (!lines.getFileNameByIndex(
          row.File, "", llvm::DILineInfoSpecifier::FileLineInfoKind::RawValue, file) ||
      !file.starts_with("armv5-v")) {
    return std::nullopt;
  }
  std::uint32_t variant = 0;
  const auto parsed = std::from_chars(file.data() + 7, file.data() + file.size(), variant);
  if (parsed.ec != std::errc{} || parsed.ptr != file.data() + file.size()) { return std::nullopt; }
  const auto declaration = std::ranges::find(image.variants, variant, &Variant::id);
  if (declaration == image.variants.end() ||
      std::ranges::none_of(declaration->spans, [&](const Span &span) {
        return row.Line - 1 >= span.address && row.Line - 1 - span.address < span.bytes.size();
      })) {
    return std::nullopt;
  }
  return FaultSite{.address = row.Address.Address,
                   .size = next.Address.Address - row.Address.Address,
                   .variant = variant,
                   .guest_pc = row.Line - 1};
}

void collect(Engine &engine,
             const Image &image,
             const llvm::object::ObjectFile &object,
             const llvm::RuntimeDyld::LoadedObjectInfo &loaded) {
  for (const auto &section : object.sections()) {
    if (section.isText() && section.getSize() != 0) {
      engine.ranges.push_back(
          {.address = loaded.getSectionLoadAddress(section), .size = section.getSize()});
    }
  }
  auto debug = llvm::DWARFContext::create(object);
  for (const auto &unit : debug->compile_units()) {
    const auto *lines = debug->getLineTableForUnit(unit.get());
    if (lines == nullptr) { continue; }
    for (std::size_t index = 0; index + 1 < lines->Rows.size(); ++index) {
      auto site = line_site(image, *lines, lines->Rows[index], lines->Rows[index + 1]);
      if (!site) { continue; }
      for (const auto &section : object.sections()) {
        if (!section.isText() || section.getIndex() != lines->Rows[index].Address.SectionIndex) {
          continue;
        }
        site->address += loaded.getSectionLoadAddress(section) - section.getAddress();
        engine.sites.push_back(*site);
      }
    }
  }
}

void optimize(llvm::Module &module) {
  llvm::PassBuilder builder;
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager components;
  llvm::ModuleAnalysisManager modules;
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(components);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, components, modules);
  auto passes = builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  passes.run(module, modules);
}

void validate_fault_map(Native &native) {
  std::ranges::sort(native.text_ranges, {}, &TextRange::address);
  std::ranges::sort(native.fault_sites, {}, &FaultSite::address);
  std::uintptr_t end = 0;
  for (const auto range : native.text_ranges) {
    if (range.address == 0 || range.size == 0 || range.address < end ||
        range.size > std::numeric_limits<std::uintptr_t>::max() - range.address) {
      refuse(native.image->identity, 0, "invalid/overlapping native executable ranges");
    }
    end = range.address + range.size;
  }
  end = 0;
  for (const auto site : native.fault_sites) {
    const auto variant = std::ranges::find(native.image->variants, site.variant, &Variant::id);
    if (site.address == 0 || site.size == 0 || site.address < end ||
        site.size > std::numeric_limits<std::uintptr_t>::max() - site.address ||
        variant == native.image->variants.end() ||
        std::ranges::none_of(variant->spans,
                             [&](const Span &span) {
                               return site.guest_pc >= span.address &&
                                      site.guest_pc - span.address < span.bytes.size() &&
                                      (site.guest_pc & (span.thumb ? 1U : 3U)) == 0;
                             }) ||
        std::ranges::none_of(native.text_ranges, [site](const TextRange &range) {
          return site.address >= range.address && site.address - range.address < range.size &&
                 site.size <= range.size - (site.address - range.address);
        })) {
      refuse(native.image->identity, site.guest_pc, "invalid/overlapping native guest-PC sites");
    }
    end = site.address + site.size;
  }
  if (native.image->options.memory_mode == CompileOptions::MemoryMode::direct &&
      (native.text_ranges.empty() || native.fault_sites.empty())) {
    refuse(native.image->identity,
           0,
           "Direct image requires linked native executable ranges and sites");
  }
  if (native.image->options.memory_mode == CompileOptions::MemoryMode::direct) {
    for (const auto entry : native.entries) {
      const auto address = std::bit_cast<std::uintptr_t>(entry);
      if (std::ranges::none_of(native.text_ranges, [address](const TextRange &range) {
            return address - range.address < range.size;
          })) {
        refuse(native.image->identity, 0, "native entry is outside linked executable ranges");
      }
    }
  }
}

} // namespace

Native native_bind(std::shared_ptr<const Image> image, const NativeSymbols &symbols) {
  Native result{
      .image = std::move(image),
      .entries = {},
      .fault_sites = {symbols.fault_map.sites.begin(), symbols.fault_map.sites.end()},
      .text_ranges = {symbols.fault_map.text_ranges.begin(), symbols.fault_map.text_ranges.end()},
      .report = symbols.report,
      .lifetime = symbols.lifetime};
  result.report.identity = result.image->identity;
  for (const auto &name : result.image->exports) {
    const NativeSymbols::Symbol *found = nullptr;
    for (const auto &symbol : symbols.symbols) {
      if (symbol.name != name) { continue; }
      if (found != nullptr) { refuse(result.image->identity, 0, "duplicate native export"); }
      found = &symbol;
    }
    if (found == nullptr || found->address == 0) {
      refuse(result.image->identity, 0, "missing native export: " + name);
    }
    result.entries.push_back(std::bit_cast<Entry>(found->address));
  }
  validate_fault_map(result);
  return result;
}

Native native_compile(std::shared_ptr<const Image> image) {
  const auto start = std::chrono::steady_clock::now();
  static std::once_flag initialize;
  std::call_once(initialize, [] {
    if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter()) {
      throw std::runtime_error("LLVM native target initialization failed");
    }
  });
  auto engine = std::make_shared<Engine>();
  llvm::orc::LLJITBuilder jit;
  jit.setObjectLinkingLayerCreator(
      [owner = engine.get(), image](llvm::orc::ExecutionSession &session, const llvm::Triple &) {
        auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
            session, [] { return std::make_unique<llvm::SectionMemoryManager>(); });
        layer->setNotifyLoaded([owner, image](llvm::orc::MaterializationResponsibility &,
                                              const llvm::object::ObjectFile &object,
                                              const llvm::RuntimeDyld::LoadedObjectInfo &loaded) {
          collect(*owner, *image, object, loaded);
        });
        return layer;
      });
  engine->jit = unwrap(jit.create());
  engine->jit->getObjTransformLayer().setTransform(
      [owner = engine.get()](std::unique_ptr<llvm::MemoryBuffer> object)
          -> llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> {
        auto file = llvm::object::ObjectFile::createObjectFile(object->getMemBufferRef());
        if (!file) { return file.takeError(); }
        for (const auto &section : (*file)->sections()) {
          if (section.isText()) { owner->bytes += section.getSize(); }
        }
        return object;
      });
  auto context = std::make_unique<llvm::LLVMContext>();
  const llvm::StringRef bytes(reinterpret_cast<const char *>(image->bitcode.data()),
                              image->bitcode.size());
  auto module =
      unwrap(llvm::parseBitcodeFile(llvm::MemoryBufferRef(bytes, image->identity), *context));
  module->setDataLayout(engine->jit->getDataLayout());
  optimize(*module);
  check(
      engine->jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context))));
  std::vector<NativeSymbols::Symbol> symbols;
  for (const auto &name : image->exports) {
    const auto address =
        unwrap(engine->jit->lookup(name)); // Materialize every entry before execution.
    symbols.push_back({.name = name, .address = static_cast<std::uintptr_t>(address.getValue())});
  }
  const PreparationReport report{
      .identity = image->identity,
      .seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
      .native_bytes = engine->bytes};
  return native_bind(std::move(image),
                     {.symbols = symbols,
                      .lifetime = engine,
                      .report = report,
                      .fault_map = {.text_ranges = engine->ranges, .sites = engine->sites}});
}
} // namespace armv5::detail
