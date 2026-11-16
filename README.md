# armv5

Independent ARMv5 native translation library, initially targeting the ARM926EJ-S
programmer-visible profile. C++23, LLVM 19, GPL-3.0-only.

The library executes prepared ARM images through direct LLVM SSA lowering,
including arithmetic/flags, branches and memory transfers. Preparation is
cacheable and consumers can link their own IR before native binding. Checked
K4/K5 kernels measure hardware counters through the public interface. The complete
ARM926 profile is still under development; unsupported forms refuse explicitly.
There is no interpreter fallback. See [coverage](doc/isa-coverage.md),
[API status](doc/api.md), [objective](OBJECTIVE.md) and
[target architecture](doc/architecture.md).

## Build and verify

Install Clang/LLVM 19 development packages, CMake and Ninja. Production builds and
tests are offline and require no manuals or manufacturer program bytes.

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++-19 \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error
clang-format-19 --dry-run --Werror include/armv5/*.hpp src/*.cpp src/*.hpp \
  test/support/*.hpp test/isa/*.cpp test/core/*.cpp test/core/*.hpp test/bench/*.cpp test/bench/*.hpp
clang-tidy-19 -p build src/*.cpp test/core/*.cpp test/isa/*.cpp test/bench/*.cpp
cmake --install build --prefix /your/prefix
```

Consumers use `find_package(armv5 CONFIG REQUIRED)` and link `armv5::core`.
Linux x86-64 execution of the listed ARM subset is validated; macOS ARM64 remains untested.
Implementation follows milestones A1–A5 in Weichwolf/lead-a1-src#2. A2 provides
installed-image ARM execution, cacheable LLVM artifacts and K4/K5; exceptions/K6
remain A3 work, Thumb/DSP/K1–K3 A4, and the ARM926 memory system A5. Hardware user
counters are accessible on Linux; see [benchmark definitions](test/bench/README.md)
and the [measured baseline](doc/benchmarks.md) for checked, reproducible reports.

## References

[Reference origins and pinned identities](doc/references.md) are recorded in
`doc/reference-manifest.json`. PDFs and extracted text remain ignored locally;
`python3 tools/references.py verify` checks them without network access. They are
not build inputs or test assets. Device firmware, clocks, peripherals, scheduling
and multi-core coupling belong to consumers. The firmware reconstruction remains
paused.

[License](LICENSE) | [Provenance](THIRD_PARTY.txt)
