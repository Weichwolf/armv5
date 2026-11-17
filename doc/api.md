# Public API

The installed headers are `armv5/core.hpp` and `armv5/memory.hpp`; the CMake package
exports `armv5::core`. Addresses, memory sizes and PCs are bytes. No LLVM type or
native entry ABI appears in these headers. ARM926 profile completion remains
open; see `isa-coverage.md` for the implemented ARM subset and outstanding modes.

## Preparation and ownership

`Core::lower(CodeImage, CompileOptions)` needs no Core or consumer RAM. It copies
and validates code spans, entries, coherent variants, layouts and transitions,
then returns unoptimized LLVM bitcode and exported names. Decoding happens only
here. Each native unit has one ARM or Thumb execution state, one CPSR processor mode
and one guest endianness. `CodeSpan::instruction_set` selects ARM/Thumb or inherits
`CompileOptions::instruction_set`; spans contain complete aligned 4-byte ARM or
2-byte Thumb instructions. An image may declare both states. Entries and visible
PCs are byte addresses without the function-pointer state tag; CPSR T selects
the execution state. Overlapping declarations must agree byte-for-byte and
select disjoint mode/state contexts. `CodeSpan::processor_modes` declares the modes in which a
span executes; an empty list inherits `CompileOptions::processor_mode`. Shared
code lists all its modes. Installation prebinds every declared unit. Control MSR
and exception return leave the current unit and switch register banks before
entering the prepared destination. Interworking exits change T and enter the
prebound state unit. Execution never decodes or compiles a mode/state. Unsupported
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

## CP15 state and storage spaces

`Registers::system` exposes the implemented CP15 register state. Its `mapping`
contains the control register, translation-table base, domains, FCSE PID and
instruction/data TCM region registers. Fault-status/address, context ID and
lockdown registers are separate fields. They are included in state inspection
and snapshots. The current profile reports r0p5 ID `0x41069265`, 16KB instruction
and 8KB data caches, four ways and 32-byte lines. MMU/FCSE translation and TLB
operations execute as described below. Cache execution effects remain pending;
reporting geometry does not enable caches.

`CodeVariant::mapping` optionally declares the CP15 assumptions for that coherent
variant. Omission inherits the reset mapping and `CompileOptions::big_endian`.
An explicit context selects its own B bit for both instruction bytes and data.
The consumer initializes the actual processor state with `set_registers` before
installation; installation checks it and never initializes it implicitly. A
mapping change through `set_registers` revokes the old native binding. A snapshot
restores processor mapping state; a differently configured installed binding is
revoked. Non-mapping CP15 state does not invalidate the mapping proof.

`Memory::Region::space` and `RegionLayout::space` select `bus`, `itcm` or `dtcm`.
Bus is the default. Each TCM space lends at most one zero-based region of 4KB to
1MB, with power-of-two size. The space and size define processor storage geometry
before installation; instruction and data TCM can overlap bus addresses while
retaining independent consumer-owned bytes. Reset disables the TCMs and retains
their geometry. Disabled TCM storage is excluded from ordinary bus accesses.
Snapshots contain the CP15 region state, not the lent bytes; restoring across a
different TCM geometry is rejected before processor state changes.

Guest MCR writes to control, TTBR, domains, FCSE PID and TCM region registers
canonicalize their defined fields and end the current unit. An unchanged context
continues normally. A changed context revokes the binding after the completed
instruction and takes a declared `CodeTransition` at the next PC; without one it
reports a changed-context fault identifying the MCR. Multiple transitions at the
same source/PC are allowed when their destination CP15 contexts are distinct;
actual processor state selects the matching destination. Equivalent contexts
remain ambiguous, including differences only in disabled TCM size bits. Pending transitions and the
new processor mapping are snapshot state. Installation of the matching variant
leaves that state and time unchanged. Invalid TCM enable/alignment/overlap writes
are refused before retirement; false predicates suppress those checks/effects.

Enabled ITCM serves instruction and data accesses; enabled DTCM serves data only.
Their region bases are physical: with M set, translation and permissions precede
TCM selection.
Disabling either exposes bus storage without discarding the consumer's TCM bytes.
A region-register base change relocates the same storage. Fixed TCM layouts are
still zero-based; their native address bias uses the variant's known guest base.
Bus proofs exclude active overlays even if no fixed TCM layout was supplied.
Executable validation splits spans at ITCM boundaries. Writes through any host
alias of actual prepared code revoke the binding, including an ITCM alias.
Ignoring a write to read-only size bits does not revoke an unchanged binding.

Prepared variants support the M, A, B, S, R, V and L4 control bits. With A set,
unaligned word/halfword accesses take Data Abort before any memory access or writeback;
byte accesses remain allowed. The first unaligned transfer address is recorded in
FAR and DFSR becomes 1; IFSR is preserved. An alignment abort retires no instruction
and charges the ordinary three-cycle exception entry. Doubleword transfers retain
the ARMv5 requirement for eight-byte alignment; a word-aligned but non-doubleword-
aligned operand remains an explicit unpredictable-access refusal. With A clear,
legacy rotation/alignment behavior is unchanged.

L4 inhibits instruction-set exchange for LDR/LDM to PC and Thumb POP PC, including
successful callbacks that request yield. BX/BLX and SPSR exception returns retain
their independent state-change semantics. V selects exception addresses in
`0xffff0000..0xffff001c`; each actual high vector and its mode must be declared.
Reset uses fixed low-vector/little-endian input pins (V=B=0).

With the MMU disabled, missing physical storage or denied region permissions
record external-abort status 8 in DFSR and the original access MVA in FAR. For
multiple/doubleword transfers, ARM926 records the final MVA of the aborted burst:
the last transfer before a 1KB boundary if an earlier word aborts, or the last
transfer address if the first abort is after that boundary. This can differ from
the failing word. IFSR is preserved. A present I/O callback returning false is a
host memory fault and leaves DFSR/FAR/IFSR unchanged, including after a partial
multiple transfer. Missing vector declarations still report the actual vector;
fault registers already reflect the architectural data abort.

With M set, declared instruction addresses and data addresses are virtual. The
FCSE PID modifies addresses below 32MB; higher addresses remain unchanged.
Translation supports 1MB sections, coarse/fine tables, 64KB/4KB pages with their
subpage permissions, and fine-table 1KB tiny pages. Table walks read physical bus
storage in the selected byte order; a missing RAM descriptor uses the read I/O
role, including a consumer-linked replacement. Domains and AP/S/R enforce the
actual privilege, including User permissions for LDRT/STRT. Translation, domain,
permission, page-walk external and physical external aborts preserve their own
status classes. Data faults update DFSR/FAR; fetch faults update only IFSR and
enter Prefetch Abort before instruction retirement. FAR uses the FCSE-modified
address and retains original byte offsets for A=0 word faults. Alignment faults
precede translation. A present callback returning false
is a host fault, not an architectural abort; successful yield completes the
current instruction, including PC/link/interworking effects.

The ARM926 unified TLB retains 64 ordinary and eight lockdown translations.
Ordinary replacement is deterministic; physical TLB placement is outside the
programmer's model. P/Victim in c10 selects locked fills and advances modulo
eight. c8 c5/c6/c7 operations alias the same unified TLB. Invalidate-all preserves
locked entries; invalidate-by-MVA removes matching ordinary and locked entries.
The operand is already an MVA, without another FCSE modification. Page-table
writes and reinstall do not invalidate translations. Snapshot save/restore
includes the hidden TLB and replacement continuation; reset clears them.

Installation validates code using existing translations or side-effect-free RAM
table walks. It never fills a TLB, advances a lockdown victim or calls I/O. A
mapping that cannot be validated from lent storage is refused. Permissions are
checked on actual execution, so a declared instruction can take Prefetch Abort.
Every fetch validates its translation against the installed virtual-to-physical
binding without decoding or hashing code. A changed physical backing revokes the
binding even if its bytes match; an undeclared translated alias still refuses.
TLB maintenance under M=1 revokes through a declared transition, which may target
the same variant. Variants still require their exact declared CP15 context.

Cache execution/maintenance, debug IFSR qualification and remaining CP15
operations are pending; cache-enabled configurations cannot yet be installed.
See `isa-coverage.md`.

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
registers, all banked registers/SPSRs, progress, fault, external-entry continuation
and pending code transfer. `State::banks` exposes the architectural banks, with
the active bank normalized into the returned copy. Setting visible registers
initializes their selected bank; it preserves inactive banks. Restore clears
residue of later execution and leaves memory and the installed image alone.
Interrupted IRQ/FIQ replay is checked, including between the two Thumb BL/BLX
halves. The prefix retires separately, writes the architectural LR and can be
snapshotted or interrupted; return resumes its suffix. Preparation rejects
unpaired halves and external/static branch entries into a suffix. Ordinary
indirect/interworking branches into a suffix fail explicitly, including at a run
bound. Remaining CP15 effects remain A5 work.

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
the current instruction before returning, including a loaded PC, interworking
or SPSR/banked-register restoration. Other mutations and execution reentry
are rejected. An atomic swap uses one swap callback, not separate read/write I/O.
Callback bindings replace an Io role with a symbol in consumer-linked IR; the
bitcode declares the corresponding Io function signature. The consumer test
`test/core/consumer.cpp` demonstrates artifact loading, linked callback IR and
native binding without any private core headers.

Preparation/install failures throw `std::runtime_error` with guest context.
Invalid owner initialization throws `std::invalid_argument`; reentry throws
`std::logic_error`. Execution failures return `Fault` and latch until reset or
restore. Missing physical storage and denied region permissions cause a Data
Abort; a present Io callback returning false remains an explicit host memory
fault. A Data Abort preserves the original instruction base register (the ARM9E
base-restored model), leaves the failing instruction unretired and enters Abort
mode. Earlier completed multiple-transfer accesses remain visible; non-base
loaded registers are architecturally unspecified on that abort.

`interrupt_ready` tests CPSR I/F masks at a stopped instruction boundary; a masked
`interrupt` request has no effect. An accepted IRQ/FIQ saves CPSR, banks SP/LR
(and FIQ R8..R12), sets masks and enters the declared vector without executing a
handler instruction. FIQ can preempt IRQ. Consumers own request levels, priority
and delivery timing. Every exception vector must appear in `interrupt_vectors`
and in a code span prepared for its exception mode. Missing declarations fail
explicitly. SWI, explicit ARM UDF and BKPT enter Supervisor, Undefined and Prefetch
Abort vectors respectively. ALU S+PC and LDM^ with PC restore SPSR and register
banks. LDM/STM^ without PC access User banks. User/System SPSR and ^ forms refuse
preparation. Reset deterministically clears unspecified register/bank values,
sets CPSR 0xd3 and starts at the supplied aligned vector. Remaining CP15 operations
and sleep/resume remain A5 work. Thumb exception returns restore T and resume the
declared Thumb unit.

## Nominal cycles

ARM9E-S DDI 0165B, section 8.1, supplies zero-wait-state base costs. The current
model excludes load-use/data-dependency interlocks, caches, bus waits and board
clock factors. ALU/immediate-shift and ordinary single transfers cost one;
register-controlled ALU shifts cost two; branches cost three; ALU writes to PC
cost three; LDR PC costs five; doubleword transfers and SWP cost two; multiple
transfers cost max(n,2), or n+4 when loading PC. MRS costs two, flags-only MSR and
PLD cost one. MSR selecting non-flag fields costs three; MUL/MLA cost two,
or four when setting flags. Long multiply/accumulate costs three, or five with
flags. Halfword/word DSP multiply costs one, except SMLALxy costs two; CLZ and
Q arithmetic cost one. Thumb operations use the same nominal operation costs;
a Thumb BL/BLX pair costs one for its prefix and three for its suffix. SWI/UDF/BKPT charge three and retire one exception-
generating instruction. IRQ/FIQ and Data Abort entry charge three without
retiring a guest instruction; returns charge their ordinary PC-write cost.
A failed predicate costs one. These are documented nominal costs,
not a cycle-accurate ARM926 pipeline or a consumer clock.
