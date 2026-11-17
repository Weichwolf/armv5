// SPDX-License-Identifier: GPL-3.0-only
#include "armv5/core.hpp"
#include "armv5/memory.hpp"
#include "support/isa.hpp"
#include "support/mmu.hpp"
#include "support/modes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {
constexpr unsigned fcse_data_root = 72;
constexpr std::uint32_t device_base_value = 100;
constexpr std::uint32_t last_word_offset = 28;
constexpr std::uint32_t all_bits = 0xffffffff;
constexpr std::uint32_t last_first_line_value = 107;
constexpr std::uint32_t first_second_line_value = 108;
constexpr std::uint32_t second_bus_word = 0x204004;
constexpr std::uint32_t cleaned_status = 0x080000d3;
constexpr std::uint32_t poison_byte = 0xa5;
constexpr std::uint32_t protected_tail_offset = 24;
constexpr std::uint32_t replacement_word_offset = 20;
constexpr std::uint32_t cacheable = 0x00200c3e, through = 0x00200c3a, flags = 0x980000d3,
                        added_flags = 0x680000d3, data_status = 0x55, instruction_status = 0xaa,
                        far = 0x87654321, replacement = 0x99aabbcc, adds = 0xe2955001,
                        multiple = 0xe8b00006, load = 0xe5901000, store = 0xe5802000,
                        clean = 0xee070f3a, test_clean_pc = 0xee17ff7a,
                        test_invalidate_pc = 0xee17ff7e, swap = 0xe1003092;

struct Device {
  armv5::Core *cpu = nullptr;
  std::array<std::uint32_t, 16> words{};
  std::vector<std::uint32_t> reads, writes;
  std::vector<armv5::State> prefixes;
  unsigned yield_read = 0, fail_read = 0, yield_write = 0, fail_write = 0;

  Device() {
    for (unsigned index = 0; index < words.size(); ++index) {
      words[index] = device_base_value + index;
    }
  }

  struct Transfer {
    std::uint32_t address;
    unsigned width;
    std::uint32_t value;
    bool writing;
  };

  bool transfer(Transfer request, std::uint32_t &value) {
    auto &events = request.writing ? writes : reads;
    events.push_back(request.address);
    prefixes.push_back(cpu->state());
    const auto index = (request.address - 0x204000) / 4;
    if (request.width != 4 || index >= words.size() ||
        events.size() == (request.writing ? fail_write : fail_read)) {
      return false;
    }
    if (request.writing) {
      words[index] = request.value;
    } else {
      value = words[index];
    }
    if (events.size() == (request.writing ? yield_write : yield_read)) { cpu->request_yield(); }
    return true;
  }

  static bool
  read(void *owner, std::uint32_t address, unsigned width, std::uint32_t &value) noexcept {
    return static_cast<Device *>(owner)->transfer(
        {.address = address, .width = width, .value = 0, .writing = false}, value);
  }

  static bool
  write(void *owner, std::uint32_t address, unsigned width, std::uint32_t value) noexcept {
    std::uint32_t ignored = 0;
    return static_cast<Device *>(owner)->transfer(
        {.address = address, .width = width, .value = value, .writing = true}, ignored);
  }
};

void attach(fixture::MmuMachine &machine, Device &device) {
  device.cpu = machine.core.get();
  std::erase_if(machine.regions, [](const armv5::Memory::Region &region) {
    return region.address == fixture::MmuMachine::data_physical;
  });
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  machine.context.control |= 4;
  machine.first(32, cacheable);
}

void read_fill(unsigned failure) {
  Device device;
  device.yield_read = 1;
  device.fail_read = failure;
  fixture::Program code;
  code.word(adds);
  code.word(multiple);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}},
                              {},
                              false,
                              {.owner = &device, .read = &Device::read, .write = &Device::write});
  attach(machine, device);
  machine.prepare(false);
  auto registers = machine.core->registers();
  registers.cpsr = flags;
  registers.r[0] = fixture::MmuMachine::data_virtual + last_word_offset;
  registers.r[5] = all_bits;
  registers.system.data_fault_status = data_status;
  registers.system.instruction_fault_status = instruction_status;
  registers.system.fault_address = far;
  machine.core->set_registers(registers);
  auto prefix = machine.core->state();
  prefix.registers.r[5] = 0;
  prefix.registers.r[15] += 4;
  prefix.registers.cpsr = added_flags;
  prefix.progress = {.instructions = 1, .base_cycles = 1};
  const auto result = machine.core->run(3);
  constexpr std::array<std::uint32_t, 16> addresses{0x20401c,
                                                    0x204000,
                                                    second_bus_word,
                                                    0x204008,
                                                    0x20400c,
                                                    0x204010,
                                                    0x204014,
                                                    0x204018,
                                                    0x204020,
                                                    0x204024,
                                                    0x204028,
                                                    0x20402c,
                                                    0x204030,
                                                    0x204034,
                                                    0x204038,
                                                    0x20403c};
  const auto count = failure == 0 ? addresses.size() : failure;
  fixture::require(device.reads == std::vector(addresses.begin(), addresses.begin() + count) &&
                       device.writes.empty(),
                   "cache fill is critical-word first and wraps within each line");
  for (unsigned index = 0; index < device.prefixes.size(); ++index) {
    auto observed = prefix;
    if (index >= 8) { observed.registers.r[1] = last_first_line_value; }
    fixture::require(device.prefixes[index] == observed,
                     "fill callback observes completed ADDS and exact partial LDM prefix");
  }
  auto expected = prefix;
  if (failure == 0) {
    expected.registers.r[0] += 8;
    expected.registers.r[1] = last_first_line_value;
    expected.registers.r[2] = first_second_line_value;
    expected.registers.r[15] = code.pc();
    expected.progress = {.instructions = 2, .base_cycles = 3};
    expected.execution = armv5::ExecutionState::yielded;
  } else {
    if (failure > 8) { expected.registers.r[1] = last_first_line_value; }
    expected.execution = armv5::ExecutionState::fault;
    expected.fault = result.fault;
    fixture::require(result.fault.kind == armv5::Fault::Kind::memory &&
                         result.fault.address == addresses[failure - 1],
                     "refused cache fill is a host memory fault");
  }
  fixture::require(machine.core->state() == expected && result.progress == expected.progress,
                   "linefill yield survives a later probe/retry; refusal preserves FSRs and "
                   "partial LDM without writeback/retirement");
}

void writeback_callback(std::uint32_t operation, bool fail) {
  Device device;
  device.yield_write = 1;
  device.fail_write = fail ? 2 : 0;
  fixture::Program code;
  code.word(load);
  code.word(store);
  code.word(operation);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}},
                              {},
                              false,
                              {.owner = &device, .read = &Device::read, .write = &Device::write});
  attach(machine, device);
  machine.prepare(false);
  auto registers = machine.core->registers();
  registers.cpsr = flags;
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  registers.system.data_fault_status = data_status;
  registers.system.instruction_fault_status = instruction_status;
  registers.system.fault_address = far;
  machine.core->set_registers(registers);
  (void)machine.core->run(2);
  const auto before = machine.core->state();
  const auto saved = machine.core->save();
  fixture::require(device.words[0] == device_base_value && device.writes.empty(),
                   "write-back hit is not yet a bus store");
  device.prefixes.clear();
  const auto result = machine.core->run(2);
  auto expected = before;
  if (fail) {
    expected.execution = armv5::ExecutionState::fault;
    expected.fault = result.fault;
    fixture::require(result.fault.kind == armv5::Fault::Kind::memory &&
                         result.fault.address == second_bus_word,
                     "cache writeback refusal retains its physical bus diagnostic");
  } else {
    expected.registers.r[15] = code.pc();
    const bool flags_read = operation != clean;
    if (flags_read) { expected.registers.cpsr = cleaned_status; }
    expected.progress.instructions += 1;
    expected.progress.base_cycles += flags_read ? 4 : 1;
    expected.execution = armv5::ExecutionState::yielded;
  }
  fixture::require(machine.core->state() == expected && device.words[0] == replacement &&
                       device.writes.size() == (fail ? 2 : 4),
                   "cache maintenance has exact successful/failed CPU and bus prefixes");
  for (const auto &observed : device.prefixes) {
    fixture::require(
        observed == before,
        "writeback callback sees state before maintenance retirement and flags update");
  }
  machine.core->restore(saved);
  device.writes.clear();
  device.prefixes.clear();
  const auto replay = machine.core->run(2);
  fixture::require(machine.core->state() == expected && replay.progress == result.progress &&
                       device.writes.size() == (fail ? 2 : 4),
                   "snapshot restores dirty halves after later clean/failure");
}

void write_through_swap() {
  Device device;
  device.yield_write = 1;
  fixture::Program code;
  code.word(load);
  code.word(swap);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}},
                              {},
                              false,
                              {.owner = &device, .read = &Device::read, .write = &Device::write});
  attach(machine, device);
  machine.first(32, through);
  machine.prepare(false);
  auto registers = machine.core->registers();
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  machine.core->set_registers(registers);
  (void)machine.core->run(1);
  const auto before = machine.core->state();
  device.prefixes.clear();
  auto expected = before;
  expected.registers.r[3] = device_base_value;
  expected.registers.r[15] = code.pc();
  expected.execution = armv5::ExecutionState::yielded;
  expected.progress.instructions += 1;
  expected.progress.base_cycles += 2;
  const auto result = machine.core->run(2);
  fixture::require(
      machine.core->state() == expected && result.execution == expected.execution &&
          device.words[0] == replacement && device.reads.size() == 8 && device.writes.size() == 1 &&
          device.prefixes[0] == before,
      "WT cache-hit SWP probe does not overwrite the old value before an accepted yielding store");
}

void physical_tags() {
  constexpr std::uint32_t invalidate_tlb = 0xee080f17;
  constexpr std::uint32_t invalidate_cache = 0xee070f36;
  constexpr std::uint32_t alternate_section = 0x00300c3e;
  constexpr std::uint32_t other = 0x55667788;
  constexpr std::uint32_t third_load = 0xe5903000;
  constexpr std::uint32_t fourth_load = 0xe5904000;
  fixture::Program code;
  for (const auto word :
       {load, store, invalidate_tlb, clean, third_load, invalidate_cache, fourth_load}) {
    code.word(word);
  }
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}});
  machine.context.control |= 4;
  machine.first(32, cacheable);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), device_base_value);
  fixture::put(std::span(machine.alternate).subspan(0x4000, 4), other);
  constexpr std::uint32_t continuation = 0x100c;
  machine.entries.push_back(continuation);
  const std::array transitions{armv5::CodeTransition{
      .source_variant = 0, .destination_variant = 0, .address = continuation}};
  machine.prepare(true, transitions);
  auto registers = machine.core->registers();
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  machine.core->set_registers(registers);
  (void)machine.core->run(2);
  machine.first(32, alternate_section);
  fixture::require(machine.core->run(1).execution == armv5::ExecutionState::transfer,
                   "TLB maintenance revokes mapping proof");
  (void)machine.core->install(machine.prepared(), 0);
  (void)machine.core->run(2);
  fixture::require(fixture::get(std::span(machine.data).subspan(0x4000, 4)) == replacement &&
                       fixture::get(std::span(machine.alternate).subspan(0x4000, 4)) == other &&
                       machine.core->registers().r[3] == replacement,
                   "VIVT hit retains old cached data after PA remap; clean uses saved physical tag "
                   "without a TLB walk");
  (void)machine.core->run(2);
  fixture::require(machine.core->registers().r[4] == other,
                   "cache invalidation makes the remapped physical page visible");
}

void set_way(bool all_locked, bool round_robin) {
  constexpr std::uint32_t clean_wrong = 0xee073f5a;
  constexpr std::uint32_t clean_right = 0xee074f5a;
  constexpr std::uint32_t invalidate_wrong = 0xee073f56;
  constexpr std::uint32_t invalidate_right = 0xee074f56;
  constexpr std::uint32_t fifth_load = 0xe5905000;
  constexpr std::uint32_t sixth_load = 0xe5906000;
  constexpr std::uint32_t way_two = 0x80000000;
  constexpr std::uint32_t way_three = 0xc0000000;
  constexpr std::uint32_t lockdown_two = 0xfffb;
  constexpr std::uint32_t lockdown_all = 0xffff;
  constexpr std::uint32_t next_value = 0x55667788;
  fixture::Program code;
  for (const auto word : {load,
                          store,
                          clean_wrong,
                          clean_right,
                          invalidate_wrong,
                          fifth_load,
                          invalidate_right,
                          sixth_load}) {
    code.word(word);
  }
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}});
  machine.context.control |= 4 | (round_robin ? 0x4000 : 0);
  machine.first(32, cacheable);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), device_base_value);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.system.data_cache_lockdown = all_locked ? lockdown_all : lockdown_two;
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  registers.r[4] = all_locked ? way_three : way_two;
  machine.core->set_registers(registers);
  (void)machine.core->run(2);
  const auto backing = machine.data;
  (void)machine.core->run(1);
  fixture::require(machine.data == backing,
                   "set/way clean of another way does not write a dirty line");
  (void)machine.core->run(1);
  fixture::require(fixture::get(std::span(machine.data).subspan(0x4000, 4)) == replacement,
                   "lockdown forces the eligible way, or way three when every L bit is set");
  fixture::put(std::span(machine.data).subspan(0x4000, 4), next_value);
  (void)machine.core->run(2);
  fixture::require(machine.core->registers().r[5] == replacement,
                   "set/way invalidation of another way retains the cached line");
  (void)machine.core->run(2);
  fixture::require(machine.core->registers().r[6] == next_value,
                   "set/way invalidation of the actual way refills external bytes");
}

void tcm_writeback() {
  constexpr std::uint32_t enable_tcm = 0xee093f11;
  constexpr std::uint32_t fourth_load = 0xe5904000;
  constexpr std::uint32_t disabled_region = 0x0020400c;
  constexpr std::uint32_t enabled_region = 0x0020400d;
  constexpr std::uint32_t continuation = 0x100c;
  constexpr std::uint32_t tcm_word = 0xa5a5a5a5;
  fixture::Program code;
  for (const auto word : {load, store, enable_tcm, clean, fourth_load}) { code.word(word); }
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}});
  std::vector<std::byte> dtcm(4096, std::byte{poison_byte});
  machine.regions.push_back({.address = 0, .bytes = dtcm, .space = armv5::Memory::Space::dtcm});
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  machine.context.control |= 4;
  machine.context.data_tcm = disabled_region;
  machine.first(32, cacheable);
  auto enabled = machine.mapping();
  enabled.data_tcm = enabled_region;
  const std::array<std::uint32_t, 1> source_entries{code.address()};
  const std::array<std::uint32_t, 1> target_entries{continuation};
  const std::array variants{
      armv5::CodeVariant{
          .id = 0, .spans = machine.spans, .entries = source_entries, .mapping = machine.mapping()},
      armv5::CodeVariant{
          .id = 1, .spans = machine.spans, .entries = target_entries, .mapping = enabled}};
  const std::array transitions{armv5::CodeTransition{
      .source_variant = 0, .destination_variant = 1, .address = continuation}};
  const armv5::CodeImage image{.identity = "authored-cache-tcm-writeback",
                               .variants = variants,
                               .layouts = {},
                               .transitions = transitions,
                               .interrupt_vectors = {}};
  const auto native = armv5::Core::compile(image);
  auto registers = machine.core->registers();
  registers.system.mapping = machine.mapping();
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  registers.r[3] = enabled_region;
  registers.r[15] = code.address();
  machine.core->set_registers(registers);
  (void)machine.core->install(native, 0);
  fixture::require(machine.core->run(3).execution == armv5::ExecutionState::transfer,
                   "TCM enable selects the declared cache context");
  (void)machine.core->install(native, 1);
  const auto tcm_before = dtcm;
  (void)machine.core->run(2);
  fixture::require(machine.core->registers().r[4] == tcm_word && dtcm == tcm_before &&
                       fixture::get(std::span(machine.data).subspan(0x4000, 4)) == replacement,
                   "clean uses the physical bus tag despite new TCM overlay; subsequent data read "
                   "gives TCM priority");
}

void eviction(bool locked) {
  constexpr std::uint32_t load_two = 0xe5943000;
  constexpr std::uint32_t load_three = 0xe5953000;
  constexpr std::uint32_t load_four = 0xe5963000;
  constexpr std::uint32_t load_five = 0xe5973000;
  constexpr std::uint32_t lock_way_zero = 0xee098f10;
  constexpr std::uint32_t load_again = 0xe5909000;
  constexpr std::uint32_t rr_bit = 0x4000;
  constexpr std::uint32_t locking = 0xfff1;
  fixture::Program code;
  for (const auto word : {load, store, load_two, load_three, load_four}) { code.word(word); }
  if (locked) { code.word(lock_way_zero); }
  code.word(load_five);
  code.word(load_again);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}});
  machine.context.control |= 4 | rr_bit;
  machine.first(32, cacheable);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  for (unsigned index = 4; index < 8; ++index) {
    registers.r[index] = fixture::MmuMachine::data_virtual + ((index - 3) * 0x800);
  }
  registers.r[8] = locking;
  machine.core->set_registers(registers);
  (void)machine.core->run(5);
  fixture::require(fixture::get(std::span(machine.data).subspan(0x4000, 4)) == 0,
                   "four ways coexist without writing back the first dirty line");
  if (locked) { (void)machine.core->run(1); }
  const auto saved = machine.core->save();
  fixture::put(std::span(machine.data).subspan(0x4000, 4), 15);
  (void)machine.core->run(2);
  fixture::require(fixture::get(std::span(machine.data).subspan(0x4000, 4)) ==
                           (locked ? 15U : replacement) &&
                       machine.core->registers().r[9] == replacement,
                   "RR eviction cleans a dirty victim and skips a guest-locked way");
  const auto after = machine.core->state();
  machine.core->restore(saved);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), 15);
  (void)machine.core->run(2);
  fixture::require(
      machine.core->state() == after &&
          fixture::get(std::span(machine.data).subspan(0x4000, 4)) == (locked ? 15U : replacement),
      "snapshot restores way contents, locks and replacement continuation without later residue");
}

void code_alias(bool partial, bool declared, bool big) {
  constexpr std::uint32_t alias = 0x01001000;
  constexpr std::uint32_t alias_section = 0x00100c3e;
  constexpr std::uint32_t store_alias = 0xe5802014;
  constexpr std::uint32_t load_four = 0xe5904000;
  constexpr std::uint32_t move_five = 0xe3a05003;
  constexpr std::uint32_t move_old = 0xe3a06001;
  constexpr std::uint32_t move_new = 0xe3a06009;
  constexpr std::uint32_t move_seven = 0xe3a07004;
  constexpr std::uint32_t loop = 0xeafffffe;
  constexpr std::uint32_t continuation = 0x100c;
  constexpr std::uint32_t failed_bus = 0x101018;
  fixture::Program code(0x1000, big);
  for (const auto word :
       {load, store_alias, clean, load_four, move_five, move_old, move_seven, loop}) {
    code.word(word);
  }
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}}, {}, big);
  machine.context.control |= 4;
  machine.first(16, alias_section);
  machine.regions[0].writable = true;
  if (partial) {
    auto tail = machine.regions[0];
    tail.address += protected_tail_offset;
    tail.bytes = tail.bytes.subspan(protected_tail_offset);
    tail.writable = false;
    machine.regions[0].bytes = machine.regions[0].bytes.first(protected_tail_offset);
    machine.regions.push_back(tail);
  }
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  auto bytes = code.bytes();
  fixture::put(std::span(bytes).subspan(replacement_word_offset, 4), move_new, big);
  auto changed = machine.spans;
  changed[0].bytes = bytes;
  const std::array<std::uint32_t, 1> source_entries{code.address()};
  const std::array<std::uint32_t, 1> target_entries{continuation};
  const std::array variants{
      armv5::CodeVariant{
          .id = 0, .spans = machine.spans, .entries = source_entries, .mapping = machine.mapping()},
      armv5::CodeVariant{
          .id = 1, .spans = changed, .entries = target_entries, .mapping = machine.mapping()}};
  const std::array transitions{armv5::CodeTransition{
      .source_variant = 0, .destination_variant = 1, .address = continuation}};
  const armv5::CodeImage image{.identity = "authored-cache-code-alias",
                               .variants = variants,
                               .layouts = {},
                               .transitions =
                                   declared ? std::span<const armv5::CodeTransition>(transitions)
                                            : std::span<const armv5::CodeTransition>{},
                               .interrupt_vectors = {}};
  const auto native = armv5::Core::compile(image);
  auto registers = machine.core->registers();
  registers.system.mapping = machine.mapping();
  registers.r[0] = alias;
  registers.r[2] = move_new;
  registers.r[15] = code.address();
  registers.system.data_fault_status = data_status;
  registers.system.fault_address = far;
  machine.core->set_registers(registers);
  (void)machine.core->install(native, 0);
  (void)machine.core->run(2);
  fixture::require(machine.code[0] == code.bytes(),
                   "dirty DCache code data is not yet visible to instruction fetch");
  const auto result = machine.core->run(2);
  fixture::require(machine.code[0] == bytes && machine.core->registers().r[6] == 0 &&
                       machine.core->registers().system.data_fault_status == data_status &&
                       machine.core->registers().system.fault_address == far,
                   "clean through a virtual alias changes backing code before any successor "
                   "executes, preserving FSRs");
  if (partial) {
    fixture::require(result.fault.kind == armv5::Fault::Kind::memory &&
                         result.fault.address == failed_bus &&
                         result.progress == armv5::Progress{} &&
                         machine.core->registers().r[15] == code.address() + 8,
                     "partial non-abortable cache clean refuses as a host fault after earlier "
                     "writes, before retirement");
    return;
  }
  fixture::require(result.progress == armv5::Progress{.instructions = 1, .base_cycles = 1} &&
                       machine.core->registers().r[15] == continuation,
                   "code-changing clean retires once at the declared continuation");
  if (!declared) {
    fixture::require(result.fault.kind == armv5::Fault::Kind::changed_code &&
                         result.fault.address == alias + replacement_word_offset,
                     "cache code-writeback diagnostic identifies the modified virtual alias");
    return;
  }
  fixture::require(result.execution == armv5::ExecutionState::transfer,
                   "code-changing clean takes a declared transition");
  const auto before = machine.core->state();
  const auto saved = machine.core->save();
  (void)machine.core->install(native, 1);
  fixture::require(machine.core->state() == before,
                   "changed-code install does not alter CPU/cache state or time");
  (void)machine.core->run(3);
  const auto after = machine.core->state();
  fixture::require(after.registers.r[6] == 9,
                   "changed successor executes only the prepared destination variant");
  machine.core->restore(saved);
  (void)machine.core->run(3);
  fixture::require(machine.core->state() == after,
                   "pending code-change snapshot restores cache and transfer without residue");
}

struct Maintenance {
  std::uint32_t word;
  bool writes, invalidates, test;
};

void maintenance(Maintenance operation) {
  constexpr std::uint32_t right_way = 0x80000000;
  constexpr std::uint32_t eligible_way = 0xfffb;
  constexpr std::uint32_t third_load = 0xe5903000;
  fixture::Program code;
  code.word(load);
  code.word(store);
  code.word(operation.word);
  if (operation.test) { code.word(operation.word); }
  code.word(third_load);
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}});
  machine.context.control |= 4;
  machine.first(32, cacheable);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  registers.r[4] = right_way;
  registers.system.data_cache_lockdown = eligible_way;
  machine.core->set_registers(registers);
  (void)machine.core->run(2);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), 15);
  (void)machine.core->run(operation.test ? 2 : 1);
  fixture::require(
      fixture::get(std::span(machine.data).subspan(0x4000, 4)) ==
          (operation.writes ? replacement : 15U),
      "clean writes dirty data; invalidation discards it; drain does not clean the DCache");
  fixture::put(std::span(machine.data).subspan(0x4000, 4), 11);
  (void)machine.core->run(1);
  fixture::require(machine.core->registers().r[3] == (operation.invalidates ? 11U : replacement),
                   "all/MVA/set-way/test operations retain or discard cached bytes as defined");
}

void rebind() {
  constexpr std::uint32_t third_load = 0xe5903000;
  fixture::Program code;
  for (const auto word : {load, store, third_load, clean}) { code.word(word); }
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}});
  machine.context.control |= 4;
  machine.first(32, cacheable);
  fixture::put(std::span(machine.data).subspan(0x4000, 4), device_base_value);
  fixture::put(std::span(machine.alternate).subspan(0x4000, 4), 15);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[0] = fixture::MmuMachine::data_virtual;
  registers.r[2] = replacement;
  machine.core->set_registers(registers);
  (void)machine.core->run(2);
  const auto before = machine.core->state();
  const auto saved = machine.core->save();
  for (auto &region : machine.regions) {
    if (region.address == fixture::MmuMachine::data_physical) { region.bytes = machine.alternate; }
  }
  machine.core->set_memory({.regions = machine.regions, .generation = 1});
  (void)machine.core->install(machine.prepared(), 0);
  fixture::require(machine.core->state() == before,
                   "rebind/install preserves guest state and hidden dirty cache");
  (void)machine.core->run(2);
  fixture::require(
      machine.core->registers().r[3] == replacement &&
          fixture::get(std::span(machine.alternate).subspan(0x4000, 4)) == replacement &&
          fixture::get(std::span(machine.data).subspan(0x4000, 4)) == device_base_value,
      "cache stores physical tags and bytes, not stale consumer host pointers");
  const auto after = machine.core->state();
  machine.core->restore(saved);
  (void)machine.core->run(2);
  fixture::require(machine.core->state() == after,
                   "snapshot rollback leaves rebound consumer storage in place");
}

void fcse_operands() {
  constexpr std::uint32_t pid = 0x04000000;
  constexpr std::uint32_t address = 0x00804000;
  constexpr std::uint32_t mva = 0x04804000;
  constexpr std::uint32_t code_section = 0x00100c12;
  constexpr std::uint32_t clean_modified = 0xee073f3a;
  constexpr std::uint32_t invalidate_original = 0xee070f36;
  constexpr std::uint32_t invalidate_modified = 0xee073f36;
  constexpr std::uint32_t load_five = 0xe5905000;
  constexpr std::uint32_t load_six = 0xe5906000;
  fixture::Program code;
  for (const auto word : {load,
                          store,
                          clean,
                          clean_modified,
                          invalidate_original,
                          load_five,
                          invalidate_modified,
                          load_six}) {
    code.word(word);
  }
  fixture::MmuMachine machine({{.program = code, .modes = {fixture::supervisor_mode}}});
  machine.context.control |= 4;
  machine.context.process_id = pid;
  machine.first(64, code_section);
  machine.first(fcse_data_root, cacheable);
  machine.prepare();
  auto registers = machine.core->registers();
  registers.r[0] = address;
  registers.r[2] = replacement;
  registers.r[3] = mva;
  machine.core->set_registers(registers);
  (void)machine.core->run(3);
  fixture::require(fixture::get(std::span(machine.data).subspan(0x4000, 4)) == 0,
                   "cache maintenance consumes an MVA without applying FCSE to a raw VA operand");
  (void)machine.core->run(1);
  fixture::require(fixture::get(std::span(machine.data).subspan(0x4000, 4)) == replacement,
                   "the actual FCSE MVA selects the dirty cache line");
  fixture::put(std::span(machine.data).subspan(0x4000, 4), 15);
  (void)machine.core->run(2);
  fixture::require(machine.core->registers().r[5] == replacement,
                   "wrong-process MVA invalidation preserves the line");
  (void)machine.core->run(2);
  fixture::require(machine.core->registers().r[6] == 15,
                   "FCSE MVA invalidation exposes external bytes");
}

} // namespace

void cache_lifecycle() {
  read_fill(0);
  read_fill(1);
  read_fill(10);
  for (const auto operation : {clean, test_clean_pc, test_invalidate_pc}) {
    writeback_callback(operation, false);
    writeback_callback(operation, true);
  }
  write_through_swap();
  physical_tags();
  tcm_writeback();
  eviction(false);
  eviction(true);
  rebind();
  fcse_operands();
  constexpr std::array operations{
      Maintenance{.word = 0xee070f16, .writes = false, .invalidates = true, .test = false},
      Maintenance{.word = 0xee070f36, .writes = false, .invalidates = true, .test = false},
      Maintenance{.word = 0xee074f56, .writes = false, .invalidates = true, .test = false},
      Maintenance{.word = clean, .writes = true, .invalidates = false, .test = false},
      Maintenance{.word = 0xee074f5a, .writes = true, .invalidates = false, .test = false},
      Maintenance{.word = 0xee070f3e, .writes = true, .invalidates = true, .test = false},
      Maintenance{.word = 0xee074f5e, .writes = true, .invalidates = true, .test = false},
      Maintenance{.word = test_clean_pc, .writes = true, .invalidates = false, .test = true},
      Maintenance{.word = test_invalidate_pc, .writes = true, .invalidates = true, .test = true},
      Maintenance{.word = 0xee070f9a, .writes = false, .invalidates = false, .test = false}};
  for (const auto operation : operations) { maintenance(operation); }
  for (const bool big : {false, true}) {
    code_alias(false, false, big);
    code_alias(false, true, big);
    code_alias(true, false, big);
  }
  for (const bool all_locked : {false, true}) {
    for (const bool round_robin : {false, true}) { set_way(all_locked, round_robin); }
  }
}
