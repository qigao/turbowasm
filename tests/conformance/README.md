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
On Windows with the Salts 2.1.0 SDK, the complete ASan run at `051d149` passed
135/135 tests in 370.34 seconds, including the Core 3.0 gates, GC lifecycle,
scalar result/call tuples, all atomic descriptors and optional CNet adapter tests.
The earlier CNet timeout was a process-launch failure: its CTest `ENVIRONMENT`
property split Windows `PATH` at semicolons and removed the ASan runtime DLL
directory. `ENVIRONMENT_MODIFICATION` now prepends SDK paths while preserving
the launch-time developer-shell environment. The existing adapter test passed
in 0.14 seconds after the fix.
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
compiles this same suite as `turbowasm_mir_memory64_test`: shared and unshared
invocations additionally require `TURBOWASM_JIT_COMPILED`, including trapping
calls and imported concurrent workers, so interpreter fallback cannot pass the native gate. WAT fixture sources
and generated C byte arrays are kept together under `tests/fixtures/`.

The `core3-mir` CI entry uses `ci-core3-mir-user` and the pinned Core 3.0
`memory64/` suite, comparing interpreter and MIR results with zero unsupported
commands and nonzero native compilation required. Existing Linux/macOS
`ci-mir-user` and `ci-macos-mir-user` jobs execute the native unit tests as well.
The macOS profile uses GCC 15 to match the published Salts TinyTest thread-local
ABI. Unix CI bootstraps the shared cache contract's hash-verified vcpkg tool
before invoking its setup action; hosted runner tool updates cannot silently
change that contract. Shared backing uses Runtime synchronization in both tiers;
each concurrent instance owns a distinct MIR backend/context.

At revision `051d149`, [native CI](https://github.com/qigao/turbowasm/actions/runs/37601573736)
passed all five jobs: Windows qualification and installed consumers, Linux
qualification and installed consumers, Android build/installed consumers, and
Linux/macOS MIR. Both MIR jobs passed 147/147 tests, including mandatory native
memory64 execution, mixed scalar arguments, four-argument calls, NaN payloads,
negative zero and bounds traps. `scalar_results_test.c` additionally requires
compiled callers for empty and mixed scalar result tuples, eight-result calls,
function-label branches, self tail calls, nested compiled/interpreted calls,
host re-entry, allocation failure/recovery and fuel/trap parity.
`atomic_lowering_test.c` checks all 63 atomic descriptors against interpreter
results and backing bytes, in shared/unshared memory32/memory64, with alignment
and bounds traps. The memory64 suite additionally checks concurrent increments,
wait/notify across growth, fence, wait interruption cleanup and fuel before an
atomic side effect. The separate
[Core 3.0 memory64 differential gate](https://github.com/qigao/turbowasm/actions/runs/37601580290)
also passed. This memory64 gate does not qualify native GC/table64, full
exception or reference/vector call boundaries. General direct scalar tail paths
have a separate qualification below.

The local Windows build can execute Runtime/helper regressions, but cannot
build MIR: the configured `mir-jit` vcpkg port supports Linux, macOS and Android.
A Windows pass is not evidence that the native MIR gate passed.

## General scalar tail-call qualification

At revision `f2d885e`, [native CI](https://github.com/qigao/turbowasm/actions/runs/37614909873)
passed all five jobs. Linux MIR passed 152/152 tests in 0.42 seconds and macOS
arm64 MIR passed 152/152 in 1.55 seconds. Windows qualification passed 137/137
plus 16/16 installed-package tests; Linux qualification passed 138/138 plus
17/17 installed-package tests. Android passed cross-compilation and the existing
installed-consumer builds; no Android runtime execution is claimed.

`scalar_tail_calls_test.c` requires compiled callers and both sides of a 4096-step
mutual tail chain with alternating 4/21 scalar parameters. It checks mixed result
bits, empty result tuples, zero-argument targets, interpreter targets, host
re-entry, traps, every Runtime allocation failure in a growing chain, and fuel
boundaries against the interpreter. The same suite uses a test backend on
platforms without MIR to verify dispatcher ownership and constant logical depth.
The structured-tail tiering regression now requires compilation as well.

The Runtime implementation at `825ca7b` passed the Windows ASan profile: 139/139
CTest entries in 252.81 seconds, including both Core 3.0 gates. An initial launch
without the MSVC developer environment failed to load runtime DLLs and was
stopped; the successful run used `VsDevCmd.bat` plus both SDK roots. The fixture
and native-expectation follow-up at `f2d885e` passed the local tail-call target
again. It replaces the fixture's unsupported MIR `i32.eqz` with an equivalent
supported `if`; it does not add `i32.eqz` lowering or weaken native admission
assertions.

This qualifies direct scalar tail transfers, including non-self and imported
calls, with arbitrary validated scalar tuple arity. It does not qualify indirect
tail calls, reference/vector signatures, the remaining scalar opcode set, native
GC/table64 or full exception lowering. Component Model async/future/stream and
broader WASI 0.2 coverage remain separate work.

## Native table storage qualification

At revision `32e8de1`, [native CI](https://github.com/qigao/turbowasm/actions/runs/37616984551)
passed all five jobs. Linux MIR passed 154/154 tests in 0.49 seconds and macOS
arm64 MIR passed 154/154 in 1.44 seconds. Windows qualification passed 138/138
plus 16/16 installed-package tests; Linux qualification passed 139/139 plus
17/17 installed-package tests. Android cross-compilation and installed-consumer
builds passed; this is not an Android runtime result. The local Windows ASan
selection passed 16/16 adjacent tests in 1.04 seconds.

`table_storage_test.c` covers table.size, table.init, table.copy and elem.drop
on table32/table64. Native builds require compiled callers. The eight cases check
current size after growth, both overlap directions, mixed address widths,
indices/lengths above 32 bits and signed i64 boundaries, empty ranges, consumed
element segments, imported backing and original funcref owners, fuel before
mutations, helper error propagation, and every Runtime table.init allocation
failure. Expected status, trap, scalar result, all table entries and segment drop
state are compared against the interpreter. Fixture binaries were generated and
validated with the pinned wasm-tools 1.261.0.

These tests qualify this table storage subset. At that revision they did not
qualify native reference-value frames or table.get/set/grow/fill; those are covered
by the increment below. Indirect calls and GC instruction lowering remain gaps.
The full interpreter Core 3.0 gate is separate from native subset qualification.

## Native reference frame qualification

At revision `3bb8acc`, [native CI](https://github.com/qigao/turbowasm/actions/runs/37621377230)
passed all five jobs. Linux MIR passed 156/156 tests in 0.73 seconds and macOS
arm64 MIR passed 156/156 in 1.65 seconds. Windows qualification passed 139/139
plus 16/16 installed-package tests; Linux qualification passed 140/140 plus
17/17 installed-package tests. Android cross-compilation and installed-consumer
builds passed; no Android runtime execution is claimed.

The Runtime implementation and tests at `40d338f` passed the complete Windows
ASan profile: 141/141 tests in 261.99 seconds, including Core 3.0
`pass=63970 fail=0 unsupported=0` and the reference gate
`pass=1364 fail=0 unsupported=0`. The only subsequent code change in `3bb8acc`
corrected MIR address temporaries to use integer local registers. The first native
run caught invalid `local p` declarations on macOS; its Linux MIR job stopped in
vcpkg bootstrap before building TurboWasm. The successful run above tests the
corrected revision on both native platforms.

`reference_native_test.c` has 11 cases and requires compiled functions in the MIR
variant. It checks reference locals, structured branches and typed select; mixed
reference parameters/results; direct calls and 1024-step mutual tail chains;
native-only GC values across host collection and re-entry; self-tail local reset;
table32/table64 get/set/grow/fill; full-width bounds and failed-fill atomicity;
fuel and interruption parity; logical call depth; stale, foreign-store, null and
nominal type rejection; root-budget exhaustion; and every Runtime allocation
failure in nested reference calls. Managed externref identity, function owners
and all supported null carriers are preserved. The table storage, scalar, SIMD,
memory64 and atomic native regressions also passed with the expanded private ABI.

This qualifies rooted reference cells and the listed operations, not a complete
native Core 3.0 backend. General indirect calls/tails have the subsequent
qualification below. Native GC instructions, full EH, reference globals and
remaining reference-control instructions, vector
call signatures and remaining numeric instructions still need implementation and
qualification. Public resumable execution continues to use the interpreter.
Component UTF-16/Latin1+UTF-16, post-return and full async/future/stream coverage,
and broader WASI 0.2 data paths remain separate work.

## Native indirect call qualification

At revision `e4b7777`, [native CI](https://github.com/qigao/turbowasm/actions/runs/37625269227)
passed all five jobs. Linux MIR passed 158/158 tests in 1.44 seconds and macOS
arm64 MIR passed 158/158 in 1.98 seconds. Windows qualification passed 140/140
plus 16/16 installed-package tests; Linux qualification passed 141/141 plus
17/17 installed-package tests. Android cross-compilation and installed-consumer
builds passed; no Android runtime execution is claimed.

The shared resolver and initial implementation at `f05eeb3` passed the complete
Windows ASan profile: 142/142 tests in 236.49 seconds, including both Core 3.0
gates. Its [first native run](https://github.com/qigao/turbowasm/actions/runs/37624273321)
also passed all five jobs. Follow-up `e4b7777` resolves imported function aliases
inside the native tail dispatcher, preventing a tail chain from accumulating
interpreter wrapper frames. That follow-up passed six local ASan regressions in
4.07 seconds and the complete native CI above.

`indirect_native_test.c` has ten cases and requires compiled callers in its MIR
variant. It covers `call_indirect`, `call_ref`, `return_call_indirect` and
`return_call_ref` with table32/table64, seven-value mixed scalar/reference tuples,
NaN payloads and negative zero, empty results, zero-argument host targets,
interpreted targets, host collection/re-entry, function subtyping and null/bounds/
signature traps. Imported tables retain their original provider; tests require
compiled provider functions and verify pending exceptions return to the caller.
Cross-store scalar targets release their temporary root registration, and legacy
instance-relative funcrefs bind their owner before a cross-instance transfer.

Long table/reference tail chains and a provider/imported-alias cycle run for 1024
transfers under a 32-root limit. Native-only managed values survive host-triggered
collections, while frame registrations and unreachable objects are released at
completion. Fuel boundaries are compared with the interpreter and every Runtime
allocation failure in ordinary and tail reference calls is checked for cleanup
and recovery. The fixture binaries were generated and validated with wasm-tools
1.261.0. Host tests use the existing host-signature API; this increment does not
add GC carriers to host signature declarations.

These results qualify scalar/reference indirect calls and tails through Runtime,
not full native Core 3.0 execution. Native GC and EH instructions, reference
globals and remaining reference control are still gaps. Vector call signatures
and scalar numeric instructions are covered by the increments below.
Public resumable execution remains on the
interpreter. Component encoding/post-return/async coverage and broader WASI 0.2
data paths remain separate work.

## MIR vector call boundary

Implementation `31cb3d0` adds vector parameters, locals and results to the private
pointer-based MIR entry. Complete vector values travel between call cells and
invocation-owned slots, preserving all 128 bits and shape metadata. Slot allocation
uses Runtime's allocator and quota; nested invocation saves/restores the parent
frame, and self-tail dispatch resets non-parameter vector locals.

The first implementation passed Windows ASan's complete 143/143 suite in 210.31
seconds, including both Core 3.0 gates. Its
[native CI run](https://github.com/qigao/turbowasm/actions/runs/37627735445)
passed all five jobs, including Linux and macOS MIR execution. Test-only follow-up
`23c578b` extends `vector_calls_test.c` to seven cases. The MIR variant requires
compiled callers and imported providers; it covers direct/indirect/reference
calls and tail forms, host collection/re-entry, interpreted targets, mixed tuples,
16-vector results, local snapshots, control merges, native-only managed locals,
imported functions/tables, fuel/traps and every call allocation failure.

The [follow-up CI run](https://github.com/qigao/turbowasm/actions/runs/37628363469)
also passed all five jobs at `23c578b`:

- Linux MIR: 160/160 tests, 1.36 seconds.
- macOS arm64 MIR: 160/160 tests, 2.36 seconds.
- Windows qualification: 141/141 tests; installed package: 16/16.
- Linux qualification: 142/142 tests; installed package: 17/17.
- Android arm64: cross-build and installed consumer build; no device execution.

Local follow-up validation passed four related ASan tests in 4.43 seconds and the
final native-local GC scenario in the vector suite in 0.08 seconds. Reproduce with
the configured Windows ASan user preset:

```powershell
cmake --build --preset win-core3-asan-user
ctest --preset win-core3-asan-user -R "vector_calls|jit_simd_helper|reference_native|indirect_native" --output-on-failure
```

This qualifies the helper-backed vector call boundary. It does not add a MIR
vector register ABI, native GC/EH instructions or resumable native execution.
Remaining SIMD instructions are covered by the later increment below.

## MIR scalar numeric instructions

Implementation `b85048b` extends the structured MIR path to all 128 scalar numeric
opcodes (`0x45` through `0xc4`) and all eight saturating conversions. Existing
direct arithmetic lowering is retained; the other operations call the same
primitives used by the interpreter through a bounded two-value local stack.
Floating constants are materialized from their original bits, including NaN
payloads, infinities and negative zero. No installed API changes are required.

`numeric_native_test.c` independently validates each generated binary module and
compares native execution against the interpreter over integer extrema, shift
widths, signed/unsigned boundaries, zero, subnormal/normal floats, ties, conversion
limits, infinities and quiet/signaling NaNs. It also compares every instruction's
fuel boundary, checks exact constant and bit-operation results, and verifies that
helper failures do not publish a result. Every function must be compiled in the
MIR variant. The numerical contract is the
[Core specification](https://webassembly.github.io/spec/core/exec/numerics.html).

The first native run passed the new numeric test on Linux and macOS; two older
tests still expected integer division to be ineligible. Follow-up `2ec239d`
updates those expectations and explicitly retains interpreted callees in the
mixed-tier tuple tests. Windows ASan passed the five initial related tests in
6.03 seconds and four follow-up regressions in 5.59 seconds. The complete Windows
ASan run passed 144/144 tests in 287.85 seconds, including both Core 3.0 gates.

[Native CI at `2ec239d`](https://github.com/qigao/turbowasm/actions/runs/37630910538)
passed all five jobs:

- Linux MIR: 162/162 tests, 1.59 seconds.
- macOS arm64 MIR: 162/162 tests, 2.84 seconds.
- Windows qualification: 142/142 tests; installed package: 16/16.
- Linux qualification: 143/143 tests; installed package: 17/17.
- Android arm64: cross-build and installed consumer build; no device execution.

```powershell
cmake --build --preset win-core3-asan-user
ctest --preset win-core3-asan-user -R "numeric_native|scalar_results|scalar_tail|indirect_native|vector_calls" --output-on-failure
```

Scalar numeric coverage does not qualify native GC/EH, remaining reference
control/globals or resumable native execution.

## Complete helper-backed SIMD admission

Implementation `f27e804` adds shuffle, lane extract/replace and extending, splat,
zero and lane memory operations to MIR's existing SIMD path. The private bridge
executes one validated instruction with a fixed local stack, sharing Runtime's
vector semantics and shared/unshared memory32/64 access checks. The module owns
the immutable instruction view until compiled code is destroyed. No installed
API or MIR vector register ABI is introduced.

`simd_native_test.c` visits every shared SIMD descriptor, every lane and both
shuffle patterns. The generated modules are independently validated, and their
MIR variants require compiled functions. Tests compare bits and shape, memory
contents, fuel boundaries and traps against the interpreter. Memory descriptors
run with two memories in all four address/shared modes, exercising the explicit
nonzero memory index, unaligned addresses, offsets, boundary and overflow traps.
The existing v128 constant/load/store and structured SIMD suites remain active;
the shuffle test now requires native compilation instead of interpreter fallback.

The first Windows ASan check passed all four relevant tests in 0.58 seconds.
The complete Windows ASan suite passed 145/145 tests in 223.15 seconds, including
both Core 3.0 gates.

[Native CI at `f27e804`](https://github.com/qigao/turbowasm/actions/runs/37632774787)
passed all five jobs:

- Linux MIR: 164/164 tests, 1.53 seconds.
- macOS arm64 MIR: 164/164 tests, 2.58 seconds.
- Windows qualification: 143/143 tests; installed package: 16/16.
- Linux qualification: 144/144 tests; installed package: 17/17.
- Android arm64: cross-build and installed consumer build; no device execution.

```powershell
cmake --build --preset win-core3-asan-user
ctest --preset win-core3-asan-user -R "simd_native|jit_simd_helper|vector_calls|memory64_execution" --output-on-failure
```

This qualifies instruction admission through Runtime helpers, not platform SIMD
register generation or full native Core 3.0. Native GC/EH, remaining reference
control/globals and resumable native execution remain separate work.
