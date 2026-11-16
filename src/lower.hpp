// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "image.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <map>
#include <string>
#include <vector>

namespace armv5::detail {
inline constexpr unsigned condition_shift = 28, opcode_shift = 21, shift_count_mask = 31;
inline constexpr std::uint32_t high_offset_mask = 0xf0;
inline constexpr std::uint32_t status_n = 0x80000000, status_z = 0x40000000, status_c = 0x20000000,
                               status_v = 0x10000000, nz_mask = 0xc0000000, nzcv_mask = 0xf0000000,
                               flags_mask = 0xf8000000, mode_mask = 0x1f, arm_pc_mask = 0xfffffffc,
                               instruction_offset_mask = 0x00ffffff, immediate_mask = 0xff,
                               transfer_offset_mask = 0xfff;

enum class Operation : std::uint8_t {
  alu,
  branch,
  exchange,
  single,
  extra,
  multiple,
  mrs,
  msr,
  preload,
  swap,
  multiply,
  software_interrupt,
  undefined,
  breakpoint
};

struct Instruction {
  std::uint32_t pc, word;
  Operation operation;
  unsigned cycles;
  bool terminates;
};

struct Values {
  std::array<llvm::Value *, 16> r{};
  std::array<llvm::Value *, 7> user{};
  llvm::Value *status = nullptr, *spsr = nullptr, *instructions = nullptr, *cycles = nullptr;
};

struct Join {
  llvm::BasicBlock *block = nullptr;
  Values values;
};

struct Block {
  std::size_t first, end;
  std::uint64_t cycles;
  Join head;
};

struct Shifted {
  llvm::Value *value, *carry;
};

class Lowering {
public:
  Lowering(Image &image,
           const Variant &variant,
           llvm::Module &module,
           const std::string &name,
           std::uint32_t mode);
  void emit();

private:
  Image &image_;
  const Variant &variant_;
  llvm::Module &module_;
  llvm::LLVMContext &context_;
  llvm::IRBuilder<> ir_;
  llvm::Function *function_;
  std::uint32_t mode_;
  llvm::Value *runtime_, *instruction_bound_{nullptr}, *cycle_bound_{nullptr}, *direct_{nullptr};
  llvm::Value *pending_signal_ = nullptr, *abort_base_ = nullptr;
  unsigned abort_register_ = 0;
  Join exit_;
  llvm::Value *exit_signal_ = nullptr, *exit_address_ = nullptr;
  std::vector<Instruction> code_;
  std::map<std::uint32_t, std::size_t> indices_;
  std::vector<Block> blocks_;
  std::vector<Join> tails_;
  std::map<std::uint32_t, std::size_t> heads_;

  llvm::Value *c32(std::uint32_t value);
  llvm::Value *c64(std::uint64_t value);
  llvm::Value *field(std::size_t offset);
  llvm::Value *load(llvm::Type *type, std::size_t offset);
  void store(llvm::Value *value, std::size_t offset);
  llvm::BasicBlock *block(const llvm::Twine &name);
  void cold_branch(llvm::Value *condition, llvm::BasicBlock *ordinary, llvm::BasicBlock *cold);
  Join join(const llvm::Twine &name);
  static void incoming(Join &destination, const Values &values, llvm::BasicBlock *source);
  void jump(Join &destination, const Values &values);
  void materialize(const Values &values);
  void exit(const Values &values, Signal signal, llvm::Value *address);
  void finish_exit(const Values &values, llvm::Value *signal, llvm::Value *address);
  void guard(llvm::Value *valid, const Values &values, Signal signal, llvm::Value *address);
  void signal_guard(const Values &values);
  void transfer(Values values, llvm::Value *target, bool exchange);
  void edge(Values values, std::uint32_t target);
  void unknown_target(Values values, llvm::Value *target);
  Values entry_values();
  void analyze();
  void emit_block(Block &body);
  void emit_tail(std::size_t index);
  bool emit_event(const Instruction &instruction, Values &values, llvm::Value *old_status);
  void emit_instruction(const Instruction &instruction, Values &values);
  void branch_instruction(const Instruction &instruction, Values values);
  llvm::Value *condition(unsigned code, llvm::Value *status);
  llvm::Value *reg(const Instruction &instruction, const Values &values, unsigned index);
  llvm::Value *flag(llvm::Value *status, std::uint32_t mask);
  Shifted
  shift(llvm::Value *value, unsigned type, llvm::Value *amount, bool immediate, llvm::Value *carry);
  Shifted operand(const Instruction &instruction, const Values &values);
  void alu(const Instruction &instruction, Values &values);
  void multiply(const Instruction &instruction, Values &values);
  [[nodiscard]] bool exception_return(const Instruction &instruction) const;
  llvm::Value *user_reg(const Values &values, unsigned index) const;
  void set_user_reg(Values &values, unsigned index, llvm::Value *value) const;
  void status_transfer(const Instruction &instruction, Values &values);
  llvm::Value *access(
      Values &values, llvm::Value *address, unsigned width, unsigned operation, llvm::Value *value);
  llvm::Value *slow_access(
      Values &values, llvm::Value *address, unsigned width, unsigned operation, llvm::Value *value);

  struct Transfer {
    unsigned base, destination, width, count;
    bool extra, reading, pre, up, writeback, register_offset, sign_extend;
  };

  static Transfer transfer_plan(const Instruction &instruction);
  void validate_transfer(const Instruction &instruction, const Transfer &plan) const;
  llvm::Value *
  transfer_offset(const Instruction &instruction, const Values &values, const Transfer &plan);
  void swap_memory(const Instruction &instruction, Values &values);
  void memory(const Instruction &instruction, Values &values);
  [[nodiscard]] bool multiple_user_bank(const Instruction &instruction) const;
  void multiple(const Instruction &instruction, Values &values);
};
} // namespace armv5::detail
