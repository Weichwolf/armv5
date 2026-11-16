// SPDX-License-Identifier: GPL-3.0-only
#include "consumer.hpp"

#include "armv5/core.hpp"
#include "support/isa.hpp"

#include <array>
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

void cached_binding(bool thumb) {
  fixture::Program program;
  if (thumb) {
    constexpr std::uint16_t load = 0x6802;
    constexpr std::uint16_t loop = 0xe7fe;
    program.half(load);
    program.half(loop);
  } else {
    program.word(fixture::transfer(true, false, 2, 0, 0));
    program.word(fixture::branch(program.pc(), {.address = program.pc()}));
  }
  fixture::Machine machine(program);
  const armv5::CompileOptions options{
      .instruction_set = thumb ? armv5::CompileOptions::InstructionSet::thumb
                               : armv5::CompileOptions::InstructionSet::arm,
      .callbacks = {{.role = armv5::CallbackBinding::Role::read, .symbol = "consumer_read"}}};
  constexpr std::uint32_t interrupt_mode = 0x12;
  constexpr std::array processor_modes{armv5::CompileOptions::supervisor_mode, interrupt_mode};
  machine.spans.front().processor_modes = processor_modes;
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
  constexpr std::uint32_t thumb_bit = 0x20;
  registers.cpsr |= thumb ? thumb_bit : 0;
  registers.r[0] = unmapped;
  machine.registers(registers);
  (void)machine.run(1);
  fixture::require(cached.exports().size() == processor_modes.size(),
                   "cached per-mode native units");
  registers.cpsr = interrupt_mode | (thumb ? thumb_bit : 0);
  registers.r[2] = 0;
  machine.registers(registers);
  (void)machine.run(1);
  fixture::require(machine.core->registers().r[2] == output &&
                       machine.core->registers().cpsr == registers.cpsr,
                   "consumer-linked IR callback or bound native lifetime");
}

} // namespace

void consumer_binding() {
  cached_binding(false);
  cached_binding(true);
}
