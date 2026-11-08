# Target JIT architecture

This is the implementation target, not a claim of existing code. The initial
repository contains project instructions, architecture/verification plans and
reference materials. No Core API, decoder, interpreter or JIT is implemented yet.

```text
consumer-owned memory and I/O -> Core -> retained native regions and loops
                                           |                 |
                                      direct RAM      event / I/O / fault
```

## Ownership and public surface

Expose only include/armv5/core.hpp and memory.hpp when implementation starts.
Use namespace armv5 and CMake target armv5::core. Keep LLVM types and native-code
details private. Core owns architectural state, progress, failure context and
translation lifetime. Consumers own storage, device models, clocks and scheduling.
One owner executes and mutates a Core. Separate cores can execute independently;
cross-core synchronization belongs to the consumer.

Execution, reset and memory mutation must have explicit ownership and reentry
rules. State inspection returns complete architectural values only when requested;
it must not force state materialization on every guest instruction. Expose useful
fault context and precise guest prefixes at observable exits.

## Translation and memory

Decode from actual guest entries. Normalize encodings into reviewed architectural
operations and retain the semantic order of old reads, computation and writes.
Lower ordinary operations natively through private LLVM 19 infrastructure on
both hosts. Real device callbacks are external effects; generic ISA helper gaps
must not masquerade as ordinary native coverage.

Compile natural regions and loops without fixed instruction-count caps. Retain
immutable native translations and link successors. Execution grants and deadlines
do not change block identities. Preserve precise interrupt/fault exits without a
per-instruction dispatcher. Keep registers live, RAM direct and cold failures out
of ordinary control flow. Hoist checks only with a valid alias/permission proof.

Record code identities, executable mappings and dependencies. Writes through data
aliases, owner mutations and guest architectural cache maintenance must invalidate
or retire affected native translations correctly. Use explicit generation changes
and ownership; do not continuously hash guest memory. Handle native-code lifetime
and host instruction-cache synchronization correctly on both hosts.

## ARM926 profile

- ARMv5TEJ, ARM state and classic Thumb state; preserve PC-visible pipeline offsets,
  interworking, conditional instruction effects and multiword Thumb BL semantics.
- Preserve R0..R15, banked SP/LR and FIQ registers, CPSR, every applicable SPSR,
  processor modes, flags, exception masks and vector behavior.
- Cover shifts at boundary counts, carry/borrow/overflow, saturating DSP operations,
  multiply variants, block transfers, writeback overlap and ARMv5 alignment rules.
  Implement guest endianness independently of host endianness.
- CP15/MMU behavior includes translation descriptors, domains, permissions, faults,
  FCSE, TLB maintenance and code coherency. Follow the ARM926 manual for the
  chosen revision; do not copy ARMv7 descriptor or barrier semantics.
- Model defined coprocessor handshakes/absence and precise undefined-instruction
  behavior. Optional accelerators, Jazelle execution and debug features require
  explicit profile capabilities and documented behavior; no silent fallback.
- Programmed I/O, interrupt controllers, timers, DMA, TI PRUs and physical board
  timing belong to consumers. An uncertain Nord TI part number does not prevent
  implementing this generic documented core profile.

Full ARMv5 architecture support is the family direction; the first concrete
acceptance profile is ARM926EJ-S. Do not imply every ARM9 derivative is identical.

## Verification and performance

Use test/isa for encodings and semantics, test/core for architectural boundaries
and memory, test/jit for generated-code/ownership behavior, and test/bench for
continuous workloads. test/support/isa.hpp will be an independently authored
fixture builder with explicit instruction fields, not a wrapper around the
production decoder. Expected outputs need independent mathematical or documented
derivations. Include encoding collisions, rejected forms and precise partial exits.

First compare correctness, then measure Release builds against matched handwritten
host ASM with equal work and checked outputs. Record cold translation, warm runs,
code size, event-boundary cost, worst cases and numerical modes. Host SIMD is an
optimization only where architectural identity and measured benefit are established.
No real-time, cycle-accuracy or cross-host claims follow from configuration alone.

Build implementation targets with C++23 and LLVM 19 using CMake/Ninja release and
strict presets; add separate thread sanitizer coverage for concurrent code. Keep
format/static checks strict and functions <=120 lines. The first implementation
increment must add real build targets and independent fixtures together; this
documentation seed intentionally contains no empty library or fake passing ISA test.
