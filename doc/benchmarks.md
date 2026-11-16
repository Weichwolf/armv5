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
