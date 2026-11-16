// SPDX-License-Identifier: GPL-3.0-only
#include "consumer.hpp"

#include "armv5/core.hpp"
#include "support/isa.hpp"

#include <cstdint>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/TargetSelect.h>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
template <class Value> Value checked(llvm::Expected<Value> result) {
  if (!result) { throw std::runtime_error(llvm::toString(result.takeError())); }
  return std::move(*result);
}
}

void consumer_binding() {
  fixture::Program program;
  program.word(fixture::transfer(true, false, 2, 0, 0));
  program.word(fixture::branch(program.pc(), {.address = program.pc()}));
  fixture::Machine machine(program);
  const armv5::CompileOptions options{
      .callbacks = {{.role = armv5::CallbackBinding::Role::read, .symbol = "consumer_read"}}};
  const auto declarations = machine.image();
  const auto lowered = armv5::Core::lower(declarations, options);
  const auto cached = armv5::Core::load(declarations, options, lowered.artifact());
  fixture::require(!llvm::InitializeNativeTarget() && !llvm::InitializeNativeTargetAsmPrinter(),
                   "consumer LLVM initialization");
  auto engine = std::shared_ptr<llvm::orc::LLJIT>(checked(llvm::orc::LLJITBuilder().create()));
  auto context = std::make_unique<llvm::LLVMContext>();
  const auto bytes = cached.bitcode();
  auto module = checked(llvm::parseBitcodeFile(
      llvm::MemoryBufferRef(
          llvm::StringRef(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
          "consumer-cache"),
      *context));
  module->setDataLayout(engine->getDataLayout());
  auto *callback = module->getFunction("consumer_read");
  fixture::require(callback != nullptr, "callback binding absent from semantic image");
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(*context, "linked.callback", callback));
  constexpr std::uint32_t output = 0x12345678;
  ir.CreateStore(ir.getInt32(output), callback->getArg(3));
  ir.CreateRet(ir.getTrue());
  if (auto error =
          engine->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
    throw std::runtime_error(llvm::toString(std::move(error)));
  }
  std::vector<armv5::NativeSymbols::Symbol> symbols;
  for (const auto &name : cached.exports()) {
    const auto address = checked(engine->lookup(name));
    symbols.push_back({.name = name, .address = static_cast<std::uintptr_t>(address.getValue())});
  }
  const auto native =
      armv5::Core::bind(cached, {.symbols = symbols, .lifetime = engine, .report = {}});
  (void)machine.core->install(native, 0);
  engine.reset();
  auto registers = machine.core->registers();
  constexpr std::uint32_t unmapped = 0xf0000000;
  registers.r[0] = unmapped;
  machine.registers(registers);
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[2] == output,
                   "consumer-linked IR callback or bound native lifetime");
}
