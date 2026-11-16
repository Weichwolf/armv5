# Public API and preparation milestone

The installed public headers are `armv5/core.hpp` and `armv5/memory.hpp`; the
CMake package exports `armv5::core`. All addresses and span sizes are bytes.

A1 establishes the complete contract vocabulary and the preparation decoder.
There is deliberately no executable image in this milestone: `lower` validates
ARM declarations and encoding classes, then refuses with the decoded operation
and the outstanding lowering reason. `compile` reaches the same refusal. `load`,
`bind`, `install`, interrupt operations and stop resumption explicitly refuse
unimplemented functionality. No semantic artifact can yet be created; the image
accessors are the declared A2 interface, not an artifact-completeness claim.

`CodeImage` borrows declarations for the duration of preparation. Variants name
coherent code versions. Spans hold executable bytes; entries and interrupt vectors
are explicit. `CompileOptions` fixes ARM/Thumb state and instruction endianness
for one prepared unit. `VariantLayout` and `CallbackBinding` carry consumer
knowledge without device-specific definitions. Source and LLVM revision are in
`compiler_identity()`; the build hashes preparation sources, including dirty
source edits, without needing Git or a manual.

`Memory` lends consumer storage; spans must outlive the owning Core's binding.
Aliased host spans describe guest aliases. The owner must notify executable code
or mapping changes through `set_memory` with a new generation while stopped, then
install a matching declared variant. Runtime invalidation and dependent bindings
arrive with native execution in A2. `Io` contains plain synchronous, non-throwing
function pointers, including one atomic swap callback. There is no peripheral,
clock, scheduler or firmware state.

Core construction currently establishes ARM supervisor reset values. Visible
register inspection, reset and snapshot copying work for the existing state.
`save` and `restore` retain progress, fault and transfer continuation without
changing consumer storage. Banked state, CP15 and interrupted-boundary snapshot
qualification remain A3/A5 work. An unprepared `run` reports zero progress and a
fault at the guest PC. A zero budget or an already reached deadline performs no
work. These owner-lifecycle checks do not claim instruction execution.

Preparation failures throw `std::runtime_error` with identity, address and reason;
invalid host initialization arguments throw `std::invalid_argument`. Execution
failures are returned in `RunResult`. `Progress` in a run result is per-call;
`State::progress` is cumulative. Cycle costs and precise event boundaries will be
documented with their implementation rather than invented in the interface seed.
