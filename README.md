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
- proposal-aware multi-memory, custom page sizes, extended const, relaxed
  SIMD and memory64 semantics with pinned upstream qualification;
- Core 3.0 recursive types/subtyping, typed function references, GC structures/arrays,
  explicit store/root ownership and table64 addresses;
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
- shared-memory/atomic execution with wait/notify and qualified legacy WASI
  threads support;
- a capability-gated `TurboWasm::WASI` Preview1 layer for args/environment,
  clocks, random, fd I/O, proc_exit and filesystem operations;
- a bounded generation-safe WASI filesystem provider ABI with Salts HostFS and
  optional littlefs implementations;
- optional `TurboWasm::WASINativeIO` async fd projection and
  `TurboWasm::WASIThreads` CFlow-backed thread-spawn/group lifecycle;
- caller-owned Runtime allocation plus module/allocation/linear-memory/table
  resource limits for embedded deployments;
- C/C++ public ABI tests;
- an installed CMake package and a small module-validation CLI.

The current implementation is intentionally scoped. Core shared-memory atomics,
legacy WASI threads, the qualified Preview1 capability layer, and interpreter
memory64 are part of the completed Runtime surface. Shared memory64 supports
atomic operations, wait/notify, imports and guarded growth. MIR admits unshared
memory64 modules and lowers scalar load/store, size/grow, bulk memory and its
existing SIMD memory subset through Runtime helpers, within its existing
function-signature and instruction admission limits. Shared memories continue
to use the interpreter. Native MIR memory64 verification runs in the Linux and
macOS MIR test profiles; the Windows MIR dependency is not supported. The synchronous Component Model subset is exposed separately
through the optional `TurboWasm::Component` façade; it does not enter the
`TurboWasm::Runtime` public ABI. WASI 0.2 remains a separate capability layer
tracked by Runtime v2 (#301). GC and table64 execute in the interpreter; native
MIR lowering of these operations is not claimed.

## Core 3.0 and managed references

The interpreter passes the complete pinned [Core 3.0 suite](tests/conformance/README.md):
258 files, 63970 binary commands, zero failures and zero unsupported commands.
The 1229 text-syntax assertions are checked by wasm-tools and reported separately;
TurboWasm itself accepts binary modules. This qualification does not imply full
Component Model, WASI 0.2, or native JIT coverage.

Modules with GC operations or composite types use `turbowasm_instance_create_in_store`.
Create one `turbowasm_store` on its owner thread and use it for the consumer and
all instance providers linked to it (host-function bindings need no instance). Ordinary instance creation rejects these modules
with `TURBOWASM_INVALID_ARGUMENT`. Existing non-GC creation remains available.

Returned GC values borrow their object until the next allocation/collection in the
store. Call `turbowasm_root_retain` before keeping a value across that boundary and
`turbowasm_root_release` when done. Copying `turbowasm_value` does not retain it.
Globals, tables, element segments, active/suspended frames and execution results
are traced automatically. Destroy executions and instances, release host roots,
then destroy the store. Function/exception references inside GC objects still
borrow their original owner instances. All store operations use the owner thread;
shared-memory threads do not enable shared GC execution.

See the complete, installed-package-tested [GC example](examples/gc.c) and
[store API](include/turbowasm/store.h). It retains a struct across instance/module
destruction and collection. To build the example independently, set
`CMAKE_PREFIX_PATH` to the installed TurboWasm and Salts SDK roots when configuring
`examples/CMakeLists.txt`.

Store defaults are finite: 64 MiB of requested metadata/payload bytes, 65536
objects, and 4096 root registrations (host roots plus active frames). Override
these through `turbowasm_store_config`; exhaustion returns `OUT_OF_MEMORY`.
Table64 accessors preserve all 64 address bits. Dense table storage is limited to
`UINT32_MAX` elements and `runtime.limits.max_table_elements`; growth beyond the
limit returns the Wasm failure sentinel without changing the table.

Rebuild consumers for the expanded public value/API surface. Artifact schema 2
retains GC type groups, store requirements and 64-bit table limits; regenerate
schema 1 artifacts from source.

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

TurboWasm::WASI / WASIThreads / WASINativeIO / WASIHostFS / WASILittleFS
    -> optional host capability layers
    -> remain separate from TurboWasm::Runtime

TurboWasm::Component
    -> optional synchronous Component Model façade
    -> public component.h is opt-in and is not included by turbowasm.h
    -> depends on TurboWasm::Runtime, never the reverse
```

TurboWasm never includes SIMDe directly and does not expose MIR types through
the installed `TurboWasm::Runtime` target.

## Build

Use CMake 3.25+, Ninja, the latest released Salts SDK, and the shared
[qigao/vcpkg-cache](https://github.com/qigao/vcpkg-cache) toolchain. The Salts
2.x API uses `cmeta_v128`, `cmeta_simd_*`, `cmeta_*` platform/filesystem
functions, and `<coro.h>`. TurboWasm does not pin the SDK version or provide
aliases for the removed Salts names.

`CMakeOptions.cmake` owns feature defaults. The versioned `CMakeUserPresets.json`
provides configure/build/test/install entry points; `presets/` contains shared
base settings. Third-party dependencies use the root `vcpkg.json`: enabling
`TURBOWASM_ENABLE_MIR_JIT` selects `mir`, while conformance selects host WABT.
The shared NuGet feed is read-only and the local binary cache is writable.
Provide `GITHUB_TOKEN` with package read access in the parent environment;
presets do not load `.env`.

For local SDK acquisition, run the following in PowerShell with .NET SDK 8+
on `PATH` (the runtime alone is insufficient):

```powershell
./cmake/ci/restore-salts-sdk.ps1 -Local -SaltsRid windows-x64
```

This resolves the latest package with `--no-cache --force-evaluate` and exports
`SALTS_ROOT` in that PowerShell process. Alternatively, set `SALTS_ROOT` to an
already installed release SDK. `-WithSaltsUtils` additionally provides
`SALTS_UTILS_ROOT` for `TURBOWASM_QUALIFY_WASI_ADAPTER_PLAN`.

Set `PROJECT_ROOT` to the parent containing `external/pkgs`, and `VCPKG_ROOT`
to the vcpkg checkout. Windows expects the cache checkout at
`%LOCALAPPDATA%/qigao/vcpkg-cache`; Linux uses `VCPKG_CACHE_REPOSITORY_ROOT`
and requires Mono for NuGet restore. SDK lookup is restricted to `SALTS_ROOT`;
`CMAKE_PREFIX_PATH` contains only the matching vcpkg profile.

From a Windows VS developer environment (`VsDevCmd.bat -arch=x64 -host_arch=x64`):

```bat
cmake --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user
cmake --build --preset install-win-release-user
```

On Linux, use `linux-release-user` for the same four commands. The `win-dev-user`
and `linux-dev-user` presets select Debug and require a matching Debug Salts SDK
in `SALTS_ROOT`. Build and vcpkg installed trees are separate per profile;
installation uses `$PROJECT_ROOT/external/pkgs/turbowasm/debug|release`.
To select the optional MIR backend locally, set `TURBOWASM_ENABLE_MIR_JIT` in
the selected user preset before configuring; no manual MIR prefix is needed.

Windows CI uses `windows-2025` and the Windows triplet supplied by the shared
cache action, matching its compiler/SDK contract.

CI and native SDK releases share `.github/workflows/native-build.yml` and use
`ci-*-user` presets. CI restores the latest published SDK on every run and
builds the complete selected graph. The published package remains Runtime +
Component only; MIR remains outside the installed Runtime link interface.
Android uses the shared outer vcpkg toolchain with the NDK chainloaded, and its
existing installed consumers are cross-compiled without running host CTest.

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
