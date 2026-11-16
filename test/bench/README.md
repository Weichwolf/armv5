# Common checked kernels

K4–K6 are the implemented contract kernels. K1–K3 remain open.
`kernels.cpp` authors ARM programs using independent encoding fields. The
separately compiled portable C++ in `reference.cpp` defines the numerical
algorithm; no production semantic code is shared. All arithmetic wraps modulo
2^32. Inputs and output checks are fixed and reproducible without manuals or
manufacturer program bytes.

K5 copies 4096 words and accumulates every copied word into a running checksum,
repeating 512 times (10,488,320 guest instructions). Word i is
`(i * 0x9e3779b9) xor rotl(i + 0x01234567, i mod 32)`. Each pass copies the same
source into the destination and continues the checksum. The complete RAM buffer,
checksum, pointer/count results and exact retirement/base cycles are checked.

K4 runs 1,048,576 table-driven state transitions (14,680,064 guest instructions),
starting at state 3 and accumulator 0x2468ace0. State selects one of eight guest
handler addresses. Each handler saves LR on the stack, adds state to the
accumulator, XORs state+1, selects (state+3) mod 8, calls a common subroutine that
XORs ror(state,1) and adds 3, then restores PC from the stack. Every handler is
visited. Complete state/accumulator, table and stack memory and exact retirement
and nominal base cycles are checked. K4 exercises BL/BLX/BX and LDR PC returns.

K6 runs a counted 64-tap FIR over 1024 output samples, repeating 32 times.
Inputs are sign-extended 16-bit integer values represented in uint32_t; products
and accumulated outputs wrap modulo 2^32, without rounding or saturation.
Sample i is `((i * 0x9e3779b9) & 0xffff) - 0x8000`; coefficient j is
`((j * 73 + 19) & 0xffff) - 0x8000`. Output s is the sum of
`input[s + j] * coefficient[j]` for all 64 taps. Every pass starts from the same
input and overwrites all 1024 outputs. The separately compiled C++ uses nested
sample/tap loops; no production semantics or encoding helpers enter its oracle.

The owner calls `run_until` at absolute 256-cycle boundaries, delivering IRQ when
a 4096-cycle event deadline is reached; an instruction may cross a boundary.
The handler increments its banked R13 counter and returns with SUBS PC,LR,#4,
preserving all FIR registers and flags. Entry plus handler cost seven base cycles
and two retired instructions per event. Algorithm work is 10,748,064 instructions
and 17,039,518 cycles; 4,167 events produce 10,756,398 total instructions and
17,068,687 cycles. Complete visible registers, RAM (including inputs/coefficients,
all outputs and surrounding bytes), IRQ counter, retirement and base cycles are
checked against independent loop totals/output. The C++ comparison runs the same
FIR algorithm; event-scheduler/bound overhead remains charged to the native run.

Run `build/armv5_bench [K4|K5|K6]`. It warms each kernel, checks every complete output,
then reports the median of three measured runs. Linux perf_event_open groups
instructions and cycles for the current owner thread, excludes kernel/hypervisor
execution and accounts for counter multiplexing. The enabled interval contains
only the warm public run (plus owner bound/IRQ delivery for K6), or corresponding
C++ algorithm, never LLVM preparation
or output validation. The CSV reports instructions/cycles per retired guest
instruction, guest instructions/s, native/C++ elapsed-time ratio, full standalone
preparation seconds, native text bytes and text bytes per *prepared* instruction.
The last metric uses unique declared guest words; it is a code-density measure.

Counter refusal is an explicit error, not a zero measurement. macOS ARM64 hardware
counter collection remains unqualified. The cycle model is documented in
`doc/api.md`; these measurements do not imply consumer throughput or a board clock.
