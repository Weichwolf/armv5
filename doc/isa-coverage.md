# ISA and implementation coverage

This is a target inventory, not an implemented decoder table. Every row is
**unimplemented**. Replace coarse rows with documented encoding/semantic
entries as work starts; record independent fixtures and supported-host
evidence before marking an instruction or mode implemented.

| Area | Required evidence | Status |
|---|---|---|
| ARM data processing and predication | Condition codes, shifter carry, all operand forms and PC/SPSR cases | Unimplemented |
| Multiply and DSP extensions | Signed/unsigned long arithmetic, saturation/Q flag, defined alias rules | Unimplemented |
| ARM memory instructions | Byte/halfword/word/doubleword/block access, SWP, writeback, endian/alignment | Unimplemented |
| Thumb and interworking | All classic Thumb forms, BL halves, BX/BLX, PC/SP behavior | Unimplemented |
| Control and exceptions | Modes, banked registers, reset, SWI, aborts, IRQ/FIQ, returns and waits | Unimplemented |
| CP15 and memory system | MMU/FCSE/TLB, permissions/domains, fault registers, maintenance, TCM | Unimplemented |
| Jazelle and optional features | Documented BXJ behavior and explicit capabilities; Java execution unresolved | Unimplemented |
| JIT coherence and hosts | Executable aliases, self-modifying code, precise exits, x86-64 and ARM64 | Unimplemented |

Overall status: documentation/reference seed only. No native execution,
ISA completeness, host validation, real-time or benchmark claim.
