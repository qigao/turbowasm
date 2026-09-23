# TurboWasm

TurboWasm is a small C11 WebAssembly runtime with explicit semantic and backend
boundaries.

## Current bootstrap

The repository currently provides:

- bounded WebAssembly binary reading;
- magic/version module admission;
- opaque module lifetime with borrowed immutable bytes;
- scalar value kinds plus typed `v128` lane refinement;
- CMeta-backed vector/mask semantic descriptors;
- representative SIMD execution through `Salts::SIMD`;
- C/C++ public ABI tests;
- an installed CMake package and a small module-validation CLI.

The bootstrap is intentionally not a complete WebAssembly implementation yet.
Full section validation and the baseline interpreter are the next runtime layers.

## Dependency boundary

```text
qigao/vcpkg-cache
    -> SIMDe package cache

Salts master
    -> CMeta
    -> Salts::SIMD (SIMDe is private)

TurboWasm
    -> Wasm decoding / validation / sandbox semantics
    -> typed Wasm IR
    -> interpreter
    -> future MIR lazy JIT
```

TurboWasm never includes SIMDe directly.

## Build

Install Salts from its current `master` branch and point `SALTS_ROOT` at the
installed SDK:

```sh
export SALTS_ROOT=/path/to/salts-sdk

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CI builds Salts from `master` on Linux, Windows and macOS and uses the shared
`qigao/vcpkg-cache` action for third-party binaries.

## SIMD

WebAssembly stores SIMD values as `v128`, but TurboWasm refines the semantic
lane shape after validation, for example:

```text
i32x4.add -> I32X4
f32x4.mul -> F32X4
i32x4.eq  -> B32X4
```

Storage stays a neutral 16-byte Salts carrier. Lane semantics map to canonical
CMeta descriptors, while execution uses `Salts::SIMD`.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the complete ownership model and JIT
roadmap. Work is tracked in issue #1.
