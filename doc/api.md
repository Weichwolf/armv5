# Public API

The installed headers are `armv5/core.hpp` and `armv5/memory.hpp`; the CMake package
exports `armv5::core`. Addresses, memory sizes and PCs are bytes. No LLVM type or
native entry ABI appears in these headers. ARM926 profile completion remains
open; see `isa-coverage.md` for the implemented ARM subset and outstanding modes.

## Preparation and ownership

`Core::lower(CodeImage, CompileOptions)` needs no Core or consumer RAM. It copies
and validates code spans, entries, coherent variants, layouts and transitions,
then returns unoptimized LLVM bitcode and exported names. Decoding happens only
here. The current unit has one ARM execution state, one CPSR processor mode and
one guest endianness; mismatched runtime modes explicitly fail. Unsupported
instructions and invalid forms refuse preparation with identity, byte PC and
reason. Readable tables outside executable spans are never decoded as code.

`SemanticImage::artifact()` owns the complete serialized preparation. It contains
the input declarations, bitcode, exports, compiler identity and checksum.
`Core::load` checks the exact code/options and compiler identity without decoding,
lowering or creating an engine. Compiler identity hashes preparation sources and
includes LLVM revision and host triple. Artifacts contain no instance pointer,
RAM contents or device state. Caches and optimized native objects belong to the
consumer. This format is versioned and deliberately rejects incompatible images.

A consumer links its device IR into the bitcode, optimizes and compiles it, then
passes addresses for the exported names to `Core::bind`. `NativeSymbols::lifetime`
keeps consumer-owned code alive; its optional report carries the consumer's
preparation seconds and text bytes. `Core::compile(CodeImage, CompileOptions)`
is the standalone shortcut using the same lowering and binding, LLVM O3 and an
eager native engine. Every export is materialized before it returns; execution
never decodes or compiles. Standalone preparation time includes the entire path.

`Core::install(native, variant)` validates actual executable bytes and declared
memory permissions/layout. It changes no guest state or time. Failed installation
revokes any old binding. Installed images are immutable and can be shared between
independent owners. `set_memory` copies new storage descriptors while stopped
and revokes the binding; a new matching install is required.

## Execution and snapshots

`run(n)` returns after exactly n complete instructions, or earlier on a refusal,
yield or declared variant transfer. `run_until(deadline)` stops at the first
complete boundary at or beyond the absolute nominal base-cycle deadline. A zero
budget or already reached deadline performs no work. Predicated-false instructions
retire without effects and cost one base cycle. Bounds never affect translation.
`RunResult::progress` is per-call; `State::progress` is cumulative.

Natural blocks use one admission check and keep guest registers in SSA across
known successors. Only the cold remainder path handles an interior run boundary.
The same instruction lowering supplies both paths. Unknown targets fail on the
next admission, after the last completed prefix has been preserved.

`registers`/`set_registers` inspect and initialize visible registers. Setting the
PC requires a declared external entry; native interior continuations can resume
at every prepared instruction boundary. `save`/`restore` preserve current
registers, progress, fault, external-entry continuation and pending code transfer.
Restore clears residue of later execution and leaves memory and the installed
image alone. Banked registers, architectural exceptions and interrupted-boundary
qualification are still A3 work; CP15 state is A5 work.

Code changes through guest stores or host aliases revoke dependent bindings.
Same-value writes do not fabricate a change. Declared transitions identify the
source/destination variants and continuation PC. A changed-code exit with a
matching transition returns `ExecutionState::transfer`; install the destination
variant and call run again. Install does not consume the pending transfer, so a
snapshot can reinstate it. Undeclared changes return a fault. Consumer writes or
mapping changes happen while stopped and are notified by `set_memory` before
execution; the core never hashes guest RAM during execution.

## Storage and callbacks

Memory regions lend initialized consumer storage and permissions. Their spans
must remain valid until rebound. Guest aliases may lend overlapping host spans.
Fixed layouts allow native RAM loads/stores; installation proves whether a data
region can alias prepared code, and unsafe writes retain dynamic invalidation.
Storage shared with other threads/cores requires consumer-owned synchronization
or callbacks; the core owns no multi-core scheduler or peripheral model.

Io callbacks run synchronously and do not throw. False is an explicit fault.
Callbacks may inspect the exact prefix and call `request_yield`; a yield completes
the current instruction before returning. Other mutations and execution reentry
are rejected. An atomic swap uses one swap callback, not separate read/write I/O.
Callback bindings replace an Io role with a symbol in consumer-linked IR; the
bitcode declares the corresponding Io function signature. The consumer test
`test/core/consumer.cpp` demonstrates artifact loading, linked callback IR and
native binding without any private core headers.

Preparation/install failures throw `std::runtime_error` with guest context.
Invalid owner initialization throws `std::invalid_argument`; reentry throws
`std::logic_error`. Execution failures return `Fault` and latch until reset or
restore. IRQ/FIQ entry, architectural aborts, sleep/resume and exception returns
remain explicit A3/A5 refusals, rather than invented no-op behavior.

## Nominal cycles

ARM9E-S DDI 0165B, section 8.1, supplies zero-wait-state base costs. The current
model excludes load-use/data-dependency interlocks, caches, bus waits and board
clock factors. ALU/immediate-shift and ordinary single transfers cost one;
register-controlled ALU shifts cost two; branches cost three; ALU writes to PC
cost three; LDR PC costs five; doubleword transfers and SWP cost two; multiple
transfers cost max(n,2), or n+4 when loading PC. MRS costs two, flags-only MSR and
PLD cost one. A failed predicate costs one. These are documented nominal costs,
not a cycle-accurate ARM926 pipeline or a consumer clock.
