# TurboWasm

TurboWasm is a small C11 WebAssembly runtime built around explicit semantic boundaries:

- WebAssembly decoding, validation, module/instance semantics and sandboxing live here.
- CMeta supplies shared semantic type/reflection contracts.
- Salts::SIMD supplies portable SIMD execution; TurboWasm never exposes SIMDe.
- CFlow/Executor integration is used only where generic execution orchestration fits.
- MIR is planned as an optional lazy native JIT backend, never as the sandbox owner.

Design and bootstrap work is tracked in issue #1.
