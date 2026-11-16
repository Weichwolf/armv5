# Production sources

`decode.cpp` classifies ARMv5TEJ ARM encodings during preparation only.
`core.cpp` implements the public preparation diagnostics and initial owner
lifecycle. LLVM lowering and native execution begin in A2; no execution decoder,
interpreter or alternate host emitter exists.
