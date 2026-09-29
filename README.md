# TurboWasm

TurboWasm is a small C11 WebAssembly runtime with explicit semantic, sandbox,
and backend boundaries.

## Current runtime

The repository currently provides:

- bounded WebAssembly binary reading and module admission;
- retained validation metadata for sections, control signatures, functions,
  data/element segments, tables, memory, references, bulk operations and SIMD;
- a semantic-reference interpreter with structured control, direct/indirect
  calls, memory/table/reference/bulk operations, SIMD, tail calls and typed
  exception handling;
- proposal-aware multi-memory, custom page sizes, extended const and relaxed
  SIMD semantics with pinned upstream qualification;
- typed tag/exception identity and cross-frame unwind across direct, indirect,
  imported and tail-call boundaries;
- shared fuel and interruption semantics across interpreted and compiled
  execution, plus restartable interpreter execution with fuel/interruption/
  host-wait yields;
- scalar value kinds plus typed `v128` lane refinement;
- CMeta-backed vector/mask semantic descriptors;
- portable SIMD execution through `Salts::SIMD`;
- an optional lazy MIR JIT for eligible hot functions;
- helper-backed SIMD MIR lowering with invocation-local private `v128` slots,
  including structured control flow;
- an explicit MIR executable-mapping budget;
- typed cross-instance linking for functions, globals, memories, tables and
  tags, plus typed synchronous host-function providers;
- optional `TurboWasm::CFlow` deadline/scheduling projection;
- optional `TurboWasm::NativeIO` bounded async host-wait bridge;
- C/C++ public ABI tests;
- an installed CMake package and a small module-validation CLI.

The current implementation is intentionally scoped. WASI Preview 1 capability
adapters are being layered on the typed host boundary. Shared-memory threads/
atomics, memory64, WebAssembly GC and the Component Model remain outside the
completed Runtime surface.

## Dependency boundary

```text
qigao/vcpkg-cache
    -> SIMDe package cache
    -> MIR JIT package + executable-memory limit

Salts SDK selected by package acquisition
    -> CMeta
    -> Salts::SIMD (SIMDe is private)
    -> Salts::Coroutine (private resumable Runtime implementation)
    -> optional Salts::CFlow / Salts::NativeIO adapters

SaltsUtils
    -> ecosystem companion only
    -> not linked by TurboWasm

TurboWasm::Runtime
    -> Wasm decoding / validation / sandbox semantics
    -> retained typed validation metadata
    -> interpreter + restartable execution
    -> optional lazy MIR JIT

TurboWasm::CFlow / TurboWasm::NativeIO
    -> optional host scheduling / async-I/O projections
    -> do not enter the Runtime public link interface
```

TurboWasm never includes SIMDe directly and does not expose MIR types through
the installed `TurboWasm::Runtime` target.

## Build

Install a compatible Salts SDK and point `SALTS_ROOT` at it. TurboWasm does not
pin the SDK version in CMake and does not link SaltsUtils directly:

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

CI restores the latest published Salts SDK from GitHub Packages and restores MIR
from the shared binary cache on Linux and macOS. The installed Runtime export remains MIR-free.

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
model, and [docs/PORTABILITY.md](docs/PORTABILITY.md) for the C11, hosted-libc,
optional-service, and embedded build boundary.
