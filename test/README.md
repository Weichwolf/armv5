# Verification

Three layers follow `doc/core-contract.md`:

- `isa`: independent authored ARM arithmetic, flags, shifts, predication and memory programs.
- `core`: public artifact/binding/install/run lifecycle, bounds, callbacks, faults, snapshots and variants.
- `bench`: K4/K5 with complete output checks and per-thread hardware counters; K1–K3/K6 remain open.

`support/isa.hpp` encodes synthetic fixtures independently of production decoding.
Every execution test declares and installs native images through the public API.
Preparation rejection tests cannot install an image and check `Core::lower`
diagnostics. No private core headers, interpreter or capture/step API is used.
The consumer binding test links its own IR using LLVM and the public bitcode and
export names. No manufacturer firmware, extracted sequences or manuals are test
dependencies. See `test/bench/README.md` for exact numerical definitions.
