# Objective

Build a complete, independent, high-performance ARMv5 ARM/Thumb native JIT
library for arbitrary consumer programs. Start with the ARM926EJ-S (ARMv5TEJ)
profile. Support Linux x86-64 and macOS ARM64 with identical guest semantics. A firmware-specific subset
does not meet completion; every legal instruction/form and architectural mode in
the declared profile needs independently justified semantics and coverage.

Expose a small Core/Memory/Io shape through core.hpp and memory.hpp, aligned with
cpu12 and dsp56300. Core owns complete readable state; consumers own memory and
supply real I/O, clocks, peripheral models and scheduling. Preserve architectural
faults, exceptions, reset, interrupts, address aliases, execution state and memory
ordering. Unsupported operations must fail with useful guest context.

Production execution is JIT-only. Compile connected native regions along natural
control flow, keep values live, retain immutable translations and link native
successors. Support executable-memory/mapping changes and cache invalidation with
explicit ownership. No hidden interpreter fallback, instruction-count translation
cap, fabricated progress or application-specific instruction substitution.

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
interrupt boundaries, self-modifying code, aliases and integration callbacks.
Production and independent verification must not share a semantic oracle. Report
exact ISA/mode coverage; unimplemented or unresolved cases prevent completion.
Tests are synthetic: manufacturer firmware and extracted instruction sequences
are not test assets. The firmware reconstruction's byte-only acceptance rule is
separate from this core's correctness requirements.

Measure cold/warm latency, guest instructions, documented base-cycle estimates,
generated code size and matched JIT/host-ASM throughput on both host platforms.
Check complete outputs independently. >=70% realistic warm host-ASM throughput is
a reference, not a device-integration gate. Measure sustainable consumer throughput
and bounded live latency separately; clock factors and unmeasured host results
must remain explicit. Profile-driven optimization must preserve all semantics.

Use C++23, LLVM 19, CMake/Ninja, strict builds, formatting, static analysis and
focused sanitizer verification before publishing implementation increments.
Every line must be understood, meaningfully named and pleasantly readable.
Completion requires a usable native engine, documented API, passing independent
ISA/JIT/core verification and demonstrated supported-host results. Documentation
and downloaded manuals alone do not meet this implementation objective.
