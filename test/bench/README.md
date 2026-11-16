# Common checked kernels

K4 and K5 are the implemented contract kernels. K1–K3 and K6 remain open.
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

Run `build/armv5_bench [K4|K5]`. It warms each kernel, checks every complete output,
then reports the median of three measured runs. Linux perf_event_open groups
instructions and cycles for the current owner thread, excludes kernel/hypervisor
execution and accounts for counter multiplexing. The enabled interval contains
only the warm public run (or corresponding C++ algorithm), never LLVM preparation
or output validation. The CSV reports instructions/cycles per retired guest
instruction, guest instructions/s, native/C++ elapsed-time ratio, full standalone
preparation seconds, native text bytes and text bytes per *prepared* instruction.
The last metric uses unique declared guest words; it is a code-density measure.

Counter refusal is an explicit error, not a zero measurement. macOS ARM64 hardware
counter collection remains unqualified. The cycle model is documented in
`doc/api.md`; these measurements do not imply consumer throughput or a board clock.
