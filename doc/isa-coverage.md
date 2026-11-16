# ISA and implementation coverage

A4 executes the ARM/Thumb operations and modes below through prepared LLVM
images. This is **not** complete ARMv5TEJ qualification. CP15 and the ARM926
memory system remain open.
Tests are independently authored programs, run through the public interface with
installed images. Rejected preparation cannot produce an installed image and is
checked through `Core::lower`; no private decoder or alternate executor is used.

| Operation/forms | Current implementation and independent evidence |
|---|---|
| ARM AND/EOR/SUB/RSB/ADD/ADC/SBC/RSC/TST/TEQ/CMP/CMN/ORR/MOV/BIC/MVN | Direct SSA; `test/isa/preparation.cpp` checks all 16 operations across signed/unsigned numeric boundaries, carry/borrow/overflow, Q/V preservation, complete visible registers |
| Operand 2 and shifts | Immediate/rotated-immediate, immediate shift and register-controlled LSL/LSR/ASR/ROR, RRX; counts 0/1/31/32/33/255/256, incoming carry, constant rotation; further encoding combinations remain to qualify |
| Predication | All 15 ordinary conditions against all 16 NZCV combinations; false memory predicates have no access/writeback effects |
| B/BL, BX/BLX register, PC | Known successors linked directly; forward ARM BL leaf bodies use caller-specific SSA return edges with checked LR/loaded-PC and cold modified-target exits; ARM destinations and returns exercised by K4; Thumb BX/BLX register, ARM BLX immediate (including interworking state changes) and interrupted Thumb BL/BLX pairs exercised by installed fixtures; interior continuations and unknown fallthrough checked |
| LDR/STR/LDRB/STRB | Immediate/scaled register offsets, indexing and writeback implemented; public tests cover pre/post up/down access, byte/word, legacy unaligned word rotation, both endiannesses, fixed and dynamic memory paths, region-end/underflow/narrow-region/wrap bounds and complete fault prefixes; K4/K5 check complete memory output |
| LDRH/STRH/LDRSB/LDRSH/LDRD/STRD | Immediate/register addressing implemented, signed extension and pairs; fixtures cover signed byte/halfword and doubleword loads/stores in both byte orders. Unpredictable alignment and operand combinations explicitly refuse; exhaustive addressing combinations remain open |
| LDM/STM | Ordinary-bank IA/IB/DA/DB implemented, ordered transfers and writeback; independent stack push/pop fixtures and K4 returns; user-bank forms independently checked in all five exception modes, plus LDM^ exception return and old-bank writeback |
| SWP/SWPB | One atomic I/O callback or one owner RAM transaction; byte/word and legacy alignment implemented, both byte orders checked |
| MRS/MSR | CPSR/SPSR reads and byte-selected writes, privilege masks and dynamic mode validation; all seven modes switch prepared units. All five SPSRs roundtrip and restore NZCVQ/mode through MOVS PC and LDM^; invalid User/System SPSR forms refuse preparation |
| MUL/MLA/MULS/MLAS | Direct SSA multiplication and accumulate modulo 2^32; independent edge products, NZ updates and CVQ preservation; K6 FIR. Long/DSP forms covered below |
| UMULL/UMLAL/SMULL/SMLAL | All signed/unsigned, accumulate and S variants; independent 64-bit edge products, wraparound, complete register results, NZ with CVQ preservation and input/output aliases in `test/isa/dsp.cpp` |
| SMULxy/SMLAxy/SMLALxy/SMULWy/SMLAWy | All bottom/top selections; signed halfword products, long accumulation, independently floor-rounded word-by-halfword products and sticky Q on overflow; K3 uses halfword multiplies |
| CLZ and QADD/QSUB/QDADD/QDSUB | Zero and signed extrema, independent clamp arithmetic, sticky Q from either double or final saturation; untouched NZCV |
| Classic Thumb ALU | Shifts, add/subtract, immediate MOV/CMP/ADD/SUB, all low-register logical/arithmetic/multiply forms and high-register operations normalize into the same reviewed operation semantics. Independent arithmetic oracle and shift boundary tables check flags and complete registers |
| Classic Thumb memory | Register/immediate byte, halfword, word and signed forms, SP-relative and PC-literal addressing, ADR and SP adjustment; both endiannesses and fixed/dynamic memory paths checked; PUSH/POP/LDM/STM ordering, base-in-list writeback suppression and POP PC interworking checked |
| Classic Thumb control | All conditional branches against independent NZCV truth masks, unconditional branches, BX/BLX and separate BL/BLX halves; IRQ and snapshot return between halves, ARM/Thumb exception entry/return, invalid pair/entry forms and out-of-profile extensions checked |
| SWI/UDF/BKPT | Precise vector/SPSR/LR/mask entry and return; explicit ARM UDF encoding and BKPT prefetch-abort fallback checked. Other unsupported encodings still refuse preparation |
| PLD | Advisory hint has no required observable data access; accepted without inventing faults or device reads |
| Invalid/unsupported encodings | Public preparation diagnostics cover extension collisions, ARMv6 media/exclusive/UMAAL/unconditional control, malformed control fields and invalid long-multiply pairs/PC and explicit unlowered CP15 |

| Lifecycle area | Evidence/status |
|---|---|
| Preparation/cache/binding | Lowered LLVM bitcode, complete checked artifact and load without lowering; missing native symbols and corrupt/mismatched artifacts rejected; consumer-linked IR callback runs through `bind` |
| Installed code/layout | Coherent overlapping ARM/Thumb bytes with separate mode/state semantics; actual byte/permission/layout validation; failed install and memory rebind revoke bindings without changing CPU/time |
| Bounds and state | Exact instruction budgets inside natural blocks, deadline overshoot only at complete boundaries, owner reentry rejection, callback prefix inspection and yield, fault latch, save/restore; `test/core/returns.cpp` checks BX/MOV/LDR/LDM returns, modified link/stack values, interworking, undeclared destinations and snapshots at every call-prefix bound |
| Variants and aliases | Native writes through host storage aliases revoke before changed successors; declared destination installation and saved pending-transfer restoration checked; a partial code-changing STM followed by Data Abort enters the abort state before revocation, then resumes through the declared variant |
| Modes, exceptions and banks | All seven CPSR modes, User/System sharing, five exception SP/LR/SPSR banks and FIQ R8..R12; nested IRQ/FIQ, mask eligibility, interrupted snapshots/replay, reset masks, undeclared vectors and base-restored Data Abort checked |
| Performance | K1–K6 check complete outputs and retire over ten million instructions; per-thread user instructions/cycles, throughput, Clang O3 ratio, preparation and text size are measured in `test/bench`; K1/K2/K6 match the contract v6 Q1.15 SHA-256 values; K6 also checks 34,042 IRQ deliveries under 256-cycle bounds. K4 hot-code attribution is in `benchmarks.md` |

| Mandatory profile work still open | Milestone |
|---|---|
| MMU-triggered instruction/data abort status, high vectors and CP15 reset state | A5 |
| Coprocessor absence and CP15 register semantics, MMU/FCSE/TLB, domains/permissions/faults, maintenance and TCM | A5 |
| BXJ documented fallback/exception behavior and explicit Java capability; Java execution unresolved | Open |
| Exhaustive encoding/operand/addressing qualification, architectural memory-abort ordering and code/mapping edge cases | Open; prevents profile completion |
| CoreMark 1.0 external reference point | A5 |
| Sanitizer qualification of own and generated code | At goal |
| macOS ARM64 semantics and counters | Untested; developer host unavailable |

Instruction encodings/semantics: ARM DDI 0100I A2–A7, restricted to ARMv5TEJ.
Profile: DDI 0198E. Nominal cycle accounting: DDI 0165B 8.1, without interlocks or
bus waits. References and their exact local identities are in
`reference-manifest.json`; manuals never enter production builds or tests.
