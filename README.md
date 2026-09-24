# TurboWasm

TurboWasm is a small C11 WebAssembly runtime with explicit semantic, sandbox,
and backend boundaries.

## Current runtime

The repository currently provides:

- bounded WebAssembly binary reading and module admission;
- retained validation metadata for sections, control signatures, functions,
  data/element segments, tables, memory, references, bulk operations and SIMD;
- a baseline interpreter with structured control, direct/indirect calls,
  memory/table semantics, reference values, bulk operations and SIMD;
- shared fuel and interruption semantics across interpreted and compiled
  execution;
- scalar value kinds plus typed `v128` lane refinement;
- CMeta-backed vector/mask semantic descriptors;
- portable SIMD execution through `Salts::SIMD`;
- an optional lazy MIR JIT for eligible hot functions;
- helper-backed SIMD MIR lowering with invocation-local private `v128` slots,
  including structured control flow;
- an explicit MIR executable-mapping budget;
- C/C++ public ABI tests;
- an installed CMake package and a small module-validation CLI.

The current implementation is intentionally scoped. Threads, exception handling,
relaxed SIMD, WASI/host binding adapters, and other post-MVP WebAssembly
features are not implied by the completed baseline/JIT work.

## Dependency boundary

```text
qigao/vcpkg-cache
    -> SIMDe package cache
    -> MIR JIT package + executable-memory limit

Salts master
    -> CMeta
    -> Salts::SIMD (SIMDe is private)

TurboWasm
    -> Wasm decoding / validation / sandbox semantics
    -> retained typed validation metadata
    -> interpreter
    -> optional lazy MIR JIT
```

TurboWasm never includes SIMDe directly and does not expose MIR types through
the installed `TurboWasm::Runtime` target.

## Build

Install Salts from its current `master` branch and point `SALTS_ROOT` at the
installed SDK:

```sh
export SALTS_ROOT=/path/to/salts-sdk

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

To enable the optional MIR backend, install the canonical `mir-jit` profile
from `qigao/vcpkg-cache` and configure with:

```sh
cmake -S . -B build-mir -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DTURBOWASM_ENABLE_MIR_JIT=ON
```

CI builds Salts from `master` and restores MIR from the shared binary cache on
Linux and macOS. The installed Runtime export remains MIR-free.

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

The JIT keeps the same ownership model:

```text
validated SIMD
      |
      v
private invocation-local v128 slots
      |
      v
TurboWasm helper ABI
      |
      v
Salts::SIMD
```

The pinned MIR v1.0 backend does not provide a vector register type, so
TurboWasm does not claim a native MIR-v128 ABI. Helper-backed SIMD is the
canonical compiled path for this backend.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the complete ownership and execution
model.
