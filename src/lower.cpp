// SPDX-License-Identifier: GPL-3.0-only
#include "lower.hpp"

#include "decode.hpp"
#include "image.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace armv5::detail {
namespace {
std::uint32_t word_at(const Span &span, std::size_t offset, bool big_endian) {
  std::uint32_t result = 0;
  for (unsigned index = 0; index < 4; ++index) {
    result |= std::to_integer<std::uint32_t>(span.bytes[offset + index])
              << ((big_endian ? 3 - index : index) * 8);
  }
  return result;
}

bool classify_alu(const Image &image, std::string_view name, Instruction &result) {
  const auto pc = result.pc;
  const auto word = result.word;
  constexpr std::array alu_names{"AND",
                                 "EOR",
                                 "SUB",
                                 "RSB",
                                 "ADD",
                                 "ADC",
                                 "SBC",
                                 "RSC",
                                 "TST",
                                 "TEQ",
                                 "CMP",
                                 "CMN",
                                 "ORR",
                                 "MOV",
                                 "BIC",
                                 "MVN"};
  const auto destination = (word >> 12) & 15;
  if (std::ranges::find(alu_names, name) == alu_names.end()) { return false; }
  const auto opcode = (word >> opcode_shift) & 15;
  const bool test = opcode >= 8 && opcode <= 11;
  const bool register_shift = (word & 0x02000000) == 0 && (word & 0x10) != 0;
  if (register_shift && (destination == 15 || ((word >> 16) & 15) == 15 || (word & 15) == 15 ||
                         ((word >> 8) & 15) == 15)) {
    refuse(image.identity, pc, "unpredictable PC in register-controlled shift");
  }
  if (test && destination != 0) {
    refuse(image.identity, pc, "unpredictable test destination field");
  }
  if ((opcode == 13 || opcode == 15) && ((word >> 16) & 15) != 0) {
    refuse(image.identity, pc, "unpredictable move source field");
  }
  if (!test && destination == 15 && (word & 0x00100000) != 0) {
    refuse(image.identity, pc, "SPSR exception return requires A3");
  }
  result.terminates = !test && destination == 15;
  result.cycles += unsigned(register_shift) + (result.terminates ? 2 : 0);
  return true;
}

bool classify_memory(const Image &image, std::string_view name, Instruction &result) {
  const auto pc = result.pc;
  const auto word = result.word;
  const auto destination = (word >> 12) & 15;
  if (name == "LDR" || name == "STR" || name == "LDRB" || name == "STRB") {
    result.operation = Operation::single;
    result.terminates = (word & 0x00100000) != 0 && destination == 15;
    if (destination == 15 && (word & 0x00400000) != 0) {
      refuse(image.identity, pc, "unpredictable byte transfer PC");
    }
    result.cycles = result.terminates ? 5 : 1;
    return true;
  }
  if (name == "LDRH" || name == "STRH" || name == "LDRSB" || name == "LDRSH" || name == "LDRD" ||
      name == "STRD") {
    result.operation = Operation::extra;
    if (destination == 15) { refuse(image.identity, pc, "unpredictable extra transfer PC"); }
    const bool pair = name == "LDRD" || name == "STRD";
    if (pair && ((destination & 1) != 0 || destination >= 14)) {
      refuse(image.identity, pc, "unpredictable doubleword register pair");
    }
    result.cycles = pair ? 2 : 1;
    return true;
  }
  return false;
}

bool classify_multiple(const Image &image, std::string_view name, Instruction &result) {
  const auto pc = result.pc;
  const auto word = result.word;
  if (name == "LDM" || name == "STM") {
    result.operation = Operation::multiple;
    constexpr std::uint32_t register_list = 0xffff;
    const auto count = std::popcount(word & register_list);
    if (count == 0 || ((word >> 16) & 15) == 15) {
      refuse(image.identity, pc, "unpredictable multiple transfer register list/base");
    }
    if ((word & 0x00400000) != 0) {
      refuse(image.identity, pc, "user-bank/exception multiple transfer requires A3");
    }
    result.terminates = name == "LDM" && (word & 0x8000) != 0;
    result.cycles = result.terminates ? unsigned(count + 4) : unsigned(std::max(count, 2));
    return true;
  }
  return false;
}

Instruction classify(const Image &image, std::uint32_t pc, std::uint32_t word) {
  std::string_view name;
  try {
    name = decode_arm(word);
  } catch (const std::runtime_error &error) { refuse(image.identity, pc, error.what()); }
  Instruction result{
      .pc = pc, .word = word, .operation = Operation::alu, .cycles = 1, .terminates = false};
  if (classify_alu(image, name, result) || classify_memory(image, name, result) ||
      classify_multiple(image, name, result)) {
    return result;
  }
  if (name == "B" || name == "BL") {
    result.operation = Operation::branch;
    result.cycles = 3;
    result.terminates = true;
    return result;
  }
  if (name == "BX" || name == "BLX register") {
    result.operation = Operation::exchange;
    result.cycles = 3;
    result.terminates = true;
    return result;
  }
  if (name == "MRS") {
    result.operation = Operation::mrs;
    result.cycles = 2;
    return result;
  }
  if (name == "MSR") {
    if ((word & 0x00400000) != 0 || (word & 0x00010000) != 0) {
      refuse(image.identity, pc, "SPSR/control MSR requires A3");
    }
    result.operation = Operation::msr;
    return result;
  }
  if (name == "PLD") {
    result.operation = Operation::preload;
    return result;
  }
  if (name == "SWP" || name == "SWPB") {
    result.operation = Operation::swap;
    result.cycles = 2;
    return result;
  }
  std::string reason(name);
  reason.append(": instruction is not lowered yet");
  refuse(image.identity, pc, reason);
}

std::uint32_t branch_target(const Instruction &instruction) {
  const auto displaced = (instruction.word & instruction_offset_mask) << 2;
  const auto signed_offset = (displaced ^ 0x02000000) - 0x02000000;
  return instruction.pc + 8 + signed_offset;
}
} // namespace

Lowering::Lowering(Image &image,
                   const Variant &variant,
                   llvm::Module &module,
                   const std::string &name)
    : image_(image),
      variant_(variant),
      module_(module),
      context_(module.getContext()),
      ir_(context_),
      function_(
          llvm::Function::Create(llvm::FunctionType::get(ir_.getVoidTy(), {ir_.getPtrTy()}, false),
                                 llvm::GlobalValue::ExternalLinkage,
                                 name,
                                 module)),
      runtime_(function_->getArg(0)) {
  function_->addFnAttr(llvm::Attribute::NoUnwind);
}

llvm::Value *Lowering::c32(std::uint32_t value) {
  return ir_.getInt32(value);
}

llvm::Value *Lowering::c64(std::uint64_t value) {
  return ir_.getInt64(value);
}

llvm::Value *Lowering::field(std::size_t offset) {
  return ir_.CreateGEP(ir_.getInt8Ty(), runtime_, c64(offset));
}

llvm::Value *Lowering::load(llvm::Type *type, std::size_t offset) {
  return ir_.CreateLoad(type, field(offset));
}

void Lowering::store(llvm::Value *value, std::size_t offset) {
  ir_.CreateStore(value, field(offset));
}

llvm::BasicBlock *Lowering::block(const llvm::Twine &name) {
  return llvm::BasicBlock::Create(context_, name, function_);
}

void Lowering::cold_branch(llvm::Value *condition,
                           llvm::BasicBlock *ordinary,
                           llvm::BasicBlock *cold) {
  constexpr std::uint32_t ordinary_weight = 10000;
  ir_.CreateCondBr(
      condition, ordinary, cold, llvm::MDBuilder(context_).createBranchWeights(ordinary_weight, 1));
}

Join Lowering::join(const llvm::Twine &name) {
  Join result{.block = block(name), .values = {}};
  llvm::IRBuilder<> builder(result.block);
  for (auto &reg : result.values.r) { reg = builder.CreatePHI(ir_.getInt32Ty(), 2); }
  result.values.status = builder.CreatePHI(ir_.getInt32Ty(), 2);
  result.values.instructions = builder.CreatePHI(ir_.getInt64Ty(), 2);
  result.values.cycles = builder.CreatePHI(ir_.getInt64Ty(), 2);
  return result;
}

void Lowering::incoming(Join &destination, const Values &values, llvm::BasicBlock *source) {
  for (unsigned index = 0; index < values.r.size(); ++index) {
    llvm::cast<llvm::PHINode>(destination.values.r[index])->addIncoming(values.r[index], source);
  }
  llvm::cast<llvm::PHINode>(destination.values.status)->addIncoming(values.status, source);
  llvm::cast<llvm::PHINode>(destination.values.instructions)
      ->addIncoming(values.instructions, source);
  llvm::cast<llvm::PHINode>(destination.values.cycles)->addIncoming(values.cycles, source);
}

void Lowering::jump(Join &destination, const Values &values) {
  incoming(destination, values, ir_.GetInsertBlock());
  ir_.CreateBr(destination.block);
}

void Lowering::materialize(const Values &values) {
  for (unsigned index = 0; index < values.r.size(); ++index) {
    store(values.r[index], offsetof(Runtime, r) + (index * sizeof(std::uint32_t)));
  }
  store(values.status, offsetof(Runtime, cpsr));
  store(values.instructions, offsetof(Runtime, instructions));
  store(values.cycles, offsetof(Runtime, cycles));
}

void Lowering::finish_exit(const Values &values, llvm::Value *signal, llvm::Value *address) {
  auto *source = ir_.GetInsertBlock();
  llvm::cast<llvm::PHINode>(exit_signal_)->addIncoming(signal, source);
  llvm::cast<llvm::PHINode>(exit_address_)->addIncoming(address, source);
  jump(exit_, values);
}

void Lowering::exit(const Values &values, Signal signal, llvm::Value *address) {
  finish_exit(values, c32(static_cast<unsigned>(signal)), address);
}

void Lowering::guard(llvm::Value *valid,
                     const Values &values,
                     Signal signal,
                     llvm::Value *address) {
  auto *ordinary = block("continue");
  auto *cold = block("refusal");
  cold_branch(valid, ordinary, cold);
  ir_.SetInsertPoint(cold);
  exit(values, signal, address);
  ir_.SetInsertPoint(ordinary);
}

void Lowering::signal_guard(const Values &values) {
  auto *ordinary = block("continue");
  auto *cold = block("event");
  cold_branch(ir_.CreateICmpEQ(pending_signal_, c32(0)), ordinary, cold);
  ir_.SetInsertPoint(cold);
  finish_exit(values, pending_signal_, load(ir_.getInt32Ty(), offsetof(Runtime, address)));
  ir_.SetInsertPoint(ordinary);
}

Values Lowering::entry_values() {
  Values values;
  for (unsigned index = 0; index < values.r.size(); ++index) {
    values.r[index] =
        load(ir_.getInt32Ty(), offsetof(Runtime, r) + (index * sizeof(std::uint32_t)));
  }
  values.status = load(ir_.getInt32Ty(), offsetof(Runtime, cpsr));
  values.instructions = load(ir_.getInt64Ty(), offsetof(Runtime, instructions));
  values.cycles = load(ir_.getInt64Ty(), offsetof(Runtime, cycles));
  return values;
}

void Lowering::unknown_target(Values values, llvm::Value *target) {
  values.r[15] = target;
  auto *bound = block("bound");
  auto *missing = block("missing");
  ir_.CreateCondBr(ir_.CreateOr(ir_.CreateICmpUGE(values.instructions, instruction_bound_),
                                ir_.CreateICmpUGE(values.cycles, cycle_bound_)),
                   bound,
                   missing);
  ir_.SetInsertPoint(bound);
  exit(values, Signal::none, target);
  ir_.SetInsertPoint(missing);
  exit(values, Signal::unprepared, target);
}

void Lowering::edge(Values values, std::uint32_t target) {
  values.r[15] = c32(target);
  const auto found = indices_.find(target);
  if (found != indices_.end()) {
    const auto head = heads_.find(target);
    jump(head != heads_.end() ? blocks_[head->second].head : tails_[found->second], values);
    return;
  }
  for (const auto &transition : image_.transitions) {
    if (transition.source_variant == variant_.id && transition.address == target) {
      exit(values, Signal::transfer, c32(target));
      return;
    }
  }
  unknown_target(values, c32(target));
}

void Lowering::transfer(Values values, llvm::Value *target, bool exchange) {
  if (exchange) {
    guard(ir_.CreateICmpEQ(ir_.CreateAnd(target, c32(1)), c32(0)),
          values,
          Signal::unsupported,
          target);
  }
  target = ir_.CreateAnd(target, c32(arm_pc_mask));
  if (const auto *constant = llvm::dyn_cast<llvm::ConstantInt>(target)) {
    edge(values, static_cast<std::uint32_t>(constant->getZExtValue()));
    return;
  }
  values.r[15] = target;
  auto *unknown = block("undeclared.target");
  auto *dispatch = ir_.CreateSwitch(target, unknown, unsigned(code_.size()));
  for (auto &index : code_) {
    auto *destination = block("indirect.target");
    dispatch->addCase(ir_.getInt32(index.pc), destination);
    ir_.SetInsertPoint(destination);
    edge(values, index.pc);
  }
  for (const auto &transition : image_.transitions) {
    if (transition.source_variant != variant_.id || indices_.contains(transition.address)) {
      continue;
    }
    auto *destination = block("variant.target");
    dispatch->addCase(ir_.getInt32(transition.address), destination);
    ir_.SetInsertPoint(destination);
    edge(values, transition.address);
  }
  ir_.SetInsertPoint(unknown);
  unknown_target(values, target);
}

void Lowering::analyze() {
  std::set<std::uint32_t> leaders(variant_.entries.begin(), variant_.entries.end());
  for (const auto &span : variant_.spans) {
    leaders.insert(span.address);
    for (std::size_t offset = 0; offset < span.bytes.size(); offset += 4) {
      const auto pc = span.address + static_cast<std::uint32_t>(offset);
      indices_.emplace(pc, code_.size());
      code_.push_back(classify(image_, pc, word_at(span, offset, image_.options.big_endian)));
    }
  }
  for (const auto &instruction : code_) {
    if (instruction.terminates) { leaders.insert(instruction.pc + 4); }
    if (instruction.operation == Operation::branch) { leaders.insert(branch_target(instruction)); }
  }
  for (std::size_t first = 0; first < code_.size();) {
    std::size_t end = first + 1;
    while (end < code_.size() && !leaders.contains(code_[end].pc)) { ++end; }
    std::uint64_t cycles = 0;
    for (auto index = first; index < end; ++index) { cycles += code_[index].cycles; }
    heads_.emplace(code_[first].pc, blocks_.size());
    blocks_.push_back({.first = first, .end = end, .cycles = cycles, .head = join("block")});
    first = end;
  }
  for (std::size_t index = 0; index < code_.size(); ++index) {
    tails_.push_back(join("bound.tail"));
  }
}

void Lowering::branch_instruction(const Instruction &instruction, Values values) {
  const auto predicate = instruction.word >> condition_shift;
  values.instructions = ir_.CreateAdd(values.instructions, c64(1));
  if (predicate != 14) {
    auto *taken = block("branch.taken");
    auto *skipped = block("branch.not.taken");
    ir_.CreateCondBr(condition(predicate, values.status), taken, skipped);
    ir_.SetInsertPoint(skipped);
    auto bypass = values;
    bypass.cycles = ir_.CreateAdd(bypass.cycles, c64(1));
    edge(bypass, instruction.pc + 4);
    ir_.SetInsertPoint(taken);
  }
  if ((instruction.word & 0x01000000) != 0) { values.r[14] = c32(instruction.pc + 4); }
  values.cycles = ir_.CreateAdd(values.cycles, c64(3));
  edge(values, branch_target(instruction));
}

void Lowering::emit_instruction(const Instruction &instruction, Values &values) {
  values.r[15] = c32(instruction.pc);
  pending_signal_ = c32(0);
  if (instruction.operation == Operation::branch) {
    branch_instruction(instruction, values);
    return;
  }
  const auto predicate = instruction.word >> condition_shift;
  auto merged = join("predication");
  auto *taken = block("execute");
  if (predicate != 14) {
    auto *skip = block("predicate.false");
    ir_.CreateCondBr(condition(predicate, values.status), taken, skip);
    ir_.SetInsertPoint(skip);
    auto skipped = values;
    skipped.r[15] = c32(instruction.pc + 4);
    skipped.instructions = ir_.CreateAdd(skipped.instructions, c64(1));
    skipped.cycles = ir_.CreateAdd(skipped.cycles, c64(1));
    jump(merged, skipped);
  } else {
    ir_.CreateBr(taken);
  }
  ir_.SetInsertPoint(taken);
  switch (instruction.operation) {
    case Operation::alu: alu(instruction, values); break;
    case Operation::branch: break; // Emitted above with direct known successors.
    case Operation::exchange: {
      auto *target = reg(instruction, values, instruction.word & 15);
      if ((instruction.word & 0x20) != 0) { values.r[14] = c32(instruction.pc + 4); }
      values.r[15] = target;
      break;
    }
    case Operation::single:
    case Operation::extra:
    case Operation::swap: memory(instruction, values); break;
    case Operation::multiple: multiple(instruction, values); break;
    case Operation::mrs:
    case Operation::msr: status_transfer(instruction, values); break;
    case Operation::preload: break; // Advisory hint has no required architectural access or effect.
  }
  if (!instruction.terminates) { values.r[15] = c32(instruction.pc + 4); }
  values.instructions = ir_.CreateAdd(values.instructions, c64(1));
  values.cycles = ir_.CreateAdd(values.cycles, c64(instruction.cycles));
  if (instruction.operation == Operation::single || instruction.operation == Operation::extra ||
      instruction.operation == Operation::multiple || instruction.operation == Operation::swap) {
    signal_guard(values);
  }
  jump(merged, values);
  ir_.SetInsertPoint(merged.block);
  values = merged.values;
  if (instruction.terminates) {
    transfer(values,
             values.r[15],
             instruction.operation != Operation::alu && instruction.operation != Operation::branch);
  }
}

void Lowering::emit_block(Block &body) {
  ir_.SetInsertPoint(body.head.block);
  auto values = body.head.values;
  const auto count = body.end - body.first;
  auto *fits = ir_.CreateAnd(
      ir_.CreateICmpULE(c64(count), ir_.CreateSub(instruction_bound_, values.instructions)),
      ir_.CreateICmpULE(c64(body.cycles), ir_.CreateSub(cycle_bound_, values.cycles)));
  // Unsigned subtraction is only valid for the admission proof while below both bounds.
  fits = ir_.CreateAnd(fits,
                       ir_.CreateAnd(ir_.CreateICmpULT(values.instructions, instruction_bound_),
                                     ir_.CreateICmpULT(values.cycles, cycle_bound_)));
  auto *ordinary = block("admitted");
  incoming(tails_[body.first], values, ir_.GetInsertBlock());
  cold_branch(fits, ordinary, tails_[body.first].block);
  ir_.SetInsertPoint(ordinary);
  for (auto index = body.first; index < body.end; ++index) {
    emit_instruction(code_[index], values);
  }
  if (!code_[body.end - 1].terminates) { edge(values, code_[body.end - 1].pc + 4); }
}

void Lowering::emit_tail(std::size_t index) {
  ir_.SetInsertPoint(tails_[index].block);
  auto values = tails_[index].values;
  values.r[15] = c32(code_[index].pc);
  guard(ir_.CreateAnd(ir_.CreateICmpULT(values.instructions, instruction_bound_),
                      ir_.CreateICmpULT(values.cycles, cycle_bound_)),
        values,
        Signal::none,
        values.r[15]);
  emit_instruction(code_[index], values);
  if (code_[index].terminates) { return; }
  const auto next = code_[index].pc + 4;
  const auto found = indices_.find(next);
  if (found != indices_.end() && !heads_.contains(next)) {
    jump(tails_[found->second], values);
  } else {
    edge(values, next);
  }
}

void Lowering::emit() {
  auto *entry = block("entry");
  ir_.SetInsertPoint(entry);
  const auto initial = entry_values();
  spsr_ = load(ir_.getInt32Ty(), offsetof(Runtime, spsr));
  instruction_bound_ = load(ir_.getInt64Ty(), offsetof(Runtime, instruction_bound));
  cycle_bound_ = load(ir_.getInt64Ty(), offsetof(Runtime, cycle_bound));
  direct_ = load(ir_.getPtrTy(), offsetof(Runtime, direct));
  exit_ = join("exit");
  llvm::IRBuilder<> exit_builder(exit_.block);
  exit_signal_ = exit_builder.CreatePHI(ir_.getInt32Ty(), 2);
  exit_address_ = exit_builder.CreatePHI(ir_.getInt32Ty(), 2);
  analyze();
  ir_.SetInsertPoint(entry);
  auto *unknown = block("unprepared.entry");
  auto *dispatch = ir_.CreateSwitch(initial.r[15], unknown, unsigned(code_.size()));
  for (std::size_t index = 0; index < code_.size(); ++index) {
    const auto head = heads_.find(code_[index].pc);
    auto &destination = head == heads_.end() ? tails_[index] : blocks_[head->second].head;
    incoming(destination, initial, entry);
    dispatch->addCase(ir_.getInt32(code_[index].pc), destination.block);
  }
  ir_.SetInsertPoint(unknown);
  exit(initial, Signal::unprepared, initial.r[15]);
  for (auto &body : blocks_) { emit_block(body); }
  for (std::size_t index = 0; index < code_.size(); ++index) { emit_tail(index); }
  ir_.SetInsertPoint(exit_.block);
  materialize(exit_.values);
  store(exit_signal_, offsetof(Runtime, signal));
  store(exit_address_, offsetof(Runtime, address));
  ir_.CreateRetVoid();
}

void lower_llvm(Image &image) {
  llvm::LLVMContext context;
  llvm::Module module(image.identity, context);
  module.setTargetTriple(llvm::sys::getDefaultTargetTriple());
  for (const auto &variant : image.variants) {
    const auto name = "armv5_" + image.key + "_v" + std::to_string(variant.id);
    Lowering(image, variant, module, name).emit();
    image.exports.push_back(name);
  }
  std::string error;
  llvm::raw_string_ostream diagnostics(error);
  if (llvm::verifyModule(module, &diagnostics)) {
    refuse(image.identity, 0, "invalid generated LLVM IR: " + error);
  }
  llvm::SmallVector<char, 0> bytes;
  llvm::raw_svector_ostream stream(bytes);
  llvm::WriteBitcodeToFile(module, stream);
  const auto view = std::as_bytes(std::span(bytes.data(), bytes.size()));
  image.bitcode.assign(view.begin(), view.end());
}
} // namespace armv5::detail
