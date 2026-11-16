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
