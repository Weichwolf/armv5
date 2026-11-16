# Primary references

Start with the ARM Architecture Reference Manual DDI 0100I for instruction
semantics and architecture-version restrictions. It also covers later/optional
features: apply the ARMv5TEJ profile instead of implementing every chapter by
default. Then use ARM926EJ-S TRM DDI 0198E for CP15, MMU/FCSE, cache/TCM and
implementation-defined behavior. The AM1705 datasheet is integration context,
not an instruction-set specification or proof of Nord's exact MCU.

doc/reference-manifest.json is the authoritative local-file inventory. Entries
record manufacturer URLs, the actual source used, revision, byte count, SHA-256,
page count and provenance limitations. PDFs/text stay ignored under doc/local/.
Verification runs offline; fetching never bypasses a changed identity silently.

Read the manufacturer's license/copyright terms in each document. Local access
does not grant redistribution rights. No manual content or manufacturer program
bytes are compiled into this project, and no manuals are committed to Git.

The initial design was compared read-only with ../cpu12 and ../dsp56300 at
revisions 717346a and ec25ce0 respectively. Their generic native-region,
ownership and verification principles are a design reference; their CPU/DSP
instruction semantics do not implement this target architecture.

## Local PDF inventory

| Document | Revision | Pages | Manufacturer source |
|---|---|---:|---|
| ARM926EJ-S Technical Reference Manual | r0p5, 2008 | 236 | [Official reference](https://documentation-service.arm.com/static/5e8e3d1088295d1e18d3a9b2) |
| ARM9E-S Technical Reference Manual | Rev 1, Issue B, September 2000 | 290 | [Official reference](https://documentation-service.arm.com/static/5e8e2f18fd977155116a77fb) |
| ARM Architecture Reference Manual | Issue I, 2005 | 1138 | [Official reference](https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a) |
| AM1705 ARM Microprocessor Datasheet | Revision F, January 2017 | 167 | [Official reference](https://www.ti.com/lit/ds/symlink/am1705.pdf) |

Use the filenames in reference-manifest.json to open the corresponding PDFs and
extracted .txt files under doc/local/. All local copies have verified identities.
