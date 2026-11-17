# A2 benchmark baseline

Measured on 2026-11-16 on Linux x86-64, Intel Core i5-10400, six physical cores, SMT disabled. The owner thread was pinned to logical CPU 2. Clang and LLVM 19.1.7; C++23 Release (`-O3 -DNDEBUG`, default x86-64 compiler target). The standalone engine uses LLVM O3 and the detected native target. No clock factor or consumer throughput is inferred.

Each kernel warms once, checks every complete output, then reports the median of three measured runs. User-only hardware instructions and cycles come from a perf_event_open group on the owner thread; interval-specific enabled/running times account for multiplexing. Preparation and output checks are outside the enabled window. The C++ reference is separately compiled and uses the same modulo-2^32 algorithm. Definitions and exact output checks are in [test/bench/README.md](../test/bench/README.md).

| Kernel | Retired guest instructions | Host instructions / guest | Host cycles / guest | Guest M instructions/s | Native / Clang O3 time | Preparation s | Native text bytes | Text bytes / prepared instruction |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K4 | 14,680,064 | 19.411 | 5.078 | 800.461 | 20.440x | 0.654482 | 22,523 | 395.140 |
| K5 | 10,488,320 | 22.398 | 5.674 | 708.712 | 107.575x | 0.048816 | 3,485 | 316.818 |

Text density divides all generated text, including cold exits and bound continuations, by unique declared guest instructions (K4: 57; K5: 11). The measured native/C++ ratio is elapsed-time slowdown, not a speedup. This baseline exposes remaining guard/status and code-density costs; it is not a final performance qualification. At A2, K1–K3/K6 and macOS ARM64 measurements remained open.

Reproduce after the Release build:

```sh
taskset -c 2 build/armv5_bench
```

Preparation/compiler identity:

```text
armv5-fa2444d99af8fd1b889f64eeb0f2c1306f590b52a56874bd7aca2f83dece02af-llvm-19.1.7-x86_64-pc-linux-gnu
```

## A3 benchmark baseline

Same host, build and warm measurement procedure as A2; K6 includes owner bound
and IRQ delivery in the enabled interval. These are independent new measurements,
not an optimization claim. Full output checks pass for all three kernels.

| Kernel | Retired guest instructions | Host instructions / guest | Host cycles / guest | Guest M instructions/s | Native / Clang O3 time | Preparation s | Native text bytes | Text bytes / prepared instruction |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K4 | 14,680,064 | 18.946 | 5.014 | 795.392 | 20.008x | 0.804198 | 27,003 | 473.737 |
| K5 | 10,488,320 | 20.598 | 5.076 | 785.841 | 95.271x | 0.049989 | 3,795 | 345.000 |
| K6 | 10,756,398 | 28.089 | 7.757 | 513.707 | 31.075x | 0.081947 | 6,360 | 302.857 |

K6 prepares 21 unique guest instructions across Supervisor and IRQ units and
checks 4,167 complete interrupts. The larger cold state/continuation paths remain
included in native text size. K1–K3, profile completion and macOS ARM64 remain open.

```text
armv5-185a3bb65092d97345a6415343076c45c5a9088c2455cde1f2ff8eeba18b3480-llvm-19.1.7-x86_64-pc-linux-gnu
```

## A3 K4 hot-code attribution before optimization

The emitted x86-64 code was traced over an interior, repeating cycle through all
eight handlers (112 guest instructions, 2,122 executed host instructions).
Preparation used the public semantic image and the same LLVM O3/LLJIT pipeline
as the standalone engine. GDB instruction stepping established the taken paths;
manual attribution assigned every executed instruction once. Entry/exit, cold
bounds and callbacks are outside this steady cycle. This counts host instructions,
not cycles, and is separate from the hardware-counter benchmark.

| Category | Host instructions / 112 guest instructions | Host instructions / guest | Share |
|---|---:|---:|---:|
| Required guest operation | 144 | 1.286 | 6.8% |
| Memory guard and address translation | 584 | 5.214 | 27.5% |
| Status flags | 176 | 1.571 | 8.3% |
| Block admission and accounting | 624 | 5.571 | 29.4% |
| Transfer between blocks | 594 | 5.304 | 28.0% |
| Total | 2,122 | 18.946 | 100.0% |

Required operations include guest address arithmetic, actual loads/stores and
link-register values. Memory includes layout/permission/alias checks, host pointer
formation and legacy word alignment/rotation. Transfer includes indirect-target
validation/dispatch, PHI register moves/spills and executed alignment NOPs. Flags
include NZCV materialization for SUBS; admission includes instruction/cycle budget
proofs and counters. Bounds, memory guards and transfers dominate this workload;
this table establishes attribution before any performance optimization.

## A4 and contract v6

K1/K2/K6 now use the common Q1.15 definition and complete-result hashes from
contract v6. The earlier A3 K6 row above records a superseded workload and cannot
be compared as the same numerical kernel. K4/K5 retain their definitions. The
host and measurement procedure are unchanged; `kernel.perf_event_paranoid=2`
permits this owner's user-only hardware counter group. macOS ARM64 remains
untested.

The bounded lowering experiments used the same A4 instruction implementation
and the same independent tests and v6 kernel inputs throughout:

1. Admission proves the maximum instruction/cycle cost of a forward-only DAG at
   native entry and backwards transfers. Forward heads share the proof; an
   indirect transfer conservatively includes every forward head. The exact
   instruction continuation remains available near either bound. All registers
   remain SSA across the admitted region. Conservative backwards-return checks
   remain a substantial cost.
2. Call-free guest control flow and loops already occupy one LLVM function per
   mode/state, with SSA edges. Adding jump threading with duplication threshold
   128 followed by InstCombine/SimplifyCFG after LLVM O3 produced identical
   instruction counts and text sizes on all six kernels; those extra passes
   were discarded. K4/K5 have no hot native-unit crossings. K6 crosses modes at
   externally observed event boundaries; ordinary register materialization and
   bank changes remain necessary there. Passing state through a new register
   ABI was not implemented. Indirect dispatch and SSA register moves remain open.
3. Fixed-layout checks use one unsigned offset comparison, including underflow
   and narrow-region rejection. Install forms an integer host-address bias once
   per region. The native access adds the guest address after its range proof.
   Disjoint writable non-executable storage is checked before other regions;
   permissions, alias-write revocation and dynamic fallback are preserved.
   K4/K5 do not contain repeated accesses off one unchanged base inside their
   hot natural blocks, so no new block-wide guard grouping was introduced.

The first and third changes are retained. These experiments reduce bookkeeping;
they do not establish a final performance qualification. Source-level guard
proofs and observed instruction attribution are different measures.

| Stage | Kernel | Host I/GI | Host cycles/GI | Native text bytes | Bytes/prepared GI |
|---|---|---:|---:|---:|---:|
| Baseline | K4 | 20.795 | 5.413 | 26,985 | 473.421 |
| Baseline | K5 | 20.398 | 5.114 | 3,767 | 342.455 |
| Baseline | K6 | 27.557 | 7.747 | 7,104 | 236.800 |
| Admission | K4 | 16.759 | 4.682 | 22,408 | 393.123 |
| Admission | K5 | 19.997 | 5.007 | 3,399 | 309.000 |
| Admission | K6 | 26.829 | 7.330 | 7,022 | 234.067 |
| Transfer trial (discarded) | K4 | 16.759 | 4.683 | 22,408 | 393.123 |
| Transfer trial (discarded) | K5 | 19.997 | 5.014 | 3,399 | 309.000 |
| Transfer trial (discarded) | K6 | 26.829 | 7.274 | 7,022 | 234.067 |
| Memory (retained) | K4 | 13.866 | 3.372 | 23,073 | 404.789 |
| Memory (retained) | K5 | 15.398 | 3.804 | 3,359 | 305.364 |
| Memory (retained) | K6 | 20.910 | 6.100 | 6,795 | 226.500 |

Attribution uses the same GDB/LLVM O3 method as A3. Each row below counts one
complete interior eight-handler cycle: 112 guest instructions. All executed
host instructions are assigned exactly once, including alignment NOPs and
register spills. Actual operations include guest address arithmetic, loads,
stores, link constants and the loop subtraction; status computation excludes
copies used only to move SSA register values. Native allocation can change
category counts even when a category's source is untouched. The rejected second
experiment has the same measured instruction counts and text sizes as admission;
its K4 object text section matches admission byte-for-byte before relocation,
so the same trace attribution applies.

| Category | After admission: count / I per GI / share | After transfer trial | After memory: count / I per GI / share |
|---|---:|---:|---:|
| Required guest operation | 128 / 1.143 / 6.8% | unchanged | 128 / 1.143 / 8.2% |
| Memory guard and address formation | 618 / 5.518 / 32.9% | unchanged | 311 / 2.777 / 20.0% |
| Block admission and accounting | 408 / 3.643 / 21.7% | unchanged | 408 / 3.643 / 26.3% |
| Transfer between blocks | 563 / 5.027 / 30.0% | unchanged | 546 / 4.875 / 35.2% |
| Status flags | 160 / 1.429 / 8.5% | unchanged | 160 / 1.429 / 10.3% |
| Total | 1877 / 16.759 / 100% | unchanged | 1553 / 13.866 / 100% |

Complete final kernel measurements follow. Prepared native text includes cold
paths, exact continuations and every declared mode/state unit. The instruction
fraction executed in prepared native code is 100% for every kernel. Per-sample
figures include block loops and, for K6, events; K3's unit is an FFT sample, K4's
is a state transition and K5's is a copied word. `ns/GI` uses elapsed time, while
cycles/GI uses the hardware counter.

| Kernel | Guest instructions | Guest I/sample | Guest cycles/sample | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s |
|---|---:|---:|---:|---:|---:|---:|---:|
| K1 | 88,867,584 | 339.002930 | 531.002922 | 17.080 | 4.480 | 1.098378 | 910.433 |
| K2 | 61,080,320 | 233.002930 | 261.002922 | 6.167 | 1.654 | 0.407856 | 2451.845 |
| K3 | 19,822,080 | 151.230469 | 161.222641 | 6.370 | 1.648 | 0.408141 | 2450.131 |
| K4 | 14,680,064 | 14.000000 | 25.999998 | 13.866 | 3.372 | 0.837472 | 1194.069 |
| K5 | 10,488,320 | 5.001221 | 7.001220 | 15.398 | 3.804 | 0.930478 | 1074.716 |
| K6 | 88,935,668 | 339.262650 | 531.911942 | 20.910 | 6.100 | 1.497434 | 667.809 |

| Kernel | Native / Clang O3 time | Preparation s | Native text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|
| K1 | 13.887x | 0.083961 | 5,898 | 210.643 |
| K2 | 2.987x | 0.249453 | 16,168 | 234.319 |
| K3 | 1.090x | 0.198696 | 13,110 | 208.095 |
| K4 | 13.581x | 0.682638 | 23,073 | 404.789 |
| K5 | 71.619x | 0.038289 | 3,359 | 305.364 |
| K6 | 19.065x | 0.092074 | 6,795 | 226.500 |

Compiler identity:

```text
armv5-f240439caa21978131a8c78f7b813decf16d4f24cf1d6a2ff8f6057bc0e07698-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Clang 19 Release build, all three public-interface suites,
clang-format and the configured clang-tidy checks; an exported source tree
builds and tests with no manuals, sibling repositories or network inputs. An
installed-package consumer executes both ARM and Thumb. Additional independent
memory fixtures check byte/word reads and writes on both sides of region bounds,
32-bit address wrap, regions narrower than a word and unchanged fault prefixes
through fixed and dynamic paths. CP15/MMU/FCSE/TLB/TCM, complete profile
qualification, CoreMark and final sanitizer qualification remain open.

Published increment totals: `src/` 3,529 lines (+673), `test/` 3,966 (+1,664)
against A3. Counts include all versioned text under those directories.


## Forward leaf calls after A4

Forward ARM BL calls with a straight-line leaf body use caller-specific SSA
edges. The same instruction emitter lowers the specialized body, using the
caller's existing forward-path admission proof. The actual LR or loaded return
PC is compared with the caller-produced link; a mismatch leaves through a cold
owner exit after ordinary target/interworking/variant checks. The shared body
and exact continuations remain available at every original guest PC. Callees
containing branches, loops or nested calls, and backwards callees, retain the
existing prepared control flow.

Same host and warm measurement method as A4. Independent new fixtures cover
BX LR, MOV PC,LR, LDR PC and LDM PC returns, changed LR/stack values, ARM/Thumb
interworking, undeclared destinations, every call-prefix instruction bound,
intervening cycle deadlines and snapshot replay, plus exact callback/yield
prefixes inside a specialized body.
The same fixtures also pass against the unchanged A4 library.

| K4 metric | A4 | Forward leaf calls |
|---|---:|---:|
| Required operation, I/GI | 1.143 (128/112) | 0.982 (110/112) |
| Memory guards/address, I/GI | 2.777 (311/112) | 2.911 (326/112) |
| Admission/accounting, I/GI | 3.643 (408/112) | 3.500 (392/112) |
| Transfer, I/GI | 4.875 (546/112) | 3.563 (399/112) |
| Status, I/GI | 1.429 (160/112) | 1.429 (160/112) |
| Total host I/GI | 13.866 (1553/112) | 12.384 (1387/112) |
| Hardware cycles/GI | 3.372 | 3.198 |
| Native text bytes | 23073 | 21726 |
| Bytes/prepared instruction | 404.789 | 381.158 |

Attribution uses an interior eight-handler cycle and the same native stepping
and category definitions as A4. Constant propagation of the specialized callee
and merged counter updates contribute to the gain. The native total matches the
hardware instruction count; allocation changes shift some work into memory
address preparation. Transfer and admission remain substantial costs.

All six complete-output checks, including the v6 Q1.15 hashes, pass. The columns
below retain the full contract report; native guest-instruction share is 100%.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 17.080 | 4.479 | 1.098501 | 910.331 | 13.848x | 0.081590 | 5,898 | 210.643 |
| K2 | 6.167 | 1.683 | 0.413109 | 2420.671 | 3.061x | 0.249729 | 16,168 | 234.319 |
| K3 | 6.370 | 1.646 | 0.404109 | 2474.579 | 1.080x | 0.193779 | 13,110 | 208.095 |
| K4 | 12.384 | 3.198 | 0.782026 | 1278.731 | 12.794x | 0.679967 | 21,726 | 381.158 |
| K5 | 15.398 | 3.813 | 0.932634 | 1072.231 | 71.806x | 0.038056 | 3,359 | 305.364 |
| K6 | 20.910 | 6.039 | 1.482416 | 674.575 | 18.862x | 0.091132 | 6,795 | 226.500 |

Guest instructions/base cycles per sample are unchanged from the A4 table.

```text
armv5-f95190e84f36053b62f02152d508e230de1e01249f2f0593dea2c83ee1ca28c9-llvm-19.1.7-x86_64-pc-linux-gnu
```

`src/`: 3,657 lines (+128 against A4).

`test/`: 4,182 lines (+216 against A4).

Validation: strict Release and all three suites, configured static analysis,
formatting, offline source export and an installed SDK consumer executing
ARM/Thumb and repeated static calls. This experiment does not complete the
profile; the open work in `isa-coverage.md` remains.


## PC-load completion before callback yield

A successful yielding ARM LDR/LDM PC or Thumb POP PC now completes its branch
before returning. LDM with SPSR restore also completes the status and bank change.
State-changing branches retain destination validation at the bound; exception
returns may resume a saved Thumb BL suffix. No destination instruction executes
before the yield. Failed callbacks still leave through the existing access-fault
path before retirement.

Independent public-interface fixtures check full state, both LDM callback
prefixes, writeback, alignment, interworking, SPSR/FIQ bank restoration, rejected
BL suffix entry, missing targets, instruction/cycle bounds and snapshot replay.
The new regression fails on the preceding library. Strict build, all three
suites, configured static analysis/format, offline source build and installed SDK
consumer pass. The standalone installed PC-yield reproducer now returns the
aligned Thumb target after exactly one instruction/five cycles.

Same host, warm method and per-thread hardware counters as above. All six
complete outputs pass; guest instructions/base cycles per sample are unchanged.
The small K6 instruction-count increase is owner exit handling; K4 allocation
changes with the additional cold branch paths. This is a correctness fix, with
no further transfer tuning.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 17.080 | 4.479 | 1.098361 | 910.447 | 12.854x | 0.081362 | 5,898 | 210.643 |
| K2 | 6.167 | 1.677 | 0.410754 | 2434.548 | 3.045x | 0.248028 | 16,168 | 234.319 |
| K3 | 6.370 | 1.644 | 0.401997 | 2487.580 | 1.073x | 0.194724 | 13,110 | 208.095 |
| K4 | 12.321 | 3.130 | 0.765446 | 1306.427 | 12.523x | 0.724187 | 21,799 | 382.439 |
| K5 | 15.398 | 3.802 | 0.953119 | 1049.187 | 71.612x | 0.039611 | 3,359 | 305.364 |
| K6 | 20.936 | 6.090 | 1.493577 | 669.534 | 18.869x | 0.092169 | 6,795 | 226.500 |

```text
armv5-e9c3f85aebdfc3d15e8218f7d270481dc80a7a8d6ef06ba8e9dea5a5754f8192-llvm-19.1.7-x86_64-pc-linux-gnu
```

Published increment totals: `src/` 3,710 lines (+53), `test/` 4,326 (+144)
against the static leaf-call increment. Profile completion and macOS ARM64
qualification remain open.


## Counted-loop turn grant

Fixed-cost natural blocks ending in unconditional SUBS counter,counter,#1 and
BNE head receive a whole-turn grant from the remaining bounds and counter.
A zero counter represents 2^32 turns before the loop naturally finishes.
The eligible body has no other counter or status dependencies: non-S independent
ALU/MUL/MLA and single transfers qualify. Other shapes retain ordinary prepared
flow. The same emitter executes every real body instruction. Cold callback,
fault, yield and code-change exits reconstruct the exact preceding turns,
counter and SUBS flags; original entries and exact tails finish partial turns.
No runtime grant continuation or instruction-count cutoff is introduced.

The one bounded experiment compares the preceding PC-yield increment with this
change. Same host, full-output checks and warm hardware measurement method.

| Metric | K1 before | K1 grant | K5 before | K5 grant |
|---|---:|---:|---:|---:|
| Whole-kernel hardware I/GI | 17.080 | 7.941 | 15.398 | 7.808 |
| Whole-kernel hardware cycles/GI | 4.479 | 2.151 | 3.802 | 2.059 |
| Interior turn native instructions / 5 GI | 89 | 38 | 77 | 39 |
| Interior admission/accounting I/GI | 3.600 (18/5) | 0.800 (4/5) | 3.600 (18/5) | 0.800 (4/5) |
| Native text bytes | 5,898 | 8,097 | 3,359 | 5,424 |
| Bytes/prepared GI | 210.643 | 289.179 | 305.364 | 493.091 |

Native instruction stepping checks repeated interior turns; the emitted objects
match the measured text sizes. Admission/accounting here counts the bound tests,
progress updates and loop decision, excluding grant setup/finalization and guest
counter arithmetic. It is an interior-turn attribution, not a whole-kernel
attribution. The full hardware totals include all setup, finalization and tails.
There are no per-turn run-bound checks inside a grant. The IR accounts the grant
as turns times cost at exit, but LLVM 19 strength-reduces cold prefix expressions
into two progress recurrences in the native loop. The measured four instructions
are those two updates plus compare/branch. No extra pass tuning was added.
Status reconstruction also leaves the ordinary loop body and contributes to the
gain. Native code grows because the grant and original resumable paths coexist.

Independent fixtures cover every short instruction/cycle prefix with fixed and
dynamic memory; zero, one, small, signed-boundary and wrapping counters; exact
flags; callback observation/yield/refusal after a preceding turn; code alias
revocation during the second turn; and snapshot replay. The same fixtures pass
on the preceding library. All three suites, configured static analysis/format,
offline source export and installed SDK ARM/Thumb/call/counted-loop consumer pass.
K6 retains the complete Q1.15 output hash, all 34,042 IRQs and 256-cycle bounds.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 7.941 | 2.151 | 0.526421 | 1899.622 | 6.710x | 0.109421 | 8,097 | 289.179 |
| K2 | 6.167 | 1.704 | 0.416768 | 2399.414 | 3.091x | 0.247634 | 16,168 | 234.319 |
| K3 | 5.408 | 1.413 | 0.345733 | 2892.404 | 0.919x | 0.232102 | 16,833 | 267.190 |
| K4 | 12.321 | 3.134 | 0.766658 | 1304.363 | 12.526x | 0.728462 | 21,799 | 382.439 |
| K5 | 7.808 | 2.059 | 0.516448 | 1936.305 | 39.242x | 0.075814 | 5,424 | 493.091 |
| K6 | 12.378 | 3.744 | 0.934480 | 1070.114 | 11.909x | 0.117793 | 8,772 | 292.400 |

All guest instructions/base cycles per sample are unchanged from A4; native
guest-instruction share remains 100%.

```text
armv5-b51947cc288d967fe589d2bc4816779c50ec4f3c8642dd05d5c81565b3c313ac-llvm-19.1.7-x86_64-pc-linux-gnu
```

Published increment totals: `src/` 3,877 lines (+167), `test/` 4,607 (+281)
against the PC-yield increment. A5/profile completion, CoreMark and final
sanitizer qualification remain open. macOS ARM64 remains untested.


## Initial A5 register and storage context

Contract v7 supplies processor-private storage through address-space regions.
The first A5 increment adds CP15 register state, lazy MRC field loads,
register-only MCR exits, context-checked variants and disabled TCM storage
geometry/lifecycle. Active MMU/TCM/cache/alignment/high-vector control remains an
explicit refusal; this is not A5 completion or a memory-system benchmark claim.

Same host and warm hardware method as above; every complete output passes.
K1–K5 instruction counts and all six generated text sizes are unchanged. K1/K5
consumer-bound object `.text` is byte-identical to the preceding increment
(8,097/5,424 bytes), checked independently of compiler/input identities. No CP15
field loads were added to their generated loops.

K6 rises from 12.378 to 12.426 I/GI in the full public-interface event workload.
Its frequent `state()` inspection now returns the additional CP15 fields; those
copies are visible in the host assembly. The larger observable state therefore
has a measurable owner cost even though generated guest text is unchanged.
There is no claim of zero aggregate K6 regression. Native code and processor
state must be distinguished from the consumer's explicit full-state inspection.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 7.941 | 2.151 | 0.526721 | 1898.538 | 6.711x | 0.108874 | 8,097 | 289.179 |
| K2 | 6.167 | 1.674 | 0.415080 | 2409.174 | 3.033x | 0.248915 | 16,168 | 234.319 |
| K3 | 5.408 | 1.424 | 0.348233 | 2871.642 | 0.933x | 0.227581 | 16,833 | 267.190 |
| K4 | 12.321 | 3.131 | 0.765624 | 1306.123 | 12.521x | 0.730008 | 21,799 | 382.439 |
| K5 | 7.808 | 2.087 | 0.514140 | 1944.994 | 39.206x | 0.066339 | 5,424 | 493.091 |
| K6 | 12.426 | 3.835 | 0.954833 | 1047.304 | 12.171x | 0.118172 | 8,772 | 292.400 |

Guest instructions/base cycles per sample are unchanged; native guest share is
100%. K6 still checks 34,042 IRQs under 256-cycle bounds and the Q1.15 K1 hash.

```text
armv5-247e501d6b24dc85c092c67cc327b130b3a29824004e3cd2df0efe61460b5c8b-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, all three suites, configured clang-tidy/format,
offline source export and an installed SDK consumer including CP15 register
access. Independent ISA and lifecycle fixtures are in `test/isa/system.cpp` and
`test/core/system.cpp`; they cover both byte orders, exact privilege/absent
coprocessor traps, software state/readback, snapshots, geometry and binding/cache
identity refusals. No manufacturer program bytes are used.

Published increment totals: `src/` 4,146 lines (+269), `test/` 5,018 (+411)
against the loop-grant increment. A5 memory-system completion, CoreMark, final
sanitizers and macOS ARM64 qualification remain open.


## Guest CP15 mapping writes and active TCM

Guest control/TTBR/domain/FCSE/TCM-region writes now canonicalize architectural
fields and exit after the completed instruction. Changed assumptions revoke the
binding and use a declared context transition; unchanged writes continue. Active
ITCM/DTCM use consumer-owned regions, with direct descriptors specialized to the
known bases. Bus proofs exclude active overlays. DTCM never supplies instruction
fetch; executable validation splits spans at ITCM boundaries. Code changes through
actual storage aliases still stop at the complete store boundary.

Independent fixtures cover both byte orders, fixed/bus-only/dynamic layouts,
atomic exchanges, top/zero bases, all nine legal TCM sizes, code-source aliases,
context masks, multiple explicitly declared destinations selected by actual CP15
state (with equivalent/undeclared contexts rejected), exact instruction/cycle bounds, invalid region writes, pending
transition snapshots and disable/relocation/re-enable with retained storage.
Unpredictable TCM writes fail before retirement. Ignored writes to read-only size
bits retain an unchanged binding. An installed SDK consumer enables DTCM via a
guest MCR/declared transition and reads its distinct data under the new context.

All six complete outputs and text sizes match the preceding register/storage
increment. K1–K5 hardware instruction counts are unchanged. After adding
state-selected transition alternatives, K6 is 12.445 I/GI versus 12.426 before:
a small owner-side increase, with unchanged generated guest text. No aggregate
zero-regression claim follows from the native-text comparison. K1/K5 object `.text` is again
byte-identical (8,097/5,424 bytes). Ordinary disabled-TCM units carry no extra
overlay or system-register checks. Same host and warm counter method as above.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 7.941 | 2.152 | 0.526681 | 1898.683 | 6.669x | 0.110134 | 8,097 | 289.179 |
| K2 | 6.167 | 1.653 | 0.407228 | 2455.629 | 2.970x | 0.248030 | 16,168 | 234.319 |
| K3 | 5.408 | 1.418 | 0.346820 | 2883.340 | 0.916x | 0.227892 | 16,833 | 267.190 |
| K4 | 12.321 | 3.131 | 0.765772 | 1305.872 | 12.530x | 0.727865 | 21,799 | 382.439 |
| K5 | 7.808 | 2.060 | 0.507926 | 1968.791 | 34.594x | 0.072139 | 5,424 | 493.091 |
| K6 | 12.445 | 3.939 | 0.967005 | 1034.121 | 12.316x | 0.117856 | 8,772 | 292.400 |

Guest instructions/base cycles per sample are unchanged; native guest share is
100%. K6 retains the Q1.15 hash, 34,042 IRQs and 256-cycle bounds.

```text
armv5-a6c496ec643eb2ca86a96e03647fa974feebde302b5b41f53ad7be7bc3dff492-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, all three suites, configured tidy/format, offline
source export and installed SDK consumers. Published increment totals: `src/`
4,361 lines (+215), `test/` 5,815 (+797) against the register/storage increment.
A5 alignment/L4/high vectors, MMU/FCSE/TLB/cache execution, architectural
fault-register updates and remaining coprocessor/Jazelle qualification remain
open. CoreMark, final sanitizers and macOS ARM64 qualification are also pending.


## CP15 alignment, legacy PC loads and high vectors

Variants now specialize the A and L4 control bits, while the owner selects actual
exception vectors from V before checking declarations. A-bit alignment faults
leave before any access or writeback and update DFSR/FAR. Independent fixtures
cover all seven modes, both instruction sets and byte orders, fixed/dynamic
memory, predication, exact first addresses, loop-turn fault prefixes, callbacks,
SPSR returns and snapshot replay. Guest MCR can transition to a context with all
four supported control bits A/B/V/L4 set.

All complete kernel outputs and generated text sizes are unchanged from the TCM
increment. K1–K5 retain their rounded hardware instruction counts. Disabled
alignment checking adds no generated guards. K6 measures 12.446 I/GI versus
12.445 previously, including owner high-vector selection on each IRQ; the native
guest text remains 8,772 bytes. These are full-interface measurements, with no
claim of zero owner overhead. Same CPU 2, i5-10400, Clang/LLVM 19.1.7 Release,
warm-up and median-of-three user hardware-counter method as above;
`perf_event_paranoid=2`.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 7.941 | 2.153 | 0.530717 | 1884.243 | 6.761x | 0.109810 | 8,097 | 289.179 |
| K2 | 6.167 | 1.671 | 0.409812 | 2440.145 | 3.036x | 0.248281 | 16,168 | 234.319 |
| K3 | 5.408 | 1.419 | 0.347174 | 2880.402 | 0.921x | 0.228494 | 16,833 | 267.190 |
| K4 | 12.321 | 3.136 | 0.766902 | 1303.947 | 12.548x | 0.727820 | 21,799 | 382.439 |
| K5 | 7.808 | 2.060 | 0.510792 | 1957.742 | 39.275x | 0.074928 | 5,424 | 493.091 |
| K6 | 12.446 | 3.838 | 0.955273 | 1046.822 | 11.915x | 0.118313 | 8,772 | 292.400 |

Guest instructions/base cycles per sample are unchanged; native guest share is
100%. K6 retains the Q1.15 K1 hash, 34,042 IRQs and 256-cycle bounds.

```text
armv5-6cd1e5cf3dd6313d3cdba74d031fcdb97851dd5d2180124589fc9843f9b528d2-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, all three suites, configured tidy/format, source-only
export build and installed SDK consumers. The new standalone consumer uses only
the two installed public headers, takes an alignment abort to a high vector and
reads DFSR through guest MRC. Published increment totals: `src/` 4,390 lines
(+29), `test/` 6,434 (+619) against the active-TCM increment. No existing semantic
golden was weakened; the legacy PC-yield cases add explicit L4 expectations.
MMU/FCSE/TLB/cache execution and maintenance, external/debug/prefetch fault-status
qualification and remaining coprocessor/Jazelle behavior remain open. CoreMark,
final sanitizers and macOS ARM64 qualification are pending. This is not A5
completion.


## External data-abort fault registers

MMU-disabled external data aborts now record DFSR status 8 and the ARM926 FAR
value. The prepared instruction retains original-MVA and transfer geometry;
only a real external-abort edge evaluates burst-end/1KB-boundary behavior and
writes fault registers. A distinct physical-abort signal keeps host callback
refusals and future MMU fault classifications separate. IFSR is preserved.

Independent fixtures check all seven modes, both byte orders, fixed/dynamic
memory, original byte offsets, byte/halfword/signed/word transfers, SWP/SWPB,
doubleword and multiple transfers. Explicit expected FAR tables cover failures
before and after a 1KB boundary and the top of the address space. Complete
register/memory prefixes, base restoration, fault-register MRC readback and
snapshots are checked. A denied second loop turn retains its exact counter,
flags and progress. Existing range and code-write abort fixtures keep their
checks and add fault-register expectations. The new fixtures fail against the
preceding published library at the missing DFSR/FAR update.

All six full outputs/hashes and K6's 34,042 IRQs/256-cycle bounds pass. Default
fixed-layout instruction counts regress for K1, K3, K5 and K6; K1 is 8.289 I/GI
versus 7.941 previously. Cold fault bookkeeping changes generated code; exact host-instruction
attribution has not been established. This
is **not** a no-regression result. Fixed-mapping performance remains an A5
validation item. The benchmark and its numerical contract are unchanged.

Same CPU 2, i5-10400, Clang/LLVM 19.1.7 Release and warm median-of-three user
hardware-counter method as above, with `perf_event_paranoid=2`.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 8.289 | 2.235 | 0.548762 | 1822.282 | 6.989x | 0.114370 | 8,144 | 290.857 |
| K2 | 6.172 | 1.676 | 0.409896 | 2439.644 | 3.011x | 0.257613 | 17,876 | 259.072 |
| K3 | 5.646 | 1.507 | 0.371956 | 2688.490 | 0.994x | 0.250251 | 17,081 | 271.127 |
| K4 | 12.205 | 2.974 | 0.727484 | 1374.600 | 11.903x | 0.730439 | 24,758 | 434.351 |
| K5 | 8.007 | 2.213 | 0.555039 | 1801.674 | 41.690x | 0.077857 | 5,526 | 502.364 |
| K6 | 12.742 | 3.978 | 0.975487 | 1025.129 | 12.421x | 0.121762 | 9,030 | 301.000 |

Guest instructions/base cycles per sample are unchanged; native guest share is
100%. No K4 call/ABI tuning was performed.

```text
armv5-3afce20c64b5ebb84c88c7a222dc3e20841be2851c749234bd045202cd3f7e45-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, all three suites, configured tidy/format, source-only
export build and installed SDK consumers. The new consumer uses only the two
public headers, partially loads a four-word transfer, observes FAR at the final
burst word instead of the failing second word and reads it through guest MRC;
snapshot restore retains the exact abort state. The export's build inputs and
the SDK library were compared byte-for-byte with the final implementation.
Published totals: `src/` 4,433 (+43), `test/` 6,810 (+376) against the A/L4/V
increment. MMU/FCSE/TLB/cache execution and maintenance, debug/prefetch status
qualification and remaining coprocessor/Jazelle behavior are still open.
CoreMark, final sanitizers and macOS ARM64 qualification remain pending.
This is not A5 completion.


## MMU, FCSE and unified TLB

Prepared M=1 variants now execute section/coarse/fine translations, large/small/
tiny pages and mixed subpage permissions. Domain/AP/S/R checks, FCSE, precise
data/prefetch fault classes, physical TCM selection, unified TLB invalidation and
eight lockdown slots execute through the same instruction lowering. No decode,
compilation or code hashing occurs at run time. M=1 memory/fetch helpers preserve
SSA values on RAM/TLB paths and materialize only for real callbacks. Installation
peeks without filling or calling I/O; snapshots retain hidden translations and
replacement continuation. Physical code remaps revoke even when bytes match.

Independent public-interface fixtures cover all seven modes for section loads,
both byte orders, fixed/dynamic layouts, a domain/AP/S/R privilege truth table,
LDRT/STRT, all page table shapes, FCSE and TCM routing, precise partial aborts,
first/second-level walk faults and instruction-fault priority. A review fixture
exposed lost low FAR bits after word alignment; all four byte offsets now retain
the original MVA on translation/domain/permission/walk faults. TLB tests cover
ordinary/locked fills, all invalidate aliases, victim wrap, mixed subpage ranges,
stale translations across reinstall and snapshots, callback prefix/yield/refusal,
ARM/Thumb PC-load completion, code aliases and equal/different-byte remaps.
Existing semantic expectations were retained.

The common K1–K6 programs still use the default M=0 fixed layouts. Their complete
outputs/hashes and K6's 34,042 IRQs/256-cycle bounds pass. All six native text sizes
are unchanged from the external-abort increment. K1–K5 retain their rounded
hardware instruction counts; K6 is 12.7424203 versus 12.7420375 I/GI. This measures
the common public-interface workload, not M=1 throughput or a no-regression
qualification for the entire A5 sequence. No K4/ABI tuning was performed.

Same CPU 2, i5-10400, Clang/LLVM 19.1.7 Release, warm median-of-three user
hardware-counter procedure, with `perf_event_paranoid=2` verified operational.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 8.289 | 2.235 | 0.548656 | 1822.637 | 6.956x | 0.115108 | 8,144 | 290.857 |
| K2 | 6.172 | 1.676 | 0.410270 | 2437.421 | 3.041x | 0.258299 | 17,876 | 259.072 |
| K3 | 5.646 | 1.487 | 0.355059 | 2816.434 | 0.939x | 0.247045 | 17,081 | 271.127 |
| K4 | 12.205 | 2.975 | 0.732649 | 1364.909 | 11.987x | 0.737480 | 24,758 | 434.351 |
| K5 | 8.007 | 2.240 | 0.547678 | 1825.889 | 41.854x | 0.106931 | 5,526 | 502.364 |
| K6 | 12.742 | 3.920 | 0.977073 | 1023.465 | 12.192x | 0.121551 | 9,030 | 301.000 |

Guest instructions/base cycles per sample are unchanged; native guest share is
100%. Compiler identity:

```text
armv5-c38b48b239f5800417c7e6ba727bcc34968d40b7c0840fef90887ba36ffc578a-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, all three suites in both the working tree and a
source-only export, configured tidy/format and five installed SDK consumers.
The new two-header consumer checks translated loads, stale PTEs, guest TLB
invalidation, a declared self-transition and hidden-TLB snapshot restoration.
All 56 build/source/test inputs match the tested export byte-for-byte; the SDK
library matches the final build. Published totals: `src/` 5,085 (+652), `test/`
8,129 (+1,319) against the external-abort increment.

Cache execution/maintenance and write-buffer ordering, remaining debug/coprocessor/
Jazelle behavior and exhaustive profile qualification remain open. Exact declared
mapping contexts remain required; general M=1 performance is unqualified. CoreMark,
final own/generated-code sanitizers and macOS ARM64 qualification remain pending.
This is not A5 completion.

## Absent coprocessors and illegal CP15 classes

All legal ARMv5 coprocessor classes now lower to precise Undefined when the
external slot (0–13) is absent. This includes the unconditional opcode space.
CP15's illegal classes use the same architectural exception path; CP14 stays
explicitly unimplemented. Preparation still rejects ARMv6 double-register
unconditional forms and malformed extension encodings. These classes reuse the
existing Undefined exit and owner exception entry.

Independent installed programs check all external numbers and the illegal CP15
classes, all seven modes, both byte orders and fixed/dynamic storage. Each case
checks a completed ADDS SSA prefix, true/false predicates or unconditional
execution, complete registers/banks/NZCVQ, exact LR/SPSR/vector/progress, exception
return, unchanged RAM/fault registers and zero callbacks. Memory forms include
long, pre/post-index, up/down, writeback, unindexed and literal encodings; even
unaligned/unmapped operands do not access memory. The new fixture refuses MCR2
preparation against the preceding published library. Existing semantic goldens
were retained.

Same CPU 2, i5-10400, Clang/LLVM 19.1.7 Release and warm median-of-three user
hardware-counter method as the MMU increment (`perf_event_paranoid=2`). All six
complete outputs/hashes, K6's 34,042 IRQs and 256-cycle bounds pass. Rounded host
instruction counts and native text sizes are unchanged; the measured cycle
counts are reported below, without a zero-regression claim. No overhead or
K4/ABI experiment was performed.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 8.289 | 2.234 | 0.546396 | 1830.175 | 6.953x | 0.114717 | 8,144 | 290.857 |
| K2 | 6.172 | 1.677 | 0.410865 | 2433.891 | 3.043x | 0.256928 | 17,876 | 259.072 |
| K3 | 5.646 | 1.497 | 0.366018 | 2732.103 | 0.979x | 0.250610 | 17,081 | 271.127 |
| K4 | 12.205 | 2.974 | 0.727439 | 1374.686 | 11.902x | 0.730506 | 24,758 | 434.351 |
| K5 | 8.007 | 2.210 | 0.553877 | 1805.454 | 41.322x | 0.073449 | 5,526 | 502.364 |
| K6 | 12.742 | 4.106 | 1.015657 | 984.584 | 12.935x | 0.122099 | 9,030 | 301.000 |

Guest instruction/base-cycle totals and the 100% native guest share are unchanged.
Compiler identity:

```text
armv5-fc7b94b9a3366e95cfad50c97f918d432b4eab80bdc7d384f7e0461008257a91-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, all three suites in the working tree and source-only
export, configured tidy/format, and six installed SDK consumers. All 57 build/
source/test inputs match the tested export byte-for-byte; the installed library
matches the final build. The new two-header consumer checks absent LDC and illegal
CP15 CDP2 after an ADDS prefix, fault-register preservation, zero callbacks and
snapshot restoration. Published totals: `src/` 5,083 (-2), `test/` 8,343 (+214)
against the MMU increment. Existing semantic goldens were retained.

A5 remains open for cache execution/maintenance, write-buffer ordering, internal
CP14/debug, Jazelle/BXJ and exhaustive profile qualification. CoreMark and final
own/generated-code sanitizers follow; macOS ARM64 remains untested.

## DCache execution, lockdown and maintenance

Prepared C/M-enabled variants now execute the ARM926 DCache: four-way 8KB storage,
32-byte lines, two 16-byte dirty halves, MVA lookup, saved physical writeback tags,
critical-word-first fills, WT/WB policies, nonallocating write misses, RR or
deterministic pseudo-random replacement and way lockdown. DCache c7 maintenance
includes all/MVA/Set-Way invalidation, clean, clean-and-invalidate, test-clean and
test-clean-invalidate. External buffered stores/writebacks complete synchronously;
drain preserves ordering and does not clean dirty cache lines. Bus queue timing
and cache miss penalties are outside the nominal cycle model.

The memory helper preserves SSA values on RAM/cache paths and uses a cold retry
for real callbacks. An earlier yield survives subsequent probes within a multiple
transfer. WT SWP preserves its old value across retry. Writebacks go to the saved
physical bus tag even after PTE/TLB/TCM changes. A code-changing clean revokes
before a successor; the first changed MVA survives later unchanged words. Partial
non-abortable writeback failures retain their host-memory diagnostic and completed
bus writes, without retiring the failed maintenance instruction or changing FSRs.
Hidden cache bytes, tags, dirty state and replacement continuation are snapshots;
rebind/install preserves them and reset invalidates them.

Independent public fixtures cover ARM/Thumb, every ARM mode, both byte orders,
fixed/dynamic storage, all M/C and page C/B policies, byte/half/signed/unaligned/
double/multiple/SWP forms, AP checks on hits, LDRT/STRT, false predicates,
critical-word-first callback order, partial failures/yield, every implemented
DCache maintenance operation, way selection/eviction, all-locked fallback,
FCSE MVA operands, physical remaps, TCM priority, rebind and pending code-change
snapshots. Reserved TTBR/PID assumptions refuse before native access. The old
lockdown-read golden was incorrect: DDI 0198E 2.3.10 requires bits 15:4 to read
as ones, so reset is `0xfff0`. The implementation and that expectation are
corrected together; legal guest read-modify-write and reset now verify the format.
Other existing semantic goldens are retained.

The common K1–K6 programs use M=0 fixed layouts. They gain no cache lookup; all
rounded host instruction counts and generated text sizes remain unchanged from
the preceding coprocessor increment. Complete outputs/hashes, 34,042 K6 IRQs and
256-cycle bounds pass. These are common-kernel measurements, not a DCache/M=1
performance qualification or a zero-regression claim for the A5 sequence. No
K4/ABI or overhead experiment was performed.

Same CPU 2, i5-10400, Clang/LLVM 19.1.7 Release and warm median-of-three user
hardware-counter method, with `perf_event_paranoid=2`.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 8.289 | 2.238 | 0.554851 | 1802.285 | 6.978x | 0.118310 | 8,144 | 290.857 |
| K2 | 6.172 | 1.678 | 0.418573 | 2389.068 | 3.035x | 0.269517 | 17,876 | 259.072 |
| K3 | 5.646 | 1.495 | 0.370699 | 2697.605 | 0.984x | 0.259281 | 17,081 | 271.127 |
| K4 | 12.205 | 2.977 | 0.738957 | 1353.258 | 11.344x | 0.793675 | 24,758 | 434.351 |
| K5 | 8.007 | 2.241 | 0.548008 | 1824.792 | 42.508x | 0.070591 | 5,526 | 502.364 |
| K6 | 12.742 | 3.982 | 0.997736 | 1002.269 | 12.400x | 0.125738 | 9,030 | 301.000 |

Guest instruction/base-cycle totals and the 100% native guest share are unchanged.
Compiler identity:

```text
armv5-4baaab8f3eedb71be1dde590c358f48a5a9c10c755db711a5a3f944e0e665532-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, all three suites in the working tree and source-only
export, configured tidy/format, and seven installed SDK consumers. All 61 build/
source/test inputs match the tested export byte-for-byte; the SDK library matches
the final build. The new two-header consumer checks dirty-data visibility, clean,
test/invalidate/refill, exact snapshot replay and the corrected reset format.
Published totals: `src/` 5,529 (+446), `test/` 9,604 (+1,261) against the
coprocessor increment.

ICache execution/maintenance, WFI, remaining debug/CP14/Jazelle behavior and the
exhaustive profile audit remain open. Exact declared mapping contexts remain
required; general M=1 performance is unqualified. CoreMark and final own/generated-
code sanitizers follow; macOS ARM64 remains untested. This is not A5 completion.

## Uncached SWP external-abort ordering

ARM926 DDI 0198E section 3.6 requires the write part of an uncached SWP even
when its read externally aborts. Writable RAM now receives that write before
Data Abort entry; the destination and instruction retirement stay unchanged.
NCNB and NCB use the same behavior. Code-alias writes retain their invalidation,
then enter Abort before a declared transition or a changed-code fault. Atomic
I/O remains one swap callback, and callback refusal remains a host memory fault.

The old read-denied SWP/SWPB RAM expectation incorrectly assumed no write. That
expectation and the implementation are corrected together against the primary
reference. Other abort expectations are retained. Independent fixtures cover
all seven modes, both byte orders, four byte offsets, word/byte transfers,
fixed/dynamic layouts, MMU-disabled and NCNB/NCB pages, every physical read/write
permission combination, exact DFSR/FAR/IFSR and progress, code-alias transitions
and snapshot replay. Translation/permission/alignment aborts still precede the
physical access.

K1–K6 complete outputs/hashes, 34,042 K6 IRQs and 256-cycle bounds pass. All
rounded host-instruction counts and native text sizes are unchanged from the
DCache increment. Same CPU 2, i5-10400, Clang/LLVM 19.1.7 Release and warm median
of three user hardware-counter measurements (`perf_event_paranoid=2`); these
common M=0 kernels do not measure the newly corrected abort path.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 8.289 | 2.248 | 0.565631 | 1767.938 | 6.805x | 0.128956 | 8,144 | 290.857 |
| K2 | 6.172 | 1.694 | 0.450718 | 2218.680 | 3.215x | 0.309415 | 17,876 | 259.072 |
| K3 | 5.646 | 1.497 | 0.376872 | 2653.421 | 0.976x | 0.298155 | 17,081 | 271.127 |
| K4 | 12.205 | 2.994 | 0.751930 | 1329.912 | 11.987x | 0.975845 | 24,758 | 434.351 |
| K5 | 8.007 | 2.231 | 0.559324 | 1787.873 | 42.203x | 0.077200 | 5,526 | 502.364 |
| K6 | 12.742 | 3.975 | 1.040800 | 960.800 | 12.889x | 0.140750 | 9,030 | 301.000 |

Guest instruction/base-cycle totals and 100% native guest share are unchanged.
Compiler identity:

```text
armv5-c1c432306f575ca387df41b1a4db071fa3c2a71bedc1c10d3df989c21c02ab2c-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, all three suites in the working tree and matching
source-only export, configured tidy/format and eight installed SDK consumers.
The added two-header consumer checks a read-aborted SWP's RAM write, unchanged
destination, original FAR, guest MRC readback and snapshot replay. Published
`src/` 5,535 (+6), `test/` 9,733 (+129) against the DCache increment.

A5 remains open for ICache execution/maintenance, WFI, remaining debug/CP14/
Jazelle behavior and exhaustive profile qualification. CoreMark and final
own/generated-code sanitizers follow; macOS ARM64 remains untested.

## Functional cache controls and code invalidation

The DCache-content implementation described in earlier sections has been
removed. `src/cache.*` and cache bytes, tags, dirty halves, replacement and
write-buffer state no longer enter execution, snapshots or rebinding. CP15
C/I/RR and c9 register behavior remain; c9 retains its corrected required-one
reset fields. Data clean/invalidate/drain operations leave consumer storage
unchanged, test-clean reports clean, and instruction prefetch retains no content.
ICache whole/combined, MVA and Set/Way invalidation revoke affected prepared
bindings through a cold exit and declared revalidation. MVA operands are already
modified; set/index coverage follows the reported 16KB, four-way ICache geometry.

Reserved TTBR/PID refusal, callback-yield preservation and SWP semantics remain.
A new uncached LDM callback fixture keeps the first yield across a second probe.
Functional cache fixtures replace the withdrawn content-policy tests. They check
register fields, direct consumer storage, no-content maintenance, flags, modes,
byte orders, layouts, page C/B attributes, affected code ranges, FCSE, predicates,
exact retirement, missing/declared transitions and snapshot replay.

Removal alone retained the earlier external-abort regression (K1 8.289 and
K5 8.007 I/GI). Sharing the cold DFSR/FAR stores in one private LLVM helper
restores K1/K5 to the requested range while retaining all independent abort and
ordering fixtures. Original guest instructions still lower directly to SSA;
there is no additional executor or runtime decoding. Generated native text grows
for some kernels as LLVM allocates the surrounding cold exits differently.

Same CPU 2, i5-10400, Clang/LLVM 19.1.7 Release and warm median-of-three user
hardware-counter method (`perf_event_paranoid=2`). The final timing run followed
completion of the parallel qualification jobs. All complete outputs/hashes,
34,042 K6 IRQs and 256-cycle bounds pass.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 7.923 | 2.148 | 0.528496 | 1892.163 | 6.735x | 0.117595 | 8,662 | 309.357 |
| K2 | 6.202 | 1.659 | 0.407588 | 2453.457 | 3.021x | 0.266303 | 18,503 | 268.159 |
| K3 | 5.387 | 1.428 | 0.349307 | 2862.814 | 0.926x | 0.270396 | 17,143 | 272.111 |
| K4 | 12.839 | 3.215 | 0.786573 | 1271.337 | 12.804x | 0.757069 | 25,206 | 442.211 |
| K5 | 7.607 | 2.126 | 0.533119 | 1875.754 | 39.986x | 0.070822 | 5,528 | 502.545 |
| K6 | 12.734 | 3.980 | 0.991134 | 1008.945 | 12.482x | 0.123443 | 9,262 | 308.733 |

Against pre-A5 `c112bcf`, K1 is 7.923 versus 7.941 I/GI and K5 is 7.607 versus
7.808. K3 is 5.387 versus 5.408. K2 (6.202 versus 6.167), K4 (12.839 versus
12.321) and K6 (12.734 versus 12.378) remain higher; this is not a blanket
no-regression result for A5. No K4 call/ABI tuning was performed. Guest
instruction/base-cycle totals and 100% native guest share are unchanged.
Compiler identity:

```text
armv5-b2406f50c9a1ccfe6127b7f9db5d262754890bb8112fb116e80c31d23c38fdd8-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, all three suites in the working tree and exact
source-only export, configured tidy/format and eight installed SDK consumers.
All 59 remaining build/source/test inputs match the tested export byte-for-byte;
the installed SDK archive matches the final build. The revised two-header cache
consumer verifies immediate storage effects, clean status, code invalidation,
revalidation and pending-transition snapshots. Existing ISA expectations outside
the withdrawn cache-content model remain intact, including SWP external-abort
write ordering and MRC R15 nominal timing from DDI 0165B table 8-2.
Published `src/` 5,226 (-309), `test/` 8,884 (-849) against `5937412`.

WFI, remaining CP14/debug/Jazelle behavior, exhaustive profile qualification and
the outstanding fixed-mapping performance differences remain open. CoreMark and
final own/generated-code sanitizers follow; macOS ARM64 remains untested.

## WFI sleep and event completion

Prepared privileged WFI now stops at its original PC without completing. DDI
0198E section 2.3.8 specifies completion on wake. Repeated sleeping runs retire
nothing and add no nominal wait cycles. Owner wake completes one instruction and
one base cycle; an eligible IRQ/FIQ completes it before exception entry, with
LR=WFI+8. Masked interrupt requests retain the sleep; the consumer signals physical
wake without vector entry through `resume_from_stop()`. The wake API is idempotent
outside a wait. Completion performed by the event API appears in cumulative
`State::progress`, while a run result contains only its own completed work.

Sleep, wake, snapshots, rebind/install and reset use the existing state interface.
No polling, device clocks, native code decoding or additional processor-state
buffer is introduced. A successful yielding MMU fetch can enter WFI's stopped
state; its completed TLB effects and unfinished wait survive snapshot replay.
User WFI takes Undefined; a false predicate retires normally in every mode.

Independent ISA fixtures cover modes, byte orders, fixed/dynamic layouts,
deadlines, repeated sleep, idempotent wake, snapshot replay and reset. The compact
core event fixture delivers alternating IRQ/FIQ after deadline runs, checking
complete state, ordered preceding stores, exact counts, banked status, LR/return,
masked requests, revalidation and replay. Existing bank/mask/high-vector generators
remain unchanged. Nine installed SDK consumers pass; the new two-header consumer
checks unfinished sleep, wake, IRQ entry/return and snapshots. This increment uses
contract v8, fast-forwarded from the shared contract-only commit.

Same CPU 2, i5-10400, Clang/LLVM 19.1.7 Release, warm median-of-three user hardware
counters (`perf_event_paranoid=2`). All six complete outputs/hashes, 34,042 K6 IRQs
and 256-cycle bounds pass. Native guest text sizes are unchanged from the
functional-cache increment. K1–K5 retain rounded instruction counts. K6 moves
from 12.733727 to 12.735258 I/GI, consistent with about four additional host
instructions per delivered IRQ for wake eligibility; no tuning was performed.
K5 was remeasured after an outlying portable-reference time; its latest checked
warm measurement is reported below.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 7.923 | 2.148 | 0.536783 | 1862.950 | 6.776x | 0.117981 | 8,662 | 309.357 |
| K2 | 6.202 | 1.658 | 0.406123 | 2462.311 | 2.974x | 0.265676 | 18,503 | 268.159 |
| K3 | 5.387 | 1.471 | 0.367929 | 2717.919 | 0.960x | 0.270817 | 17,143 | 272.111 |
| K4 | 12.839 | 3.218 | 0.791086 | 1264.085 | 12.925x | 0.767451 | 25,206 | 442.211 |
| K5 | 7.607 | 2.123 | 0.519282 | 1925.735 | 39.918x | 0.071049 | 5,528 | 502.545 |
| K6 | 12.735 | 3.916 | 0.973294 | 1027.439 | 12.408x | 0.124807 | 9,262 | 308.733 |

Guest instruction/base-cycle totals and 100% native share are unchanged. The
previous report retains the pre-A5 comparison; the noted K4/K6 differences remain
reported without further tuning. Compiler identity:

```text
armv5-93ccf9ca424c5f17c61a42f3e4d0927b77ebb7892a6824aa9ed1f428c1155cd8-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release, working-tree suites and source-only export, configured
tidy/format and nine installed SDK consumers. After reducing duplicate WFI bank/
vector fixture setup, core/bench were rerun and the exact final export passes all
three suites. All 60 build inputs and contract v8 match the export byte-for-byte;
the installed SDK archive matches the final build. Published `src/` 5,246 (+20),
`test/` 9,116 (+232) against the contract-v8 baseline `1bc0bfa`.

A5 remains open for the bounded trivial-Jazelle behavior, disabled-debug profile
and final coverage table. CoreMark and own/generated-code sanitizers follow A5;
macOS ARM64 remains untested.

## A5 functional-profile completion

A5 completes the declared functional ARM926EJ-S profile on Linux x86-64:
ARM/classic Thumb, all seven modes, exceptions, CP15/MMU/FCSE/TLB/TCM,
functional cache controls/code invalidation and WFI owner wake. Jazelle is the
architectural trivial implementation: BXJ behaves as BX without changing LR,
J is permanently zero and the ID/control registers read zero/ignore writes.
No Java bytecodes execute. Privileged CP14 debug transfers read zero/ignore
writes; the documented profile deviation is that the DCC W flag never sets.
User restrictions, unsupported-selector diagnostics and precise Undefined for
other legal CP14 classes remain. BKPT retains the existing Prefetch Abort path,
including unchanged IFSR/FAR, without internal halting.

The coverage table names an executing prepared-image fixture for every operation
and lifecycle row. It records capabilities and excluded electrical/debug/cache
content models explicitly; no exhaustive encoding Cartesian sweep is claimed.
The existing generators now include BXJ call/return/interworking, CP14 registers,
privilege and PSR J writes, unsupported CP14 classes, and a PLD no-access fixture.
Existing semantic goldens were retained.

Same i5-10400, CPU 2, Clang/LLVM 19.1.7 Release and warm median-of-three user-only
hardware-counter method (`perf_event_paranoid=2`). Measurements followed the
qualification jobs; all six complete outputs/hashes, 34,042 K6 IRQs and 256-cycle
bounds pass. Native guest share is 100%; guest instruction/base-cycle totals and
native text sizes are unchanged from the WFI increment. This measures the common
fixed-layout M=0 kernels; general M=1 throughput remains unqualified.

| Kernel | Host I/GI | Host cycles/GI | ns/GI | Guest M I/s | Native/C++ time | Preparation s | Text bytes | Bytes/prepared GI |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K1 | 7.923 | 2.147 | 0.525636 | 1902.457 | 6.688x | 0.118269 | 8,662 | 309.357 |
| K2 | 6.202 | 1.659 | 0.407726 | 2452.626 | 2.989x | 0.264939 | 18,503 | 268.159 |
| K3 | 5.387 | 1.427 | 0.349170 | 2863.932 | 0.923x | 0.270227 | 17,143 | 272.111 |
| K4 | 12.839 | 3.215 | 0.788767 | 1267.801 | 12.885x | 0.754597 | 25,206 | 442.211 |
| K5 | 7.607 | 2.117 | 0.517759 | 1931.402 | 40.311x | 0.092952 | 5,528 | 502.545 |
| K6 | 12.735 | 3.913 | 0.963125 | 1038.287 | 12.277x | 0.123153 | 9,262 | 308.733 |

The pre-A5 comparison remains explicit. K4/K6 increases accompany the MMU/abort
bookkeeping; they were not tuned during the bounded A5 completion.

| Kernel | Pre-A5 `c112bcf` I/GI | Final A5 I/GI | Change | Pre-A5 cycles/GI | Final A5 cycles/GI | Pre-A5 text bytes | Final A5 text bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| K1 | 7.941 | 7.923 | -0.22% | 2.151 | 2.147 | 8,097 | 8,662 |
| K2 | 6.167 | 6.202 | +0.56% | 1.704 | 1.659 | 16,168 | 18,503 |
| K3 | 5.408 | 5.387 | -0.38% | 1.413 | 1.427 | 16,833 | 17,143 |
| K4 | 12.321 | 12.839 | +4.21% | 3.134 | 3.215 | 21,799 | 25,206 |
| K5 | 7.808 | 7.607 | -2.57% | 2.059 | 2.117 | 5,424 | 5,528 |
| K6 | 12.378 | 12.735 | +2.89% | 3.744 | 3.913 | 8,772 | 9,262 |

Compiler identity:

```text
armv5-c3a3d908a297fdd6c09f6b4d87a8d9304b6a0ada652474b0f7425b0183a1a2b7-llvm-19.1.7-x86_64-pc-linux-gnu
```

Validation: strict Release; ISA/core/bench in the working tree and source-only
export; configured clang-tidy/clang-format; ten separately built installed SDK
consumers using only the two public headers. After the final two-line PLD fixture,
ISA was rerun in both builds; production, core and benchmark inputs were unchanged.
All 60 build/source/test inputs and contract v8 match the export byte-for-byte;
the installed SDK archive matches the final build. The tenth consumer checks
CP14 zero/ignored writes, BXJ to Thumb, unchanged LR, J=0 and snapshot replay.

Published totals: `src/` 5,281 (+35), `test/` 9,183 (+67) against `0a75d12`.
The complete bounded remainder after `d1ea576` is +55 production/+299 test lines,
including WFI. The full A5 series against pre-A5 `c112bcf` adds 1,404 production
and 4,576 test lines, counting all versioned text under `src/` and `test/`.

A5 is complete for the declared functional profile. Next, before CoreMark:
retain guest registers across units, provide full virtual address-space backing
including MMU views, then cross-unit flag liveness. Each optimization requires
all six checked outputs and a separate measured comparison. Own/generated-code
ASan/UBSan and separate TSan qualification remain pending. macOS ARM64 is untested;
no developer host was available. The overall work remains open for those steps.

## R1 register-retention qualification

The existing A5 mode/state functions already retain guest registers in SSA through
static edges and dynamic dispatch. Native stepping qualifies R1 with no production
change: register-field traffic is below one host instruction per guest instruction
in both K4 and K6. The next implementation step is full address-space backing/MMU
views, followed by cross-unit flag liveness.

Measured on the same Linux x86-64 i5-10400 and LLVM 19.1.7 Release as A5, using
`31c52bc`. The installed SDK archive and restored build archive are byte-identical
(SHA-256 `a5a5148b6bc818a73edf332bbff33af576af3a12c4698192280ced64e5d9a1ae`);
the benchmark executable is also byte-identical to the saved A5 executable.
The profiler uses public lower/bind/install/run APIs, the same LLVM O3/native
pipeline and the authored benchmark programs. GDB stepping observes the generated
code and owner calls without modifying the core or adding runtime instrumentation.
Raw disassembly, traces and profiling tools remain ignored local analysis.

A register-field instruction means an executed native instruction whose memory
operand touches this owner's Runtime R0–R15, CPSR/SPSR or banked registers.
Vector accesses count once per host instruction; progress, bounds, signals,
descriptors, consumer RAM and host stack spills are excluded. Effective addresses
were checked against the actual owner pointer and the field offsets of this build.
K4's cycle is a repeated 112-guest-instruction path through all eight handlers;
its 1,438 host instructions agree with the hardware-counter rate of 12.839 I/GI.
A complete 128-transition K4 run additionally includes owner entry/exit.
K6 covers 32 deadline grants at 256-cycle intervals, one IRQ and its return:
5,227 completed guest instructions and 8,193 base cycles (the final deadline
overshoots at a complete instruction). The K4 reference output and K6 complete RAM
prefix/IRQ count are checked independently after the traced window.

| Quantity | K4 steady cycle | K4 complete short run | K6 event window |
|---|---:|---:|---:|
| Completed guest instructions | 112 | 1,792 | 5,227 |
| Stepped host instructions | 1,438 | 23,378 | 66,045 |
| Native register-field instructions at ordinary owner entry/exit | 0 | 45 | 1,440 |
| Owner bookkeeping and explicit state inspection | 0 | 3 | 577 |
| IRQ eligibility, entry, return and bank/mode changes (including IRQ native entry/exit) | 0 | 0 | 58 |
| Register-field instructions at ordinary dynamic transfers | 0 | 0 | 0 |
| Register-field instructions inside straight translated flow | 0 | 0 | 0 |
| Register-field instructions at memory-helper/Io calls | 0 | 0 | 0 |
| Total register-field instructions | 0 | 48 | 2,075 |
| Register-field host instructions/GI | 0 | 0.026786 | 0.396977 |
| Generic dispatches from guest control | 16 | 256 | 0 |
| Routing instructions for those dispatches | 136 | 2,176 | 0 |
| Routing instructions/dynamic dispatch | 8.5 | 8.5 | — |
| Dynamic dispatches/GI | 0.142857 | 0.142857 | 0 |
| Owner/mode-entry PC dispatches | 0 | 1 | 33 |
| Routing instructions for owner/mode-entry dispatch | 0 | 9 | 258 |

Dispatch routing counts target-index formation, range checks, jump-table lookup
and the indirect jump. It excludes guest branch/interworking effects, link values,
accounting and PHI register moves. K4 uses eight 8-instruction BLX dispatches and
eight 9-instruction loaded-PC return dispatches per cycle. Eight additional guest
BX LR leaf returns use guarded static SSA continuations. K6 has no ordinary guest
indirect dispatch; its one exception return changes mode through the owner, while
entry/resume routing costs 8 instructions per Supervisor entry and 2 at the IRQ
entry (32 × 8 + 2 = 258). No memory-helper or Io call occurs in these fixed-RAM
windows. All native K6 register-field accesses occur in the entry-load and common
exit-store ranges, with none in the intervening guest flow.

After profiling, the unchanged full K1–K6 benchmark passes complete outputs/hashes,
34,042 K6 IRQs and 256-cycle bounds. Rounded host I/GI and native text sizes remain
identical to the A5 report; fresh user-counter cycles/GI are K1 2.147, K2 1.659,
K3 1.429, K4 3.220, K5 2.127 and K6 3.956. This is an attribution result, not a
speedup claim. No new ISA behavior or supported-host claim is introduced; macOS
ARM64 remains untested.

Published `src/` 5,281 (+0), `test/` 9,183 (+0). The existing Release/ISA/core/bench
qualification remains applicable to the identical source and binaries; no tests
were changed. R1 is satisfied by the current SSA control flow; memory mapping,
flag liveness, CoreMark and final sanitizer qualification remain open.

## Guarded full-address-space baseline (b1)

Measured on 2026-11-17 with the same Linux x86-64 host, LLVM/Clang 19.1.7
and CPU-2 pinning. The additional `armv5_mapped_bench` consumer lends a sparse
4 GiB reservation through the existing public API: protected low holes,
read-only code and RAM from 0x4000 through 0xffffffff. Guest programs, inputs,
retirement, bounds, interrupts and independent numerical oracles are unchanged.
The complete working buffer is checked after each run, as is the zero-filled
remainder of RAM through sparse-file extents, outside measurement.

Production source and installed headers are unchanged. Both consumers still
execute the existing guarded lowering with precise guest aborts. The ordinary
benchmark retains its original layout. The larger view increases host
instruction counts on every kernel; this is a measurement baseline, not a
speedup or a switch of the ordinary memory path.

| Kernel | Ordinary host I/GI | 4 GiB host I/GI | Change | Ordinary host cycles/GI | 4 GiB host cycles/GI |
|---|---:|---:|---:|---:|---:|
| K1 | 7.923344 | 8.138682 | +2.72% | 2.147 | 2.250 |
| K2 | 6.201774 | 6.287618 | +1.38% | 1.659 | 1.674 |
| K3 | 5.387183 | 5.572458 | +3.44% | 1.429 | 1.511 |
| K4 | 12.839324 | 13.178610 | +2.64% | 3.218 | 3.279 |
| K5 | 7.607471 | 8.007471 | +5.26% | 2.124 | 2.281 |
| K6 | 12.735258 | 12.909793 | +1.37% | 3.915 | 4.015 |

Full metrics for the guarded 4 GiB consumer:

| Kernel | Retired guest instructions | Guest M instructions/s | Native / Clang O3 time | Preparation s | Native text bytes | Text bytes / prepared instruction |
|---|---:|---:|---:|---:|---:|---:|
| K1 | 88,867,584 | 1795.214 | 7.012x | 0.118916 | 8,688 | 310.286 |
| K2 | 61,080,320 | 2425.122 | 3.012x | 0.270793 | 18,611 | 269.725 |
| K3 | 19,822,080 | 2663.338 | 0.996x | 0.266189 | 17,761 | 281.921 |
| K4 | 14,680,064 | 1232.983 | 13.270x | 0.773604 | 25,315 | 444.123 |
| K5 | 10,488,320 | 1752.089 | 44.024x | 0.070855 | 5,541 | 503.727 |
| K6 | 88,935,668 | 1006.068 | 12.435x | 0.125566 | 9,219 | 307.300 |

Complete K1/K2/K6 contract hashes and all K3–K5 outputs pass in both layouts;
K6 retains 34,042 IRQs and the same 256-cycle deadlines. Public lifecycle tests
check the first and last RAM words, precise low-hole abort state/DFSR/FAR and
detection of a real native store outside the original working buffer. Release
ISA/core/bench suites and the source-only offline export pass; the updated core
and both benchmark suites pass again after adding the expanded-storage oracle.
Configured clang-tidy and clang-format pass. Eleven installed-SDK consumers,
including a 4 GiB final-word load and snapshot replay, pass using only the two
installed headers and public library. macOS ARM64 remains untested.

Native stepping of the mapped K4 steady cycle finds 1,476 host instructions
for 112 guest instructions and 24 RAM accesses. The following lower bound counts
only access admission and the descriptor's host-address bias load:

| Identified work | Host instructions / 112 GI | Host instructions/GI |
|---|---:|---:|
| Range-offset formation and limit setup | 48 | 0.428571 |
| Range comparisons and conditional branches | 48 | 0.428571 |
| Store-safety comparisons and conditional branches | 16 | 0.142857 |
| Descriptor host-address-bias loads | 24 | 0.214286 |
| Total identified admission/bias work | 136 | 1.214286 |

This is not total memory cost: guest address arithmetic, ARM word alignment
and rotation, actual loads/stores, PHI moves, host spills and other descriptor
routing are excluded. The ordinary cycle remains 1,438 host instructions.
Direct host-MMU views, including non-identity guest mappings and exact fallback
where host pages cannot express the mapping, remain pending. Their gate compares
against both this mapped baseline and the original ordinary kernels.

Preparation/compiler identity:

```text
armv5-e8278ccb5258ebc56aa3e884d70ce69798dfbd587845d725f793ed7b75c101af-llvm-19.1.7-x86_64-pc-linux-gnu
```

Published `src/` 5,281 (+0), `test/` 9,513 (+330). Flag liveness, CoreMark and
final sanitizer qualification follow the memory implementation.

## Direct host views (b2)

Measured on 2026-11-17 on the same Linux x86-64 host, Clang/LLVM 19.1.7,
Release, owner pinned to CPU 2. Each configuration warms once and reports the
median of three complete, checked runs. No own compilation, analysis or test
job ran alongside these counter windows. Other developer work on the host was
not stopped. Exact and Direct use the same sparse 4 GiB geometry and original
programs, numeric inputs and full-state/RAM oracles. `identity` enables Client
AP3 sections; `remapped` inverts physical address bit 31, including code and
table placement. The table and unused storage are checked outside measurement.

Direct uses typed volatile guest accesses. `UNSOUND` below is a measurement-only
copy with ordinary accesses, held outside the source tree and never shipped.
It fails the independently authored optimized store/reload alias regression;
passing the six kernels does not make that access form valid. Registers and
flags remain ordinary SSA in the shipped implementation.

| Mapping | Kernel | Exact I/GI | Direct I/GI | UNSOUND I/GI | Exact C/GI | Direct C/GI | UNSOUND C/GI |
|---|---|---:|---:|---:|---:|---:|---:|
| off | K1 | 8.330421 | 4.188867 | 4.215419 | 2.351647 | 1.194262 | 1.202971 |
| off | K2 | 6.287619 | 3.313419 | 3.313419 | 1.672732 | 0.940260 | 0.940134 |
| off | K3 | 5.585761 | 3.655587 | 3.655587 | 1.465053 | 0.961681 | 0.967412 |
| off | K4 | 12.107183 | 9.321474 | 9.607189 | 3.006173 | 2.288186 | 2.370051 |
| off | K5 | 8.007522 | 2.606944 | 2.606944 | 2.352578 | 0.703539 | 0.705354 |
| off | K6 | 12.871918 | 9.061407 | 9.152802 | 4.179337 | 2.956723 | 2.983099 |
| identity | K1 | 316.723005 | 4.171154 | 4.221301 | 118.146138 | 1.191400 | 1.202984 |
| identity | K2 | 317.365513 | 3.553767 | 3.553767 | 126.289841 | 0.990496 | 0.989799 |
| identity | K3 | 309.712973 | 3.675163 | 3.675163 | 124.083832 | 0.961975 | 0.971973 |
| identity | K4 | 349.446507 | 9.464332 | 9.321475 | 136.418123 | 2.298956 | 2.292316 |
| identity | K5 | 341.806156 | 2.606944 | 2.606944 | 123.181311 | 0.704049 | 0.703823 |
| identity | K6 | 329.732068 | 9.037206 | 9.150412 | 123.415479 | 2.971972 | 2.969620 |
| remapped | K1 | 332.074001 | 4.171154 | 4.221301 | 120.768762 | 1.191033 | 1.201792 |
| remapped | K2 | 332.841852 | 3.553767 | 3.553767 | 130.406751 | 0.990225 | 0.989588 |
| remapped | K3 | 325.282728 | 3.675163 | 3.675163 | 128.623494 | 0.961607 | 0.960919 |
| remapped | K4 | 366.803654 | 9.464332 | 9.321475 | 141.351940 | 2.300271 | 2.289407 |
| remapped | K5 | 361.805042 | 2.606944 | 2.606944 | 127.039678 | 0.703819 | 0.705285 |
| remapped | K6 | 345.184547 | 9.037206 | 9.150412 | 126.476940 | 2.944385 | 2.975941 |

Full Direct preparation/size and elapsed-time results follow. Text includes both
Direct and Exact units, cold exits and bound continuations. Install time includes
building the derived view and is separate from standalone LLVM preparation.
The slowdown column compares the same algorithm in separately compiled Clang O3
C++; no consumer throughput is inferred.

| Mapping | Kernel | Retired GI | Guest M/s | Native / C++ time | LLVM preparation s | Install ms | Native text bytes | Bytes / prepared instruction |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| off | K1 | 88,867,584 | 3312.454 | 3.747x | 0.202938 | 0.032221 | 12,207 | 435.964 |
| off | K2 | 61,080,320 | 4183.649 | 1.700x | 0.443046 | 0.029016 | 23,978 | 347.507 |
| off | K3 | 19,822,080 | 4145.259 | 0.629x | 0.430565 | 0.031849 | 23,862 | 378.762 |
| off | K4 | 14,680,064 | 1738.454 | 9.138x | 1.443080 | 0.028925 | 31,472 | 552.140 |
| off | K5 | 10,488,320 | 5639.479 | 13.175x | 0.108082 | 0.026713 | 7,351 | 668.273 |
| off | K6 | 88,935,668 | 1343.317 | 9.246x | 0.217204 | 0.027110 | 13,231 | 441.033 |
| identity | K1 | 88,867,584 | 3314.304 | 3.745x | 0.403655 | 0.791963 | 34,447 | 1230.250 |
| identity | K2 | 61,080,320 | 4003.832 | 1.809x | 0.995034 | 0.818820 | 79,657 | 1154.449 |
| identity | K3 | 19,822,080 | 4135.789 | 0.630x | 0.886000 | 0.765948 | 74,111 | 1176.365 |
| identity | K4 | 14,680,064 | 1729.665 | 9.229x | 2.397108 | 0.779795 | 98,971 | 1736.333 |
| identity | K5 | 10,488,320 | 5660.507 | 13.487x | 0.192612 | 0.799755 | 19,421 | 1765.545 |
| identity | K6 | 88,935,668 | 1334.719 | 9.309x | 0.430764 | 0.807956 | 37,903 | 1263.433 |
| remapped | K1 | 88,867,584 | 3314.832 | 3.740x | 0.399581 | 0.810712 | 34,447 | 1230.250 |
| remapped | K2 | 61,080,320 | 3953.041 | 1.778x | 0.983628 | 0.782932 | 79,657 | 1154.449 |
| remapped | K3 | 19,822,080 | 4143.516 | 0.630x | 0.878320 | 0.793132 | 74,111 | 1176.365 |
| remapped | K4 | 14,680,064 | 1732.607 | 9.442x | 2.347236 | 0.786573 | 98,971 | 1736.333 |
| remapped | K5 | 10,488,320 | 5635.173 | 13.578x | 0.192320 | 0.815218 | 19,421 | 1765.545 |
| remapped | K6 | 88,935,668 | 1348.748 | 9.210x | 0.429883 | 0.780258 | 37,903 | 1263.433 |

All six Direct kernels use fewer host instructions and host cycles than the
published b1 guarded 4 GiB baseline. K4 changes from 13.178610 to 9.321474 I/GI
with M off. Current Exact results also differ from b1: the normal benchmark
now lends the whole low code region and uses the current lowering and native
layout. The table above compares Exact and Direct in identical current geometry;
the preceding b1 table remains the published baseline.

### Native memory attribution

Native stepping of the same 16-iteration K4 prefix (224 retired GI) gives the
following counts. Helper instruction counts include nested calls and exclude
native argument setup. These short traces include initial TLB fills and are
attribution evidence, not replacements for the steady counter measurements.

| Mapping / mode | All traced host instructions | Native unit instructions | Fetch helper instructions | Data helper instructions | Fetch / data helper calls |
|---|---:|---:|---:|---:|---:|
| identity / exact | 79,016 | 8,577 | 49,020 | 21,280 | 224 / 48 |
| identity / direct | 2,440 | 2,288 | 0 | 0 | 0 / 0 |
| remapped / exact | 82,915 | 8,577 | 51,495 | 22,704 | 224 / 48 |
| remapped / direct | 2,440 | 2,288 | 0 | 0 | 0 / 0 |

Both Direct traces execute exactly 48 native guest RAM transactions. There are
no range/store-safety comparisons or descriptor-bias loads at those accesses:
the identified b1 admission/bias cost is absent. Guest address arithmetic,
alignment/rotation, actual accesses, view-base retention/reloads and spills
still cost instructions. The Exact MMU helper subtotal is 313.839286 I/GI for
identity and 331.245536 for remapping; Direct eliminates these calls. This
subtotal excludes native call setup and is not total memory cost. The unchanged
warm Direct entry adapter previously measured 49 entry plus 10 exit instructions
per native invocation, excluding guest body and caller, with zero successful-entry
signal-mask system calls.

### Qualification and regime

One benchmark executable replaces the earlier ordinary/mapped duplicates.
Generic fixtures cover sections, coarse/fine large and small pages, tiny and
subpage Exact fallback, domains/AP, FCSE, privilege and forced-user transfers,
TLB lockdown and retained stale translations, snapshot requalification, TCM
routing/protection, shared-file executable aliases, guest PTE writes followed
by TLB invalidation and declared reinstall, and terminal native faults including
PC zero and dead loads. An independent consumer links and binds real executable
ranges and line tables and proves that metadata is copied. ARM/Thumb block,
doubleword, endian, signed, swap and legacy-rotation cases use installed images.
Optimized real-alias store/reload, variable 32-bit wrap and shifted-copy tests
retain independent numerical oracles.

The source-only export passes all eight test registrations (223.88 s), followed
by the final added remap fixture through the core suite (28.24 s). Configured
clang-tidy and clang-format pass. A separately built installed-SDK consumer
passes independent artifact load, LLVM linking, metadata-copy validation and a
real terminal fault with a nonzero variant, using only the two installed public
headers. No manufacturer bytes or manuals are build/test inputs.

Current Direct eligibility uses one privilege-class view. Other privilege,
unexpressible host-page geometry, incompatible retained translations, tiny or
subpage mappings, unreadable prepared code and active lockdown filling use
Exact; forced-user accesses retain Exact helpers. Registered Io or linked
callback roles conservatively select Exact for the image. Resumable native-fault
I/O is not implemented. Fatal Direct faults report diagnostics only and disable
the instance. Cross-unit flag liveness, CoreMark, final own/generated-code
sanitizer qualification and macOS ARM64 remain open.

Preparation/compiler identity:

```text
armv5-166f28a081154b1dd19fb6cac70ce2b6e01069e25a47e9720cc97ac4532236b0-llvm-19.1.7-x86_64-pc-linux-gnu
```

Published `src/` 6,156 (+875), `test/` 11,092 (+1,579).
