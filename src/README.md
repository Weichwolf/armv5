# Production sources

`decode.cpp` classifies preparation-only ARM encodings. `image.cpp` owns validated
code declarations and checked artifacts; `lower.cpp`, `alu.cpp` and `memory.cpp`
emit LLVM IR directly, with SSA registers, natural-block admission and cold exits.
`native.cpp` supplies the standalone LLVM optimization/binding path; `core.cpp`
owns CPU state, native bindings and consumer memory/callback access. No instruction
is decoded or compiled at execution time. Remaining profile work is recorded in
`doc/isa-coverage.md`.
