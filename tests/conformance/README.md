# TurboWasm WebAssembly spec conformance harness

This directory is test infrastructure and is not installed with `TurboWasm::Runtime`.

## Core 3.0 qualification

The complete suite is [WebAssembly/spec `wg-3.0`](https://github.com/WebAssembly/spec/tree/9d36019973201a19f9c9ebb0f10828b2fe2374aa),
commit `9d36019973201a19f9c9ebb0f10828b2fe2374aa`. Its 258 `.wast` files are
recursively discovered, including GC, SIMD, exception handling and memory64/table64.
Conversion uses [`wasm-tools` 1.261.0](https://github.com/bytecodealliance/wasm-tools/releases/tag/v1.261.0),
whose JSON format covers Core 3.0 syntax and permitted relaxed-SIMD result sets.

The qualified Windows interpreter run reports:

```text
CORE_CONFORMANCE pass=63970 fail=0 unsupported=0 total=63970 files=258
TEXT_FRONTEND_CONFORMANCE pass=1229 fail=0 provider=wasm-tools (excluded from TurboWasm binary counts)
```

`--all-core` defaults to a zero unsupported budget. A failure, unsupported binary
command, frontend failure, missing/empty suite, failed conversion or failed runner
makes the gate fail. Frontend syntax rejection is never counted as a TurboWasm
validation pass. `assert_invalid` text modules are converted into binary and then
validated by TurboWasm; syntax-only `assert_malformed` text cases are checked by
wasm-tools and reported separately. All ordinary binary module validation,
instantiation, linking, execution and trap assertions run through TurboWasm.

The runner supports module definitions/fresh instances, named registration, invoke
and global-get actions, exact scalar/vector results, permitted NaN classes,
whole-vector alternatives, reference patterns, and exception/trap assertions.
A shared explicit GC store roots instances and host arguments. Failed
instantiations preserve escaped state through a private test hook until teardown.
These hooks do not expand the installed Runtime ABI.

## Reproduce locally

Set `TURBOWASM_CORE3_TOOLS` to the executable and `TURBOWASM_SPEC_ROOT` to the pinned
checkout. With the usual Salts SDK environment and MSVC developer shell:

```powershell
cmake --preset win-core3-user
cmake --build --preset win-core3-user
ctest --preset win-core3-user --output-on-failure
```

For AddressSanitizer, use `win-core3-asan-user` for all three commands. Run CTest
from the MSVC developer shell so the ASan runtime DLL is on `PATH`.
Instrumented native executables and resumable coroutine stacks use an explicit
8 MiB budget (`TURBOWASM_SANITIZER_STACK_BYTES`): MSVC's default executable stack
was exhausted by ASan frames before the interpreter's Wasm call-depth trap.
The ordinary build keeps its existing stack/depth policy.
On Windows with the Salts 2.1.0 SDK, the complete ASan run passed 133/133 tests,
including the Core 3.0 gates, GC lifecycle and optional CNet adapter tests.
The earlier CNet timeout was a process-launch failure: its CTest `ENVIRONMENT`
property split Windows `PATH` at semicolons and removed the ASan runtime DLL
directory. `ENVIRONMENT_MODIFICATION` now prepends SDK paths while preserving
the launch-time developer-shell environment. The existing adapter test passed
in 0.14 seconds after the fix; the full regression completed in 231.22 seconds.
The smaller `turbowasm_conformance_core3_references` test gives fast feedback on
reference control, recursive types, bit operations and validation. Its success
does not substitute for the full gate. `test_run_core.py` tests strict failure
accounting, recursive discovery, integer encodings, alternatives and frontend
separation without executing Wasm.

## CI and legacy proposal gates

`.github/workflows/conformance.yml` runs the full `core3` matrix entry on pull
requests using `ci-core3-user` with ASan. It pins the specification commit and
wasm-tools release archive SHA-256. Runtime builds still use the shared Salts SDK,
vcpkg cache and CMake preset setup. The tool is test-only; Runtime gains no parser
or validation dependency on wasm-tools. Linux CI results require the workflow to
run; a local Windows pass does not certify other platforms.

Existing `core`, `mir`, exception-handling, threads, tail-call, custom-page-sizes,
memory64, extended-const, multi-memory and relaxed-simd entries retain their pinned
WABT/proposal inputs. Revisions and selectors are in `proposals.json`; MIR
comparison checks eligible compiled paths against the interpreter. Threads and
custom page sizes are separate extensions, not additional Core 3.0 requirements.
Component Model provenance is checked separately by `verify_component_model.py`.

```sh
cmake --preset ci-core3-user
cmake --build --preset ci-core3-user
ctest --preset ci-core3-user -R '^turbowasm_conformance_(driver|core3)$'
```

CI prepares `SALTS_ROOT`, `VCPKG_ROOT`, `VCPKG_CACHE_REPOSITORY_ROOT`,
`VCPKG_BINARY_SOURCES`, `VCPKG_OVERLAY_PORTS`, `TURBOWASM_SDK_RID`,
`TURBOWASM_TRIPLET` and `TURBOWASM_HOST_TRIPLET`. The workflow supplies the pinned
specification checkout and `TURBOWASM_CORE3_TOOLS` before configuration.

## Memory64 native and shared regression

`memory64_execution_test.c` covers shared memory64 atomic operations, imported
concurrent increments, wait/notify across growth, artifact restore, 64-bit
address/offset bounds, bulk memory, and the native helper ABI. The MIR build
compiles this same suite as `turbowasm_mir_memory64_test`: every native-fixture
invocation additionally requires `TURBOWASM_JIT_COMPILED`, including trapping
calls, so interpreter fallback cannot pass the native gate. WAT fixture sources
and generated C byte arrays are kept together under `tests/fixtures/`.

The `core3-mir` CI entry uses `ci-core3-mir-user` and the pinned Core 3.0
`memory64/` suite, comparing interpreter and MIR results with zero unsupported
commands and nonzero native compilation required. Existing Linux/macOS
`ci-mir-user` jobs execute the native unit tests as well. Shared memories remain
interpreter-only, independent of their address width.

The local Windows build can execute Runtime/helper regressions, but cannot
build MIR: the configured `mir-jit` vcpkg port supports Linux, macOS and Android.
A Windows pass is not evidence that the native MIR gate passed.
