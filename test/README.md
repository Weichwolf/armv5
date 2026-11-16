# Verification

Three layers follow `doc/core-contract.md`:

- `isa`: independent synthetic encoding/semantic programs.
- `core`: the public preparation and owner lifecycle.
- `bench`: K1–K6 with complete independent output checks, starting in A2.

`support/isa.hpp` encodes fixtures independently of production decoding. A1 tests
preparation diagnostics through `Core::lower`, as required by the initial
non-executing milestone in lead-a1-src#2. No private decoder interface is tested.
From A2 onward, execution tests use declared and installed native images only.
No manufacturer firmware, extracted sequences or manuals are test dependencies.
