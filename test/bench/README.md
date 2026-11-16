# Common checked kernels

All six contract kernels K1–K6 are implemented and check complete output.
`kernels.cpp` authors ARM programs using independent encoding fields. The
separately compiled portable C++ in `reference.cpp` defines the numerical
algorithm; no production semantic code is shared. K4/K5 arithmetic wraps modulo 2^32. K1/K2/K6 implement the common contract
v6 Q1.15 definition. Inputs and complete output checks are fixed and reproducible
without manuals, network access or sibling repositories. The common generator
is not compiled or copied here: the guest and separately compiled portable C++
algorithms must match the contract's complete-result SHA-256 values. Hash words
are signed 32-bit little endian, including sign extension.

K1 and K6 process 256 consecutive blocks of 1024 samples through a causal 64-tap
FIR. History starts with 63 zeros and continues across every block boundary.
Input advances the unsigned 32-bit LCG `r = r*1664525+1013904223`, starting at
`0x243f6a88`; sample i is `(int32(r>>16)-32768)/2` with truncating division.
Indices divisible by 97 instead use 32767; the following indices use -32768.
Coefficient j is `(((j*17)%31)-15)*32`, in causal order. Each output is the sum
of the 64 products, rounded to nearest with ties to even and clipped to
[-32768,32767]. The guest uses a signed 32-bit MLA accumulator: the coefficient
absolute sum bounds the accumulator and rounding bias within this range.
The complete 262,144-sample output matches the Q1.15 K1 hash in
[core-contract.md](../../doc/core-contract.md). K1 retires 88,867,584 instructions
and 139,199,230 base cycles, including all block loops.

K2 streams the same 256 blocks through four transposed direct-form-II biquads.
For each section, with all quantities in Q1.15:
`y = Q(x*b0 + s1*32768)`,
`next_s1 = Q(x*b1 - y*a1 + s2*32768)`,
`next_s2 = Q(x*b2 - y*a2)`.
Q rounds to nearest with ties to even and saturates to signed 16 bits. Output and
both states are quantized at every section; all eight initial states and all
four coefficient sets are fixed in `kernels.cpp`. Coefficients bound the signed
32-bit intermediate sums and rounding bias; no intermediate saturation replaces
cancellation. The independent C++ uses signed 64-bit arithmetic and rounds the
magnitude before restoring its sign, unlike the guest's shift/parity/bias form.
All 262,144 outputs and eight final states match their two contract SHA-256
values. Complete registers and RAM are also checked. Work is 61,080,320 guest
instructions and 68,420,350 base cycles.

K3 runs a 256-point radix-2 complex FFT 512 times. Input real/imaginary components
are `((i*97+13)&4095)-2048` and `((i*53+7)&4095)-2048`, packed as signed 16-bit
real low / imaginary high. The guest copies natural-order input through a
bit-reversal offset table, then computes decimation-in-time butterflies.
`fft.hpp` fixes the authored Q15 twiddles for exp(-2*pi*i*k/256), rounded and
clipped to signed 16 bits. Complex products round by floor((product+16384)/32768);
every butterfly output divides by two with floor rounding. The small input and
stage scaling preserve signed-16-bit component and signed-32-bit product headroom.
The portable reference computes its own bit reversal and scalar butterflies.
All 256 packed outputs, temporary/final registers, input/twiddle/permutation
storage, surrounding RAM and progress are checked. Work is 19,822,080 instructions
and 21,131,774 cycles. Native programs are authored in `kernels.cpp`; none is
substituted by a host algorithm in the production core.

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

K6 uses the identical K1 program and output under event load. The owner calls
`run_until` at absolute 256-cycle boundaries and delivers IRQ when a 4096-cycle
event deadline is reached; one instruction may cross a boundary. The handler
increments its banked R13 counter and returns with SUBS PC,LR,#4, preserving all
FIR registers and flags. Entry plus handler cost seven base cycles and two
retired instructions per event. Exactly 34,042 events produce 88,935,668 total
instructions and 139,437,524 cycles. Every output, visible register, surrounding
RAM byte, event count and progress total is checked. The C++ comparison runs the
same FIR algorithm; scheduler/bound/event overhead remains charged to native
execution.

Run `build/armv5_bench [K1|K2|K3|K4|K5|K6]`. It warms each kernel, checks every complete output,
then reports the median of three measured runs. Linux perf_event_open groups
instructions and cycles for the current owner thread, excludes kernel/hypervisor
execution and accounts for counter multiplexing. The enabled interval contains
only the warm public run (plus owner bound/IRQ delivery for K6), or corresponding
C++ algorithm, never LLVM preparation
or output validation. The CSV reports instructions/cycles per retired guest
instruction, guest instructions/s, native/C++ elapsed-time ratio, full standalone
preparation seconds, native text bytes and text bytes per *prepared* instruction.
Text density uses unique declared guest words. Additional CSV columns report guest
instructions and base cycles per sample, elapsed ns per guest instruction, and
the fraction of retired guest instructions executed by prepared native code
(always 1; no interpreter path exists). The work unit for K3 is one FFT sample,
for K4 one state transition and for K5 one copied word. K1/K2/K6 include all
262,144 chronological samples. The native share is an instruction fraction,
not a sampled fraction of host execution time.

Counter refusal is an explicit error, not a zero measurement. macOS ARM64 hardware
counter collection remains unqualified. The cycle model is documented in
`doc/api.md`; these measurements do not imply consumer throughput or a board clock.
