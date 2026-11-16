# Target architecture

This is the implementation target, not a completeness claim. A1 provides the
public Core API, preparation decoder and strict build targets. A2 now has direct
SSA LLVM execution and cacheable preparation. A3 adds specialized mode units,
banked registers, precise exception entry/return and checked K6 event load;
A4 adds classic Thumb/interworking, integer/DSP multiplication and all six
checked kernels. Profile completion remains open. See
[isa-coverage.md](isa-coverage.md) and [api.md](api.md) for current evidence.

```text
consumer: CodeImage + CompileOptions -> Core::lower -> SemanticImage (LLVM bitcode)
consumer: link device IR, optimize, cache -> Core::bind -> NativeImage
consumer: Core::install on a stopped owner -> run / run_until -> save / restore
```

The interface, lifecycle, verification layers and benchmarks are defined in
[core-contract.md](core-contract.md) and are not repeated here.

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

The consumer declares executable code: spans, instruction entries, variants,
interrupt vectors, transitions and memory layouts. The core decodes that declared
code once during `lower`, normalizes encodings into reviewed architectural
operations and keeps the semantic order of old reads, computation and writes.
Nothing is decoded, compiled or looked up on an admitted execution path. Real
device callbacks are external effects. A code or mapping change through any view
revokes the binding; execution continues only after a declared variant is
installed. Record code identities and dependencies by explicit generation, never
by hashing guest memory at run time.

### Lowering rules, measured on cpu12 and dsp56300

These rules come from whole-instrument measurements of the Blofeld emulator
(Weichwolf/blofeld-src issues #32, #34, #35). Start with them.

1. **Lower every instruction directly to LLVM IR.** Architectural registers are
   SSA values inside a translation unit, loaded at entry and stored at real
   exits. Do not clone an interpreter step per instruction and rely on the
   optimizer to dissolve it: dsp56300 did, and it cost 54 to 62 host
   instructions per guest instruction.
2. **One lowering path for the whole instruction set.** No fast subset beside a
   slow general path. In dsp56300 the fast path covered two thirds of the
   executed instructions and the remaining third carried most of the cost,
   including the hottest loops.
3. **Account time and budget per basic block.** Admit the worst acyclic path
   at unit entry and backwards transfers; forward heads reuse that proof.
   Indirect transfers conservatively include every forward head. The exact
   per-instruction path finishes a nearby bound. Counters combine per block.
4. **Compute status bits where they can be observed.** A later instruction or an
   exit observes them; sticky bits accumulate in one value. In the dsp56300
   per-instruction path 33 of 39 host instructions of an ADD were status work.
5. **One processor or numeric mode per prepared unit.** Never emit all modes side
   by side at each site and select at run time. dsp56300 produced 27.8 MB of
   native text that way, and on x86 half of the pipeline slots were lost to
   instruction delivery.
6. **Mark exits cold.** Fault, refusal, bound and unprepared paths carry branch
   weights and stay out of line, so the executed code is dense.
7. **State lives in registers, not in core memory.** Guest RAM is consumer
   storage reached through declared layouts; the processor state is not written
   back between instructions.
8. **Cacheable preparation and complete save and restore from the first usable
   Core.** Both are contract requirements and both were expensive to retrofit.
9. **Idle loops are data.** A firmware loop pass that changes nothing can be
   retired arithmetically up to the run bound; keep the lowering able to prove a
   pass is a fixed point (blofeld-src #35).

Hoist checks only with a valid alias or permission proof. Keep the host stack
bounded across guest loops. Host SIMD is used only where guest dependencies and
the numerical contract are proved and the benefit is measured.

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

Follow sections 7 and 8 of the contract. Three test layers only: `test/isa`
(independently authored programs with independently derived expectations,
organized by operation class, addressing mode and processor or numeric mode),
`test/core` (the public interface and lifecycle: run bounds, faults, interrupts,
code change, transitions, save and restore, artifact load) and `test/bench`
(kernels K1 to K6, which also check complete output). Every test drives the
public interface with declared, installed images. There is no single-instruction
capture entry point and no test of internal structures; cpu12 and dsp56300 had
both and had to park 291 test files when those entry points were removed.

`test/support/isa.hpp` is an independently authored fixture builder with explicit
instruction fields, not a wrapper around the production decoder. Include encoding
collisions, rejected forms and precise partial exits. An instruction form that
is not lowered refuses preparation with identity, address and reason; that
refusal is itself tested.

Write the benchmark kernels early, K6 (a kernel under a run bound every 256 base
cycles with periodic interrupts) first. Report host instructions and host cycles
per guest instruction for each kernel with every increment that touches lowering.
No real-time, cycle-accuracy or cross-host claim follows from configuration alone.

The first implementation increment adds real build targets, the public headers
of the contract and independent fixtures together; no empty library and no fake
passing test.
