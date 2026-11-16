# A2 benchmark baseline

Measured on 2026-11-16 on Linux x86-64, Intel Core i5-10400, six physical cores, SMT disabled. The owner thread was pinned to logical CPU 2. Clang and LLVM 19.1.7; C++23 Release (`-O3 -DNDEBUG`, default x86-64 compiler target). The standalone engine uses LLVM O3 and the detected native target. No clock factor or consumer throughput is inferred.

Each kernel warms once, checks every complete output, then reports the median of three measured runs. User-only hardware instructions and cycles come from a perf_event_open group on the owner thread; interval-specific enabled/running times account for multiplexing. Preparation and output checks are outside the enabled window. The C++ reference is separately compiled and uses the same modulo-2^32 algorithm. Definitions and exact output checks are in [test/bench/README.md](../test/bench/README.md).

| Kernel | Retired guest instructions | Host instructions / guest | Host cycles / guest | Guest M instructions/s | Native / Clang O3 time | Preparation s | Native text bytes | Text bytes / prepared instruction |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| K4 | 14,680,064 | 19.411 | 5.078 | 800.461 | 20.440x | 0.654482 | 22,523 | 395.140 |
| K5 | 10,488,320 | 22.398 | 5.674 | 708.712 | 107.575x | 0.048816 | 3,485 | 316.818 |

Text density divides all generated text, including cold exits and bound continuations, by unique declared guest instructions (K4: 57; K5: 11). The measured native/C++ ratio is elapsed-time slowdown, not a speedup. This baseline exposes remaining guard/status and code-density costs; it is not a final performance qualification. K1–K3/K6 and macOS ARM64 measurements remain open.

Reproduce after the Release build:

```sh
taskset -c 2 build/armv5_bench
```

Preparation/compiler identity:

```text
armv5-fa2444d99af8fd1b889f64eeb0f2c1306f590b52a56874bd7aca2f83dece02af-llvm-19.1.7-x86_64-pc-linux-gnu
```
