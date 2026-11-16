// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "image.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
};

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
} // namespace

Native native_bind(std::shared_ptr<const Image> image, const NativeSymbols &symbols) {
  Native result{.image = std::move(image),
                .entries = {},
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
  engine->jit = unwrap(llvm::orc::LLJITBuilder().create());
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
  return native_bind(std::move(image), {.symbols = symbols, .lifetime = engine, .report = report});
}
} // namespace armv5::detail
