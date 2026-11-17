// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "consumer.hpp"
#include "support/faults.hpp"
#include "support/isa.hpp"
#include "support/mapped.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/DebugInfo/DIContext.h>
#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Triple.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
constexpr std::uint32_t variant = 17;

template <class Value> Value checked(llvm::Expected<Value> result) {
  if (!result) { throw std::runtime_error(llvm::toString(result.takeError())); }
  return std::move(*result);
}

struct Provider {
  std::unique_ptr<llvm::orc::LLJIT> jit;
  std::vector<armv5::NativeSymbols::TextRange> ranges;
  std::vector<armv5::NativeSymbols::Site> sites;

  void loaded(const llvm::object::ObjectFile &object,
              const llvm::RuntimeDyld::LoadedObjectInfo &addresses) {
    auto debug = llvm::DWARFContext::create(object);
    for (const auto &section : object.sections()) {
      if (!section.isText() || section.getSize() == 0) { continue; }
      const auto base = addresses.getSectionLoadAddress(section);
      ranges.push_back({.address = base, .size = section.getSize()});
      // This independent provider asks LLVM for each byte's location instead of
      // parsing row intervals as the core's standalone compiler does.
      for (std::uint64_t offset = 0; offset < section.getSize(); ++offset) {
        const auto location = debug->getLineInfoForAddress(
            {.Address = section.getAddress() + offset, .SectionIndex = section.getIndex()});
        if (location.FileName != "armv5-v17" || location.Line == 0) { continue; }
        const auto pc = location.Line - 1;
        if (!sites.empty() && sites.back().address + sites.back().size == base + offset &&
            sites.back().guest_pc == pc) {
          ++sites.back().size;
        } else {
          sites.push_back(
              {.address = base + offset, .size = 1, .variant = variant, .guest_pc = pc});
        }
      }
    }
  }
};

std::shared_ptr<Provider> link(const armv5::SemanticImage &image) {
  fixture::require(!llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter(),
                   "consumer native initialization");
  auto owner = std::make_shared<Provider>();
  llvm::orc::LLJITBuilder builder;
  builder.setObjectLinkingLayerCreator(
      [provider = owner.get()](llvm::orc::ExecutionSession &session, const llvm::Triple &) {
        auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
            session, [] { return std::make_unique<llvm::SectionMemoryManager>(); });
        layer->setNotifyLoaded([provider](llvm::orc::MaterializationResponsibility &,
                                          const llvm::object::ObjectFile &object,
                                          const llvm::RuntimeDyld::LoadedObjectInfo &addresses) {
          provider->loaded(object, addresses);
        });
        return layer;
      });
  owner->jit = checked(builder.create());
  auto context = std::make_unique<llvm::LLVMContext>();
  const auto bytes = image.bitcode();
  auto module = checked(llvm::parseBitcodeFile(
      llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
          "independent-provider"),
      *context));
  module->setDataLayout(owner->jit->getDataLayout());
  if (auto error = owner->jit->addIRModule(
          llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
    throw std::runtime_error(llvm::toString(std::move(error)));
  }
  return owner;
}

void rejected(const armv5::SemanticImage &image, const armv5::NativeSymbols &symbols) {
  bool refused = false;
  try {
    (void)armv5::Core::bind(image, symbols);
  } catch (const std::runtime_error &) { refused = true; }
  fixture::require(refused, "invalid consumer native fault metadata is rejected");
}

void invalid_maps(const armv5::SemanticImage &image, armv5::NativeSymbols symbols) {
  auto ranges =
      std::vector(symbols.fault_map.text_ranges.begin(), symbols.fault_map.text_ranges.end());
  auto sites = std::vector(symbols.fault_map.sites.begin(), symbols.fault_map.sites.end());
  rejected(image, {.symbols = symbols.symbols, .lifetime = symbols.lifetime, .report = {}});
  auto exports = std::vector(symbols.symbols.begin(), symbols.symbols.end());
  exports.front().address = 1;
  rejected(image,
           {.symbols = exports,
            .lifetime = symbols.lifetime,
            .report = {},
            .fault_map = symbols.fault_map});
  const auto original_range = ranges.front();
  const auto original_site = sites.front();
  symbols.fault_map = {.text_ranges = ranges, .sites = sites};
  for (const auto bad :
       {armv5::NativeSymbols::TextRange{.address = 0, .size = 1},
        armv5::NativeSymbols::TextRange{.address = original_range.address, .size = 0},
        armv5::NativeSymbols::TextRange{.address = std::numeric_limits<std::uintptr_t>::max(),
                                        .size = 2}}) {
    ranges.front() = bad;
    rejected(image, symbols);
  }
  ranges.front() = original_range;
  for (const auto bad :
       {armv5::NativeSymbols::Site{
            .address = original_site.address, .size = 0, .variant = variant, .guest_pc = 0},
        armv5::NativeSymbols::Site{.address = original_site.address,
                                   .size = original_site.size,
                                   .variant = std::uint64_t{1} << 32,
                                   .guest_pc = 0},
        armv5::NativeSymbols::Site{.address = original_site.address,
                                   .size = original_site.size,
                                   .variant = variant,
                                   .guest_pc = 1},
        armv5::NativeSymbols::Site{.address = 1, .size = 1, .variant = variant, .guest_pc = 0},
        armv5::NativeSymbols::Site{.address = original_site.address,
                                   .size = original_site.size,
                                   .variant = variant,
                                   .guest_pc = 0x10000}}) {
    sites.front() = bad;
    rejected(image, symbols);
  }
  sites.front() = original_site;
  sites.push_back(original_site);
  symbols.fault_map.sites = sites;
  rejected(image, symbols);
  sites.pop_back();
  ranges.push_back(original_range);
  symbols.fault_map = {.text_ranges = ranges, .sites = sites};
  rejected(image, symbols);
}
} // namespace

void native_provider() {
  fixture::Program code(0);
  code.word(fixture::transfer(true, false, 0, 1, 0));
  code.word(fixture::data(13, false, 0, 0, true, 7)); // The read must survive a dead result.
  fixture::MappedMachine machine(code, 4096, armv5::CompileOptions::MemoryMode::direct);
  machine.regions_.back().bytes = machine.regions_.back().bytes.first(0x10000);
  machine.layout_.back().size = 0x10000;
  machine.core->set_memory({.regions = machine.regions_, .generation = 1});
  const std::array variants{
      armv5::CodeVariant{.id = variant, .spans = machine.spans_, .entries = machine.entries_}};
  const std::array layouts{armv5::VariantLayout{.variant = variant, .regions = machine.layout_}};
  const armv5::CodeImage declarations{.identity = "independent-native-provider",
                                      .variants = variants,
                                      .layouts = layouts,
                                      .transitions = {},
                                      .interrupt_vectors = {}};
  const armv5::CompileOptions options{.callbacks = {},
                                      .memory_mode = armv5::CompileOptions::MemoryMode::direct};
  const auto lowered = armv5::Core::lower(declarations, options);
  const auto image = armv5::Core::load(declarations, options, lowered.artifact());
  auto provider = link(image);
  std::vector<armv5::NativeSymbols::Symbol> exports;
  for (const auto &name : image.exports()) {
    exports.push_back(
        {.name = name,
         .address = static_cast<std::uintptr_t>(checked(provider->jit->lookup(name)).getValue())});
  }
  const armv5::NativeSymbols symbols{
      .symbols = exports,
      .lifetime = provider,
      .report = {},
      .fault_map = {.text_ranges = provider->ranges, .sites = provider->sites}};
  const auto native = armv5::Core::bind(image, symbols);
  invalid_maps(image, symbols);
  provider->ranges.clear();
  provider->sites.clear();
  (void)machine.core->install(native, variant);
  auto expected = machine.core->state();
  expected.registers.r[1] = fixture::AddressSpace::data_address;
  machine.core->set_registers(expected.registers);
  expected.registers.r[0] = 7;
  expected.registers.r[15] = 8;
  expected.progress = {.instructions = 2, .base_cycles = 2};
  (void)machine.run(2);
  fixture::require(machine.core->state() == expected,
                   "consumer-linked image owns copied native metadata");
#if defined(__linux__) && defined(__x86_64__)
  const fixture::FaultHandler handler;
  machine.core->reset(0);
  auto registers = machine.core->registers();
  registers.r[1] = 0x20000;
  machine.core->set_registers(registers);
  const auto result = machine.core->run(2);
  fixture::require(
      result.execution == armv5::ExecutionState::fault && result.fault.address == registers.r[1] &&
          result.fault.identity == declarations.identity && result.fault.variant == variant,
      "consumer-owned image reports a real native fault");
  fixture::contains(result.fault.reason, "read at guest PC 0x0;");
#endif
}
