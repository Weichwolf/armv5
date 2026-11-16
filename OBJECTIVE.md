# Objective

Build a complete, independent, high-performance ARMv5 ARM/Thumb native translation
library for arbitrary consumer programs. Start with the ARM926EJ-S (ARMv5TEJ)
profile. Support Linux x86-64 and macOS ARM64 with identical guest semantics. A firmware-specific subset
does not meet completion; every legal instruction/form and architectural mode in
the declared profile needs independently justified semantics and coverage.

Expose a small Core/Memory/Io shape through core.hpp and memory.hpp, aligned with
cpu12 and dsp56300. Core owns complete readable state; consumers own memory and
supply real I/O, clocks, peripheral models and scheduling. Preserve architectural
faults, exceptions, reset, interrupts, address aliases, execution state and memory
ordering. Unsupported operations must fail with useful guest context.

Follow doc/core-contract.md, the contract shared by all cores. Production
executes prepared LLVM images only: the consumer declares executable code,
entries, variants and memory layouts; the core lowers them to LLVM IR; the
consumer optimizes, caches and installs. Keep values live across natural control
flow and link known successors directly. A code or mapping change revokes the
binding and a declared variant is installed. No interpreter, no per-host emitter,
no decoding or compilation at run time, no instruction-count translation cap,
fabricated progress or application-specific instruction substitution. Complete
state save and restore and cacheable preparation are part of the interface.

Preserve ARM and classic Thumb execution, interworking, predication, the barrel
shifter, arithmetic/shift flags, multiplication and E-extension DSP instructions,
loads/stores, atomic swap, endian/alignment rules, PC semantics and coprocessor
instruction behavior. Implement all banked registers, CPSR/SPSR, processor modes,
exception entry/return, reset and IRQ/FIQ semantics. Include the documented CP15,
MMU, FCSE, TLB, cache-maintenance and TCM behavior needed by the ARM926 profile.
Architecturally visible memory-system behavior is required; cycle-accurate caches
and electrical bus simulation are separate requirements, not implied here.

Document Jazelle explicitly: implement documented BXJ entry/fallback and exception
behavior, and state whether Java execution is available. Do not advertise a full
Jazelle DBX implementation from ARM/Thumb support alone. Unavailable documentation
or unimplemented mandatory profile behavior must remain an explicit open item;
do not silently treat Java bytecodes as ARM instructions or invent no-op semantics.
Exclude ARMv6/v7, Thumb-2, AArch64, NEON and an invented VFP unit from this profile.

Provide reproducible independently authored ISA fixtures and independent expected
results. Verify state, flags, numerical edge cases, memory access/order, faults,
interrupt boundaries, declared code changes and transitions, aliases and
integration callbacks.
Production and independent verification must not share a semantic oracle. Report
exact ISA/mode coverage; unimplemented or unresolved cases prevent completion.
Tests are synthetic: manufacturer firmware and extracted instruction sequences
are not test assets. The firmware reconstruction's byte-only acceptance rule is
separate from this core's correctness requirements.

Measure with the common benchmarks of the contract (kernels K1 to K6): host
instructions and host cycles per guest instruction from hardware counters, guest
instructions per second, ratio to the same algorithm in portable Clang O3 C++,
preparation seconds and native code bytes, every kernel checking its complete
output. Sustainable consumer throughput and live latency are measured in the
consumer; clock factors and unmeasured host results stay explicit.

Use C++23, LLVM 19, CMake/Ninja, strict builds, formatting, static analysis and
focused sanitizer verification before publishing implementation increments.
Every line must be understood, meaningfully named and pleasantly readable.
Completion requires a usable native engine, documented API, passing independent
ISA and core verification and demonstrated supported-host results. Documentation
and downloaded manuals alone do not meet this implementation objective.
