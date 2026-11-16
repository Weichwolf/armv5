# ISA and implementation coverage

A1 provides preparation-time ARM operation classification. **No instruction
executes yet.** `Core::lower` identifies an operation and refuses LLVM lowering;
invalid encodings fail earlier with image identity and byte address. Classification
is not semantic coverage and does not yet validate every UNPREDICTABLE operand
combination. No implemented-profile or performance claim follows from this table.

Independent authored fixtures: `test/support/isa.hpp`, `test/isa/preparation.cpp`.
The public preparation interface is the A1 verification boundary specified by
lead-a1-src#2; installed-image execution starts in A2. No private decoder test API
or alternate execution path exists.

| ARM encoding class | A1 classification fixtures | Execution status |
|---|---|---|
| AND/EOR/SUB/RSB/ADD/ADC/SBC/RSC/TST/TEQ/CMP/CMN/ORR/MOV/BIC/MVN | All 15 ordinary conditions, legal S-bit classes, immediate rotate, immediate/register shift, both byte orders | Unimplemented |
| MUL/MLA, UMULL/UMLAL/SMULL/SMLAL | Every operation class; multiply/extra-transfer collision | Unimplemented |
| SMLAxy/SMLAWy/SMULWy/SMLALxy/SMULxy | Every operation class; reserved accumulator fields | Unimplemented |
| QADD/QSUB/QDADD/QDSUB, CLZ | Every operation class; control-extension collision | Unimplemented |
| MRS/MSR | CPSR/SPSR MRS, register/immediate MSR; malformed reserved field | Unimplemented |
| LDR/STR/LDRB/STRB | Immediate-offset operation classes; media-space rejection | Unimplemented |
| LDRH/STRH/LDRSB/LDRSH/LDRD/STRD | Register/immediate classes; reserved offset and post-index encoding rejection | Unimplemented |
| LDM/STM, SWP/SWPB | Operation classes; exclusive-space rejection | Unimplemented |
| B/BL, BX/BLX, BXJ | All branch classes, BLX immediate H bit, unconditional collision | Unimplemented |
| SWI/BKPT | Both operation classes; non-AL BKPT rejection | Unimplemented |
| PLD | Immediate/register forms; register-shift rejection | Unimplemented |
| LDC/STC/CDP/MRC/MCR and unconditional forms | Every operation class; coprocessor extension collision | Unimplemented |
| MCRR/MRRC | Both classes; ARMv6 unconditional forms rejected | Unimplemented |
| Reserved/version extensions | ARMv6 media/exclusive/UMAAL/CPS/SETEND/RFE, architecturally undefined encoding, malformed control fields | Explicit preparation refusal |

| Remaining profile area | Required evidence | Status |
|---|---|---|
| ARM numerical and memory semantics | Flags/shifter boundaries, PC/SPSR, DSP saturation, writeback overlap, access/fault order, alignment/endian | Unimplemented (A2/A4) |
| Thumb and interworking | Every classic Thumb form, BL halves, BX/BLX, PC/SP semantics | Unimplemented (A4) |
| Exceptions and processor modes | All banked registers, CPSR/SPSR, reset, SWI/aborts, IRQ/FIQ, return, waits | Unimplemented (A3) |
| CP15 and memory system | MMU/FCSE/TLB, permissions/domains, fault registers, maintenance, TCM | Unimplemented (A5) |
| Jazelle and optional features | BXJ fallback and exception behavior; explicit Java capability | Unimplemented; Java execution unresolved |
| Native binding and coherency | Artifact load/bind/install, aliases, code/mapping changes, transitions, precise exits, full snapshots | Unimplemented (A2 onward) |
| Common benchmarks | K1–K6, complete checked outputs, hardware counters, Clang O3 ratios, preparation time and native bytes | Unimplemented |
| Host qualification | Strict build/format/static analysis on Linux x86-64; generated code and sanitizer qualification at goal | Linux preparation only; macOS ARM64 untested |

Sources: ARM DDI 0100I A3.1/A3.16 and the individual A4/A5 encodings, selected for
ARMv5TEJ, not the later architecture additions in that manual. Identities and
provenance are recorded in `reference-manifest.json`. Manuals are not build inputs.
