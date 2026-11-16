# armv5

Independent ARMv5 native translation library, initially targeting the ARM926EJ-S
programmer-visible profile. C++23, LLVM 19, GPL-3.0-only.

A1 supplies the two public headers, build/package targets, ARM preparation decoder
and independent encoding fixtures. **Native execution is not implemented yet.**
Preparation refuses every operation with image identity, address and reason;
there is no interpreter fallback. See [coverage](doc/isa-coverage.md),
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
  test/support/*.hpp test/isa/*.cpp test/core/*.cpp
clang-tidy-19 -p build src/core.cpp src/decode.cpp \
  test/core/preparation.cpp test/isa/preparation.cpp
cmake --install build --prefix /your/prefix
```

Consumers use `find_package(armv5 CONFIG REQUIRED)` and link `armv5::core`.
Linux x86-64 preparation is validated; macOS ARM64 remains untested.
Implementation follows milestones A1–A5 in Weichwolf/lead-a1-src#2. Installed-image
ISA execution, cacheable LLVM artifacts and K4/K5 begin in A2; exceptions/K6 in A3;
Thumb/DSP/K1–K3 in A4; the ARM926 memory system in A5. Hardware user counters are
accessible on the development host; kernel benchmark results do not exist yet.

## References

[Reference origins and pinned identities](doc/references.md) are recorded in
`doc/reference-manifest.json`. PDFs and extracted text remain ignored locally;
`python3 tools/references.py verify` checks them without network access. They are
not build inputs or test assets. Device firmware, clocks, peripherals, scheduling
and multi-core coupling belong to consumers. The firmware reconstruction remains
paused.

[License](LICENSE) | [Provenance](THIRD_PARTY.txt)
