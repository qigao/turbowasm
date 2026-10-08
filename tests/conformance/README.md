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

## MIR globals and nullable-reference control

Implementation `422ad86` admits `global.get/set`, `br_on_null`,
`br_on_non_null` and `unreachable` in the structured MIR backend. Global access
uses Runtime accessors and complete value cells, including imported aliases,
vector shape, managed externrefs and function owners. Nullable branches reuse
the existing taken-edge merge assignments. Traps and mutations follow each
instruction's fuel checkpoint.

The extended `reference_native_test.c` requires compiled functions in its MIR
variant. It compares both branch outcomes at block/function/loop targets against
the interpreter and checks explicit expected results. Global coverage includes
scalar bit patterns, vector shape, reference carriers, provider/consumer
mutation, collection after native writes, rejected immutable/wrong-type/foreign-
store writes and mutation ordering across all tested fuel boundaries. Reachable
and untaken `unreachable` paths verify trap and fuel priority. The fixture binaries
are generated and validated by pinned wasm-tools 1.261.0.

Windows ASan passed the three focused tests in 0.40 seconds and all 145 tests in
203.53 seconds, including both Core 3.0 gates. The
[native CI run](https://github.com/qigao/turbowasm/actions/runs/37635567464)
at `422ad86` passed all five jobs:

- Linux MIR: 164/164 tests, 1.98 seconds.
- macOS arm64 MIR: 164/164 tests, 2.80 seconds.
- Windows qualification: 143/143 tests; installed package: 16/16.
- Linux qualification: 144/144 tests; installed package: 17/17.
- Android arm64: cross-build and installed consumer build; no device execution.

```powershell
cmake --build --preset win-core3-asan-user
ctest --preset win-core3-asan-user -R "reference_native|vector_calls|scalar_results" --output-on-failure
```

This closes the globals and nullable-reference control gaps. Native GC instructions
(including cast branches), EH and resumable native execution remain incomplete.
Component encoding/post-return/async and broader WASI 0.2 coverage remain separate
work; these test counts do not qualify those features.

## MIR GC instructions and complete Core 3.0 differential replay

Implementation `b13a687` lowers all 31 GC-prefixed operations through the existing
Runtime GC executor. A private instruction view describes fixed, repeated or
heterogeneous operands without allocation. Constructor scratch demand comes from
reachable emission, so an unreachable `array.new_fixed 4294967295` does not
allocate a corresponding argument buffer. Complete argument/result cells use the
existing native frame root source. The store retains object ownership, recursive
type identities, quotas and collection. Cast branches reuse taken-edge control
merges; no installed API or heap layout changes.

`gc_native_test.c` has eight cases covering every opcode with extrema/packed values
and fuel boundaries, heterogeneous constructors, vectors and their shapes,
transitive roots under host collection, repeated native allocations with an
eight-object quota, cast branches at block/loop targets, external identity/null
conversions, dropped segments, null/bounds/cast traps, quota failure and injected
allocation failures. Its MIR variant requires compiled functions. Fixtures are
independently validated with wasm-tools 1.261.0.

Windows ASan passed the three focused tests in 0.40 seconds and all 146 tests in
445.73 seconds, including both Core 3.0 gates. The
[native CI run at `b13a687`](https://github.com/qigao/turbowasm/actions/runs/37637794241)
passed all five jobs:

- Linux MIR: 166/166 tests, 1.91 seconds.
- macOS arm64 MIR: 166/166 tests, 2.73 seconds.
- Windows qualification: 144/144 tests; installed package: 16/16.
- Linux qualification: 145/145 tests; installed package: 17/17.
- Android arm64: cross-build and installed consumer build; no device execution.

The existing `turbowasm_conformance_core3-mir` gate now recursively replays the
entire pinned suite, expanding its previous memory64-only scope. The
[first full differential run](https://github.com/qigao/turbowasm/actions/runs/37637802570)
passed in 18.05 seconds. Logging-only follow-up `f588db7` enables verbose CTest
output so successful conformance statistics remain in CI logs. Its
[detailed run](https://github.com/qigao/turbowasm/actions/runs/37638555437)
passed in 17.28 seconds:

```text
CORE_CONFORMANCE pass=63970 fail=0 unsupported=0 total=63970 files=258
TEXT_FRONTEND_CONFORMANCE pass=1229 fail=0 provider=wasm-tools (excluded from TurboWasm binary counts)
MIR_REPLAY compiled=6556 interpret_only=272 cold=2187 calls=6828 files=258
DIFFERENTIAL_REPLAY files=258 commands=63970 mismatches=0
```

The MIR counters describe function-state entries across retained instances, not
instruction coverage or a percentage of native execution. The 272 interpret-only
entries remain an admission-audit target; this replay qualifies mixed-tier parity,
not complete native Core 3.0. Native EH, resumable native execution, Component
encoding/post-return/async and broader WASI 0.2 coverage remain outstanding.

```powershell
cmake --build --preset win-core3-asan-user
ctest --preset win-core3-asan-user -R "gc_native|reference_native|vector_calls" --output-on-failure
```

With the pinned tool/spec environment on Linux, reproduce the full differential
gate using `cmake --preset ci-core3-mir-user`,
`cmake --build --preset ci-core3-mir-user` and
`ctest --preset ci-core3-mir-user --verbose -R "^turbowasm_conformance_core3-mir$"`.

## MIR typed exceptions and native admission audit

Implementation `4f01c65` lowers `try_table`, `throw` and `throw_ref` with static
lexical catch dispatch. Runtime retains tag identity, exception payload ownership
and cross-instance transfer. Tail calls exit their caller's protected region.
`eh_native_test.c` covers all four catch variants, nested/ordered catches,
rethrow, null/trap distinction, direct/indirect/reference/tail/mixed-tier calls,
imported tag identity, mixed scalar/vector/GC payloads, fuel and allocation failure.

Windows ASan passed 147/147 tests in 265.37 seconds, including both Core 3.0 gates.
The [native CI run](https://github.com/qigao/turbowasm/actions/runs/37641495192)
passed all five jobs. Linux MIR passed 168/168 tests in 1.15 seconds; macOS arm64
MIR passed 168/168 in 2.69 seconds. Android remains cross-build validation only.
The [full differential gate](https://github.com/qigao/turbowasm/actions/runs/37641504405)
reported:

```text
CORE_CONFORMANCE pass=63970 fail=0 unsupported=0 total=63970 files=258
TEXT_FRONTEND_CONFORMANCE pass=1229 fail=0 provider=wasm-tools (excluded from TurboWasm binary counts)
MIR_REPLAY compiled=6606 interpret_only=230 cold=2179 calls=6836 files=258
DIFFERENTIAL_REPLAY files=258 commands=63970 mismatches=0
```

All 230 interpret-only entries were imported functions (`imported=1 eligible=0`),
whose dispatch remains Runtime-owned; no observed defined-function entry was
interpret-only. These are function-state counts, not instruction coverage. EH
admission also lets previously cold callees enter tiering, so the totals cannot
be computed by simply subtracting the former 44 EH definitions from 272.
Resumable native execution, Component encoding/post-return/async and broader
WASI 0.2 coverage still require their separate qualification.

## Resumable native frames

Runtime implementation `a32c069` routes restartable invocations through the
existing tiered dispatcher. A separate private backend capability admits
suspension; ordinary execution-control support is insufficient. The Salts
coroutine retains generated frames and invocation-local reference/vector storage.
Store frame sources stay registered during yield. Cancellation unwinds through
the same callbacks and cleanup paths without replaying host effects.

`resumable_native_test.c` exercises direct, indirect, reference, tail, EH and
cross-instance calls; stack-only GC values and vector bits during host wait and
instruction-by-instruction fuel yields; interruption policy changes; cancellation
with allocation balance; terminal traps/exceptions; and allocation failure after
resume. Its MIR-only cases also pin a callee to the interpreter and check that a
backend without resumable opt-in keeps the reference path. MIR cases assert
compiled function states while suspended. Both fixtures are independently
validated with wasm-tools 1.261.0.

Windows ASan passed the eight focused tests in 1.07 seconds and all 148 tests in
225.77 seconds. The
[Core 3.0 differential run](https://github.com/qigao/turbowasm/actions/runs/37642754579)
at `a32c069` retained 63970 passes, zero failures/unsupported commands and zero
interpreter/MIR mismatches; the native-state counters remained
`compiled=6606 interpret_only=230 cold=2179 calls=6836`.

The first native build exposed an incorrect enum name in the new MIR-only test.
Test-only correction `4ecd9ea` uses the declared initial state. In the
[corrected native run](https://github.com/qigao/turbowasm/actions/runs/37643220455),
macOS arm64 MIR passed 170/170 tests in 2.74 seconds, including the new resumable
MIR target. Linux MIR qualification is still pending in that run.

```powershell
cmake --build --preset win-core3-asan-user
ctest --preset win-core3-asan-user -R "resumable|host_wait|eh_native|gc_native|jit_backend_contract|jit_tiering|shared_memory_path" --output-on-failure
```

Linux/macOS use the complete `ci-mir-user` / `ci-macos-mir-user` profiles; the
targeted native filter is `mir_resumable`. These results do not qualify Component
encoding/post-return/async or the broader WASI 0.2 surface.

## Canonical string encoding qualification

Implementation `7d31a9c` adds UTF-16LE and tagged Latin-1/UTF-16, preserving
canonical guest realloc sequences and normalizing private host strings to UTF-8.
`component_encoding_test.c` covers all encoding pairs, Unicode boundaries,
moving/failing reallocations, malformed input, memory32/memory64 lengths,
nested host values, separate Component instances connected through an import
provider, and retained resumable calls. Both fixtures passed wasm-tools 1.261.0.

Windows ASan passed 44/44 Component/WASI 0.2 tests and the full 149/149 tests in
191.70 seconds. In the [native run](https://github.com/qigao/turbowasm/actions/runs/37646837512),
Linux MIR passed 171/171 in 2.34 seconds and macOS arm64 MIR passed 171/171 in
3.95 seconds. Both include `turbowasm_mir_resumable_test`, completing the Linux
resumable qualification left pending in the preceding run. Windows CI and the
Android cross-build also passed; the ordinary Linux job was still preparing
dependencies when these results were recorded.

This qualifies canonical string conversion, not post-return, local-function
canon-lower binding, complete Component async/future/stream or broader WASI 0.2.

## Canonical post-return execution

`component_post_return_test.c` covers Core-instance cleanup callbacks after
successful lifting, exact scalar arguments, empty results, memory32/memory64
indirect result copies, cross-Core-instance callbacks, failed lifting, cleanup
traps and uncaught exceptions, allocation failures, owned results, retained
public owners, fuel yields and cancellation. A separate import-provider fixture
checks that canon lower traps before leaving the Component during cleanup.
All four WAT fixtures passed wasm-tools 1.261.0 validation. Malformed-option
cases check duplicates, absent/out-of-range indices and post-return on lower.

Implementation `6d108a7` passed all 48 selected Windows ASan
Component/WASI 0.2/resumable/host-wait tests in 2.78 seconds and the complete
150/150 tests in 218.86 seconds, using the VS developer environment required
for the ASan runtime DLLs. The MIR variant additionally asserts that the producer and
cleanup function are compiled while the cleanup frame is suspended. The
[native run](https://github.com/qigao/turbowasm/actions/runs/37650266391) passed all
five jobs: Linux MIR 173/173 in 2.00 seconds, macOS arm64 MIR 173/173 in 2.02
seconds, Windows, ordinary Linux and the Android cross-build. Direct canonical
cleanup targets were added after this qualification, as described below.

Direct canon lower and resource.drop cleanup targets now pass signature admission
and trap on the canonical leave gate before provider or handle side effects.
Core cleanup also cannot call resource.new/drop, while resource.rep is permitted.
Guest realloc shares this gate. The expanded post-return test exercises both
synchronous and resumable direct targets, new/drop preservation of resource-table
ownership, legal representation reads, invalid new/rep cleanup signatures, and
provider exclusion during realloc. Both modified fixtures pass wasm-tools
validation. The MIR test variant attaches backends to the resource fixture too.
The expanded Windows ASan selection passed 48/48 in 2.05 seconds.
At `5034ef6`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37652723361)
passed all five jobs, including Linux MIR 173/173 in 2.07 seconds and macOS
arm64 MIR 173/173 in 2.02 seconds. Android qualification is a cross-build.

```powershell
cmake --build --preset win-core3-asan-user
ctest --preset win-core3-asan-user -R "component|wasi02|resumable|host_wait" --output-on-failure
```

## Resource destructor execution control

`component_resource_reentry_test.c` exercises guest resource.drop calling a
Core destructor under the original execution's control. Its seven cases cover
fuel suspension, cancellation without handle restoration or destructor replay,
trap and uncaught-exception propagation, cumulative caller/destructor depth,
interleaved suspended calls, interruption, and nested host-wait completion and
cancellation. The MIR variant additionally checks that the suspended destructor
has reached compiled state. The WAT fixture passes wasm-tools 1.261.0 validation.

Windows ASan passed the related 49/49 tests in 2.71 seconds and the complete
151/151 suite in 245.59 seconds. The subsequently added host-wait case passed
with all seven re-entry cases (3,398 assertions) in the same preset; production
code was unchanged after the full run. Native MIR qualification passed at
`13ac5e3`, as recorded below.
This closes destructor budget/depth reset, not local-function canonical lowering
or the remaining Component async and WASI 0.2 surface.

## Guest realloc execution control

`component_realloc_execution_test.c` covers memory32 and memory64 realloc under
canonical lowering in five cases: fuel suspension without provider replay,
cancellation and private-result cleanup, trap/exception propagation, cumulative
caller/realloc depth, and interruption. The native variant checks the suspended
realloc function's compiled state. Both WAT fixtures pass wasm-tools 1.261.0
validation. Windows ASan passed the focused test and the related 50/50 tests in
2.48 seconds. Runtime code is unchanged from the preceding full qualification;
the changes are private Component call contexts and error propagation. Native
MIR qualification passed at `13ac5e3`, after branch pushes recovered from the
GitHub server error. Local-function canonical lowering follows below.

## Local canonical lowering

Local canon lower now reuses lift adapters, including local export/instance
aliases and calls from a Core start function. Invocation-local copies retain
the original execution control for the body, realloc and post-return. Own
arguments transfer at callee admission; borrow loans last through unwind. Owned
result handles remain staged until every result field has lowered successfully.

`component_local_lower_test.c` has eleven cases covering start/alias binding,
memory64 UTF-8 to memory32 UTF-16 conversion, local own/borrow movement, nested
post-return fuel suspension/cancellation, resource loans across suspension,
string allocation failures, owned-result cleanup failure, borrow cancellation,
composite-result rollback and early/lazy initialization allocation failures.
Its WAT fixture passes wasm-tools 1.261.0 validation. Windows ASan passed the
related 51/51 tests in 2.59 seconds; the final initialization-failure extension
passed with all eleven cases and 48,391 assertions. Native variants check that
the suspended local body and post-return functions are compiled.
At `13ac5e3`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37657819525)
passed all five jobs: Linux MIR 179/179 in 2.15 seconds, macOS arm64 MIR 179/179
in 2.51 seconds, Windows, ordinary Linux and the Android cross-build. This also
qualifies the preceding destructor and realloc control increments. The
[Core 3.0 MIR differential run](https://github.com/qigao/turbowasm/actions/runs/37657827907)
retained 63,970 passes, zero failures/unsupported commands, and zero differences
across 258 files; MIR counters were compiled=6606, interpret_only=230, cold=2179,
calls=6836. Indirect parameters follow below; complete async/future/stream and
broader WASI 0.2 remain separate work.

## Indirect canonical parameter tuples

Canonical lower now accepts parameter tuples beyond the flat ABI limit. Lift
and lower share complete tuple layout, alignment and bounds validation, including
memory64 arithmetic. Larger logical argument lists use checked fixed-size
storage retained until callback return or cancellation. The output pointer is
separate from the input tuple pointer.

`component_indirect_lower_test.c` exercises 17 mixed-width parameters, a single
large composite parameter, local and host providers, memory32/memory64, tuples
ending exactly at the memory boundary, invalid alignment/bounds/overflow,
indirect results, fuel suspension/cancellation and allocation failures. The
local-lower fixture additionally checks that whole-tuple bounds failure leaves
an input own untouched, while a later invalid character releases an already
lifted own exactly once. All three WAT fixtures pass wasm-tools 1.261.0
validation. The final Windows ASan Component/WASI 0.2/resumable/host-wait
selection passed 52/52 in 2.03 seconds. At `d148ae5`, the
[native run](https://github.com/qigao/turbowasm/actions/runs/37659894577)
passed all five jobs, including Linux MIR 181/181 in 2.40 seconds and macOS arm64
MIR 181/181 in 2.14 seconds. Windows, ordinary Linux and the Android cross-build
also passed. This qualifies indirect parameters; it does not qualify async
tasks, future/stream execution or the remaining WASI interfaces.

## Private async type and notification primitives

Future/stream metadata now retains optional payloads in the private type graph.
Validation rejects non-value payloads, transitive borrows, cycles, excessive
nesting and the pinned proposal's immediate `stream<char>` restriction. This
does not enable these types in the binary loader or synchronous host admission.

The private async notification primitive separates progress publication from
event delivery. Subtask notifications coalesce into the latest state; terminal
delivery is distinguished from merely starting or resolving. Endpoint state
preserves partial stream progress, deferred host cancellation acknowledgement,
and the different future/stream precedence when completion, cancellation and
peer closure race. This primitive allocates nothing and owns no data buffers,
resources or execution frames. The enclosing owner must eventually perform
loan/buffer release and guest resumption; that integration is not yet present.

`component_async_type_test.c` and `component_async_state_test.c` cover primitive,
composite and nested endpoint payloads, invalid references and ownership, depth
boundaries, notification coalescing, all six orderings of completion/cancel/close,
progress limits and exactly-once event delivery. Windows ASan passed the three
type/state targets in 0.10 seconds, followed by the related
Component/WASI 0.2/resumable/host-wait selection: 54/54 in 4.38 seconds.
Task scheduling, waitable sets, canonical async decoding, actual endpoint copies
and public host integration remain unqualified and under implementation.
At `5cc254c`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37662445331)
passed all five jobs: Linux MIR 183/183 in 2.34 seconds, macOS arm64 MIR 183/183
in 2.53 seconds, Windows, ordinary Linux and the Android cross-build. These
results cover the private type/notification primitives and existing behavior.

## Shared async handles and private waitable sets

Resource, waitable and waitable-set slots now share one bounded canonical handle
table. Explicit kinds prevent cross-kind access; generation retirement prevents
stale handles from becoming valid again when a slot changes kind. Async objects
remain caller-owned at stable addresses, independent of table reallocations.

The private waitable layer supports set membership transfer, fair event polling,
active-wait pins and exclusive individual waits. Empty polling yields the NONE
tuple. Nonempty or pinned sets cannot drop, and individual waits prevent event
theft or membership changes. Terminal subtask delivery releases actual resource
loans before publishing the event, with a guarded callback that may grow the
table. Callback failure consumes the notification without replaying cleanup.

`component_waitable_test.c` checks shared quotas, all five waitable event kinds,
membership rollback, stale and wrong-kind handles, cancellation pins, pending
copy retention, round-robin delivery, allocation failure, generation exhaustion
and terminal release/reentry. Its allocator forces table growth to relocate
storage and tracks live allocations. Windows ASan passed four focused targets
in 0.18 seconds and the related 55/55 tests in 4.36 seconds. A final focused run
also covers invalid event outputs and retaining an endpoint through deferred
cancellation. Canonical guest bindings, scheduling, endpoint data transfers and
public async host APIs still need implementation and end-to-end qualification.
At `aec8ef1`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37664259457)
passed all five jobs: Linux MIR 184/184 in 2.11 seconds, macOS arm64 MIR 184/184
in 2.38 seconds, Windows, ordinary Linux and the Android cross-build.

## Private endpoint rendezvous and readable ownership

Paired future/stream endpoints now move actual canonical host values, including
composite allocations and owned resource cleanup obligations. Source cells are
cleared only when moved; partial transfer, cancellation and peer closure retain
untransferred values. Host arrays remain exclusively borrowed until notification
delivery, including waitable-set polling and exclusive waits. Unit transfers
use logical counts without dummy allocations. Readable ends can detach to host
ownership and attach to another canonical table while preserving peer links and
idle close notifications; failed destination admission preserves host ownership.

`component_endpoint_test.c` covers both arrival orders, partial/coalesced progress,
zero-length readiness, real composite/resource movement, cancellation and close
precedence, same-instance type restrictions, buffer admission and overlap,
28-bit limits, unit futures, registration rollback, ownership transfer guards,
stale handles and quota/allocation failures. The custom allocator tracks all live
allocations. Windows ASan passed four focused targets in 0.14 seconds and the
related Component/WASI 0.2/resumable/host-wait selection: 56/56 in 2.83 seconds.
Two additional buffer/await cases then passed with the full endpoint target:
18 cases, 387 assertions in 0.04 seconds, with production code unchanged.
These results qualify host-array rendezvous and private owner transitions only.
Guest memory codecs, nested endpoint values, canonical async bindings, task
scheduling and public async host integration remain under implementation.
At `59925b8`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37666577052)
passed all five jobs: Linux MIR 185/185 in 2.16 seconds, macOS arm64 MIR 185/185
in 2.93 seconds, Windows, ordinary Linux and the Android cross-build.

## Nested private endpoint values

Canonical host values now retain a unique readable future/stream owner through
their existing move/release protocol. Entering a value freezes direct endpoint
operations; extraction clears the carrier and restores direct access. Value
destruction closes the endpoint once, including endpoints nested in composites.
Cross-graph value-type comparison uses structural fields and ordered labels;
resource leaves use nominal identity. This comparison allocates nothing and
shares the existing value-depth limit. It does not make endpoints admissible
through the synchronous public host boundary or canonical guest memory codecs.

The type tests cover inline/indexed representations, independently indexed
graphs, all composite wrappers, labels, endpoint kind/unit distinctions, nominal
resources and depth/cycle rejection. The endpoint tests transfer actual
`list<record<future<u32>>>` and `list<record<stream<u32>>>` values across distinct
type graphs. They verify payload mismatch before ownership transfer, frozen
direct access, pending peer-close preservation, extraction followed by actual
data transfer, and nested destruction notifying a pending writer without losing
its untransferred value. Windows ASan passed five focused targets in 0.18 seconds
and the related 56/56 tests in 2.43 seconds; a final focused run also covers the
nested stream variant. Guest codecs, task scheduling, canonical async bindings
and public host lifetime/transfer APIs still need implementation and qualification.
At `23444a8`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37667715152)
passed Windows, ordinary Linux, Linux MIR (185/185 in 1.21 seconds) and macOS
arm64 MIR (185/185 in 3.00 seconds). Android cross-build was still running when
these results were recorded.

## Private canonical endpoint handle codecs

Private canonical conversion now represents future/stream values as four-byte
handles under memory32 and memory64 and one i32 flat carrier. Dedicated endpoint
callbacks keep their ownership separate from resource callbacks; absent endpoint
callbacks still reject conversion. Lift validates the readable handle, payload
type and idle/unjoined state before allocating its host owner and consuming the
handle. Lower reserves handles in a caller-owned scope and publishes them only
at explicit commit; rollback preserves input values. Reserved handles remain
inaccessible during guest realloc reentry. Each lifted value has a separate
Runtime-allocated owner record, so destroying an obsolete value after commit
cannot close a newly lifted owner of the same endpoint.

`component_endpoint_codec_test.c` exercises flat and guest-memory conversion,
records and lists, exact memory-end bounds, memory32/64 overflow and alignment,
type/kind/membership/ownership admission, duplicate lowering, allocator and quota
failure, partial lower rollback, partial lift cleanup, and peer-close delivery
across commit. Its realloc callback forces handle-table growth and grows Wasm
memory while confirming that a reserved endpoint cannot be lifted. All tracked
Runtime allocations are released. Windows ASan passed five focused targets in
0.14 seconds and the related 57/57 tests in 2.38 seconds. Two final admission/event
cases also passed with the complete codec target: 14 cases, 630 assertions in
0.04 seconds, with production code unchanged after the broader regression.

These are private endpoint-value codecs, not guest stream/future read/write
buffer execution. Guest buffer rendezvous, canonical async instruction bindings,
task scheduling and public host lifetime/transfer APIs remain under implementation.
At `325e6dc`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37669417367)
passed all five jobs, including Windows, ordinary Linux, Android cross-build,
Linux MIR (186/186 in 1.21 seconds) and macOS arm64 MIR (186/186 in 1.95 seconds).

## Private guest and host transfer buffers

Endpoint rendezvous now copies between host value arrays and checked guest
regions under memory32 and memory64. Guest reads lift the whole batch before
any destination write, including overlapping numeric regions. Guest writes
reuse canonical encoding and explicit ownership commit/rollback hooks. Conversion
callbacks guard both endpoints against reentrant mutation, and no guest memory
pointer survives realloc or memory growth. Copy failure terminates both operations;
error delivery releases their buffer borrows without publishing a successful event.

`component_buffer_test.c` covers all four host/guest source/destination combinations,
both arrival orders and memory widths, overlapping regions, range admission,
unit/zero-length copies, coalesced progress, cancellation before destructive lift,
UTF-8/UTF-16 conversion across realloc and growth, snapshot allocation failure,
nested endpoint batch commit/rollback, and actual owned resource transfer. It also
checks failed destination lowering and failed commit after destructive guest reads:
source handles stay consumed, destination reservations are removed, and temporary
owners close exactly once. The async type tests distinguish an endpoint handle's
codec requirements from those of its future payload. Fixtures track all Runtime
allocations through cleanup.

Windows ASan passed the related 58/58 targets in 2.96 seconds. After adding the
destructive-read failure and type-feature cases, all seven focused targets passed
in 0.28 seconds, with production behavior unchanged since the broader regression.
The buffers and callbacks remain private borrowed objects. Canonical async guest
bindings, task scheduling, and retained public host lifetime/transfer APIs still
need implementation and end-to-end qualification; these tests do not qualify that
complete surface.

At `c09b471`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37672058269)
passed Windows, Android cross-build, Linux MIR (187/187 in 1.19 seconds), and
macOS arm64 MIR (187/187 in 2.93 seconds). Ordinary Linux was still running when
these results were recorded.

## Private endpoint forwarding

Forwarding consumes matching idle, unjoined readable/writable intermediates and
connects their surviving peers. Existing pending buffers immediately rendezvous,
preserving previously accumulated progress and retaining any larger remainder.
Zero-length readiness, recursive forwarding, and closure propagation follow the
pinned Canonical ABI. Host-only forwarding adds no allocation. Guest conversion
uses the same checked buffers and callback guards as ordinary copies. Admission
failure preserves both intermediates; conversion failure after linking returns
the trap and leaves both survivor borrows pending until error delivery.

The endpoint suite covers the numeric buffer-size matrix, prior progress followed
by further copies, idle/pending and unit/typed futures, recursive and absent-peer
cases, handle removal, wrong direction, membership/waiter/reentry admission,
independently indexed types, synchronous survivor waits, the same-component
composite restriction, and explicitly moved composite resource ownership. The
guest-buffer suite also covers successful forwarding and allocation failure after
link publication under memory32/64. All tracked allocations are released.
Windows ASan passed 58/58 related targets in 2.86 seconds. A final ownership case
then passed with all three focused targets in 0.10 seconds: endpoint 28 cases /
1155 assertions, codec 14 / 630, and buffer 13 / 948. Production code was unchanged
after the broader regression. Canonical guest bindings, scheduling, and public
host task/lifetime APIs remain unqualified and are not enabled by this primitive.
At `e9afd8f`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37672985127)
passed Windows, Android cross-build, Linux MIR (187/187 in 1.22 seconds), and
macOS arm64 MIR (187/187 in 3.09 seconds). Ordinary Linux was still running when
these results were recorded.

## Private async canonical signatures

Canonical signature calculation now supports synchronous, stackful async and
callback async modes. Async lowers use the four-carrier direct parameter limit,
an address for payload results, and an i32 subtask return word. Async lifts retain
the sixteen-carrier parameter limit and return either no Core values or one i32
callback word. Separate task.return signatures carry up to sixteen payload
carriers directly and use one address above that limit. Only addresses change
width under memory64. Existing synchronous adapters use the same implementation
in synchronous mode, and signature outputs remain unchanged on failure.

The async type suite covers both memory widths, mixed integer/float carriers,
4/5 and 16/17 parameter boundaries, unit/scalar/string results, callback and
stackful lifts, task.return tuples through twenty carriers, nested endpoint
results, invalid modes and malformed late references. Windows ASan passed five
focused targets in 0.25 seconds and the related 58/58 targets in 2.72 seconds.
These tests qualify private signature calculation and synchronous regression;
they do not qualify async function decoding, canonical option validation, task
scheduling, or public async execution.
At `abc3c69`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37673794023)
passed Windows, ordinary Linux, Linux MIR (187/187 in 2.57 seconds), and macOS
arm64 MIR (187/187 in 2.71 seconds). Android cross-build was still running when
these results were recorded.

## Private async type and option decoding

An internal metadata-only entry point now decodes async function types,
future/stream payloads, and async/callback lift/lower options through the existing
binary parser and allocator. Instance-local aliases preserve function async flags
and endpoint references. Async lowering resolves callee types through direct
imports, lifts, local inline instances, export aliases, and imported interfaces.
The decoder checks duplicate/conflicting options, index bounds, async function
typing, and required memory for async lowers. Graph validation rejects invalid
endpoint payloads, including transitive borrows and immediate stream<char>.

Ordinary/public loading still rejects async forms. Exec rejects metadata-only
objects before allocation or Core start execution. These objects borrow their
input bytes and own their decoded arrays; every failed decode frees its arrays
and resets the output. The private path does not validate actual Core callback
signatures or admit async execution yet. Canonical builtin decoding is covered
by the following increment.

`component_async_binary_test.c` covers the admission gates, nested type aliases,
memory32/64 options, callee resolution paths, malformed/truncated input, and
allocation failure at every allocation until successful decoding. Windows ASan
passed five focused targets in 0.16 seconds and the related 59/59 targets in
2.61 seconds. A final transitive-borrow rejection case also passed with the whole
async binary target; production code was unchanged after the broader regression.
At `2a16bd3`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37675079125)
passed all five jobs: Windows, Android cross-build, ordinary Linux, Linux MIR
(188/188 in 2.26 seconds), and macOS arm64 MIR (188/188 in 3.01 seconds).

## Private async canonical builtin decoding

The metadata decoder now retains thirty async builtin forms: task return/cancel,
subtask cancel/drop, context get/set, thread yield, backpressure inc/dec,
waitable-set operations and join, and stream/future new/read/write/cancel/drop/
forward. Each definition consumes one Core function index and retains its type,
options and immediates. Unknown explicit-thread and error-context proposal
builtins still return unsupported; ordinary loading and execution remain gated.

Binary tests check every supported signature in memory32 and memory64, packed
i64 endpoint pairs, memory64 stream results, fixed i32 future/control results,
context width consistency, typed payload memory, destination realloc for dynamic
payloads, nested endpoint handle opacity, unit copies without memory, and
task.return direct/indirect parameters. Invalid kinds/indices, duplicate or
conflicting options, reserved immediates, truncation at every immediate byte,
and allocation failures after multiple retained entries exercise cleanup.
These tests qualify metadata and signatures only; task scheduling, guest builtin
execution, cancellation acknowledgement and public endpoint lifetime integration
remain incomplete.

Windows ASan passed five focused targets in 0.16 seconds. After the final context
width consistency test and decoder adjustment, the related Component/WASI/resume
regression passed 59/59 targets in 2.55 seconds.
At `3eef5b3`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37676566968)
passed all five jobs, including Linux MIR (188/188 in 1.92 seconds) and macOS
arm64 MIR (188/188 in 3.00 seconds).

## Private async Core task execution

The private task runner now drives real resumable Core entry and callback calls.
It checks their exact signatures, defers argument preparation behind backpressure,
bounds live tasks, distinguishes task resolution from Core exit, pins callback
waitable sets, and retains callback exclusivity across Core host/fuel suspension.
Stackful tasks do not require that exclusive slot. Callback cancellation precedes
ready events and is delivered once; guest task.cancel requires delivery and no
outstanding borrowed handles. A result may still win a pending cancellation.

task.return matches the active lift's result type and memory/encoding before
lifting values through existing canonical codecs. Results move to the task and
then to its caller, or are destroyed on teardown. Destroy unwinds any retained
Core host callback before releasing waits/results/domain admission. Uncaught
Core exceptions become canonical traps. Argument ownership and borrow cleanup
remain with the preparing canonical adapter; the runner does not invent another
resource table or take ownership of borrowed graphs/instances.

`component_task_test.c` runs the committed Wasm fixture through actual imports,
covering callback events, cancellation acknowledgement and return races,
backpressure/quota, early results, retained host waits, repeated fuel suspension,
memory32/64 string/indirect-tuple/unit returns, invalid options/loans/signatures,
Core exception isolation, and allocation failure cleanup. The interpreter suite
passed 17 cases with 865 assertions on Windows ASan; related regression passed
60/60 targets in 2.48 seconds. A separate MIR target uses the same suite with
compiled-execution assertions; Linux/macOS results are recorded below.

At `9e6dd9f`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37680478846)
passed all five jobs. Linux MIR passed 190/190 in 1.73 seconds; macOS arm64 MIR
passed 190/190 in 3.60 seconds. Both executed the new MIR Component task target,
including its assertions that key entry/callback/wait/fuel paths were compiled.

This qualifies an internal execution owner, not public async Component support.
Automatic binding of decoded builtins/lifts, canonical argument ownership,
instance-retaining public task/endpoint/transfer APIs, and
the remaining Component/WASI integration are still incomplete.

## Private task and waitable builtin bindings

The initial twelve builtin families bind decoded descriptors to exact Core host
signatures: task return/cancel, context get/set, backpressure inc/dec, thread
yield, waitable-set new/drop/wait/poll, and waitable join. A committed Component
fixture provides the decoded descriptors; a real Core Wasm fixture imports and
executes the resulting bindings. Subtask cancel/drop support is recorded below;
endpoint families remain rejected, and public async admission remains disabled.

The task driver resumes builtin YIELD on its next quantum and SET waits only
when a non-consuming readiness check finds a pending event. The retained host
callback consumes that event and releases its set pin. Unwind releases the same
pin without replaying the import. Guest-created set storage belongs to the task
domain, shares canonical table quotas, and is freed after successful unregister;
domain teardown rejects nonempty/pinned sets. Event payload stores follow the
canonical order, including partial memory output if the second u32 store traps.

task.return now compares resolved mutable memory identity through imported
aliases. Comparing Core instance pointers and local indices incorrectly rejected
two aliases of the same memory; the regression uses two import layers, reversed
memory indices, both address widths, and a distinct-memory rejection case.

Windows ASan passed the three focused targets in 0.19 seconds and the related
Component/WASI/resume/host-wait/memory/link regression, 76/76 in 3.48 seconds. The
task suite passed 25 cases and 2490 assertions, including real builtin suspension,
context isolation, memory32/64 event output, may_leave checks, cancellation,
owned-set allocation failure and cleanup, and invalid/unsupported binding
admission. At `11652f1`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37683175897)
passed all five jobs, including Linux MIR 190/190 in 2.28 seconds and macOS arm64 MIR 190/190 in
2.17 seconds. Both executed the MIR task suite in 0.02 seconds, including
compiled builtin suspension/event-output paths. Component instantiation still needs
to construct these bindings automatically alongside async lift/lower adapters,
subtasks, endpoint ownership and the public retained-instance API.

## Private subtask propagation and cancellation

The caller-side subtask now connects real callee task preparation, task.return
and cancellation acknowledgement to the shared waitable table. The result adapter
must finish lowering/moving the result before RETURNED is committed. Actual
resource lend counts stay held until terminal delivery; STARTED alone releases
no loan. Eager results return the canonical raw RETURNED word without allocating
a handle. Deferred calls publish the current phase and a generation-checked
handle, suppressing a duplicate STARTED and coalescing later progress.

Decoded synchronous/async subtask.cancel and subtask.drop run as Core imports.
Cancellation owns an exclusive waitable pin before entering the callee callback;
the synchronous form keeps it across a real host-wait suspension, while the
async form returns BLOCKED if unresolved. A result may win cancellation. Callback
resolution detaches the caller context, allowing the subtask to be destroyed
while callee Core cleanup remains suspended. Failure first unwinds retained Core
frames, then detaches with a sticky error. Waiters observe the failure and loan
cleanup occurs once during owner teardown, without a fabricated terminal event.

Eager cancellation into an available callback in another scheduling domain shares
the initiating caller's remaining fuel and interruption check. The child retains
its own continuation if it suspends; the caller is charged consumed fuel before
its Core frame resumes. The suite checks both exhaustion and interruption, as
well as an immediate acknowledgement with unlimited execution.

The committed task fixtures cover both memory widths for caller result stores,
real lend/drop exclusion, pre-start cancellation, deferred acknowledgement,
reentrant event theft prevention, cancellation unwind, stale handles, eager and
deferred cleanup failures, argument/result failures, and handle allocation OOM.
The same tests are compiled into the existing interpreter and MIR task targets.
Windows ASan passed 43 task cases with 4989 assertions. The related
Component/WASI/resumable/host-wait/memory/link/fuel/interrupt selection passed
76/76 targets in 3.02 seconds. At `d47fdd3`, the
[native run](https://github.com/qigao/turbowasm/actions/runs/37687006122) passed all
five jobs: Linux MIR passed 190/190 in 1.43 seconds and macOS arm64 MIR passed
190/190 in 2.88 seconds. Their MIR task targets passed in 0.02 and 0.03 seconds,
respectively, including compiled cancellation and inherited-budget assertions.
This remains private integration: automatic canonical argument/result adapters,
endpoint builtin bindings, retained public async owners and end-to-end Component
instantiation are still required before opening public async admission.

## Retained async argument preparation

Task argument preparation now executes once on the same retained coroutine as
the Core entry. Runtime allocates the typed argument storage first, then the
private preparation hook validates and copies converted carriers only on success.
This makes guest realloc suspension part of the task execution: fuel exhaustion,
interruption and host waits retain the conversion frame instead of replaying it.
Destroy unwinds that frame before caller abandonment, so temporary strings and
borrow scopes remain alive until their actual users have stopped. Core allocation
failure occurs before caller arguments are consumed. Callback exclusivity remains
held throughout preparation; no callback turn repeats the preparation hook.

The task fixture lowers a real canonical string through memory32/64 guest realloc
functions and verifies the prepared guest entry reads the converted bytes. Tests
cover host waits, cancellation while converting, fuel/interruption resumes,
callback exclusivity, malformed prepared carriers, realloc traps/exceptions,
destruction while preparing, and an allocation-failure sweep. MIR runs assert
that the realloc functions and prepared entries actually compiled.

Windows ASan passed 50 task cases with 5988 assertions. The related
Component/WASI/resumable/host-wait/memory/link/fuel/interrupt regression passed
76/76 targets in 3.04 seconds. At `fc648c5`, the
[native run](https://github.com/qigao/turbowasm/actions/runs/37688569975) passed all
five jobs. Linux MIR passed 190/190 in 2.21 seconds and macOS arm64 MIR passed
190/190 in 2.34 seconds, including the compiled task preparation tests.
This fixes the execution boundary required by async canonical argument
adapters; automatic binary binding, endpoint builtins and retained public async
owners remain incomplete, and public async admission stays closed.

## Async canonical call conversion

The private per-invocation call frame now connects canonical lower arguments to
the retained callee task and lowers task.return into caller memory. It copies
raw carriers on admission and reads guest parameter memory only after
backpressure clears. Caller and callee flattening limits independently select
direct or indirect parameter tuples. Result storage finishes before terminal
publication; lifted argument owners and lender releases remain retained until
delivery or failure teardown. Resource and endpoint codecs use explicit atomic
commit/rollback hooks for each direction.

The committed Core fixture exercises ordinary host imports and nested retained
tasks, memory32/64 in both directions, four/five/seventeen parameter boundaries,
UTF-16 conversion, unit results, pre-start cancellation, deferred results, and
callback continuation after eager return. Actual resource tables verify own
round trips, borrow exclusion until event delivery, and failed commits. A future
endpoint round trip preserves its peer and failed commit closes the consumed
source. Guest realloc waits in argument and result conversion retain their
stacks; both completion and destruction paths release values in order. The suite
also checks malformed admission, result bounds, inherited fuel and an allocation
failure sweep. MIR targets use the same suite with compiled-entry assertions.

Windows ASan passed 22 call cases with 1903 assertions and the existing 50 task
cases with 5988 assertions. The related Component/WASI/resumable/host-wait/memory/
link/fuel/interrupt regression passed 77/77 targets in 3.18 seconds. At `5ee8469`,
the [native run](https://github.com/qigao/turbowasm/actions/runs/37691360677) passed
all five native matrix jobs. macOS passed 192/192 in 3.28 seconds and Linux MIR
passed 192/192 in 2.18 seconds, both including the compiled async call target in
0.02 seconds. This is still a private invocation
boundary: automatic binary binding, per-instance call ownership, endpoint builtin
dispatch and retained public async handles remain incomplete. Public async
admission remains closed until those integration requirements are verified.

## Endpoint Core builtin binding

The private task binding now dispatches stream/future new, read/write,
cancel-read/write, drop-readable/writable and forward through the existing typed
endpoint engine. New pairs have domain-owned stable storage bounded independently
of local handle occupancy, so moving readable ends cannot bypass the capacity
limit. Domain teardown rejects live transferred ends. Fully closed pairs are
collected without invalidating their surviving peers.

Each endpoint retains one guest buffer descriptor through event/error delivery.
Synchronous copy pins before conversion callbacks, suspends on a retained Core
host wait, and resumes only when its own event is ready. Destruction cancels the
local copy and delivers its terminal event before releasing the memory borrow;
it rejects destruction while a peer is converting that buffer. Async operations
return BLOCKED with the descriptor retained independently of the initiating task.
Resource and endpoint payloads use the shared buffer codec transaction hooks.

The Core fixture covers packed handles, both memory widths, sync and async
stream/future copies, cancellation, future completion/drop rules, forwarding,
waitable-set delivery, payload/kind/range admission, forged waitables, domain
capacity after handle transfer, allocator failures and half-pair rollback. Host
string rendezvous checks retained realloc options, copy failures and reentrant
event/destruction exclusion. Unit payloads ignore pointer values as specified.
Windows ASan passed 16 builtin cases with 2977 assertions; the related
Component/WASI/resumable/host-wait/memory/link/fuel/interrupt regression passed
78/78 targets in 3.35 seconds. At `7edcf37`, the
[native run](https://github.com/qigao/turbowasm/actions/runs/37693201902) passed all
five matrix jobs. Linux MIR passed 194/194 in 1.19 seconds and macOS arm64 MIR
passed 194/194 in 3.00 seconds, including the compiled endpoint builtin suite.

These bindings remain private. Automatic Component binary instantiation, public
async task/endpoint owners and per-instance retained callback contexts are still
required before enabling public async admission. The broader Component/WASI
completion goal is not established by this primitive-level qualification.

## Private async Component instantiation

The internal async exec entry reuses the normal module/instance loader, alias
maps and canonical memory/realloc resolution. Stable task/endpoint builtin host
bindings are linked from actual Component binary definitions; async lifts resolve
to validated stackful/callback task bindings. Explicit task and canonical handle
limits bound the domain. Synchronous invocation rejects async exports, and
destruction rejects registered tasks, live async handles and transferred ends
whose pair storage still belongs to the exec.

Guest realloc inherits the running task's retained Core execution control and
uses fresh context slots. Auxiliary ownership excludes other tasks in the domain
through fuel/interruption suspension, then restores context and may_leave on
success, trap or unwind. Non-numeric stream copies within the same Component
retain their canonical trap; host string delivery exercises resolved builtin
realloc and forbids destroying the waiting reader during conversion.

`component_async_exec_test` uses a generated Component fixture containing Core
imports, canonical builtins and async exports. Windows ASan passed 16 cases with
1837 assertions, covering scalar/callback/yield/cancellation, memory32/memory64
strings and mixed-width stream copies, auxiliary suspension/unwind, task/handle
capacity, moved endpoint lifetime, malformed callback/import signatures, missing
realloc options and every constructor allocation failure. Related Component/WASI/resumable/host-wait/
memory/link/fuel/interrupt regression passed 79/79 targets in 3.49 seconds.
The MIR target runs this same suite with compiled-function assertions. At
`249fbc0`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37696876045)
passed all five matrix jobs. Linux MIR passed 196/196 in 1.94 seconds and macOS
arm64 MIR passed 196/196 in 2.86 seconds, including the compiled async exec target
in 0.02 seconds on each platform.

This is an internal integration stage. Retained public async owners and nested
Component instantiation remain incomplete. Local resource/endpoint call
transactions are covered below. Public async admission stays closed.

## Automatic local async lowering

The shared inline-provider linker now binds async canon lower to validated local
async lifts. An exec-owned FIFO retains each async-call frame through Core exit,
terminal delivery and guest subtask drop. Eager completed children retire without
callbacks while their parent continues, so repeated sequential calls reuse the
bounded task capacity. Polling removes the running frame from the ready list
before guest callbacks can create/collect nested calls. Each poll has a turn limit
and an explicit per-turn fuel/interruption budget.

Published failures remain observable to waiters. Unpublished failures unwind and
release their arguments before collection. Private failure teardown requires
exported caller tasks to be destroyed first; it is separate from cooperative guest
subtask cancellation. Async unit calls without arguments/results no longer require
a memory option. Memory remains mandatory for indirect arguments, dynamic values
and canonical result destinations.

`component_async_lower_test` runs actual Component binaries with local async
lower imports, memory32-to-memory64 string parameters/results, deferred callbacks,
early task.return, nested calls during polling, guest cancellation, capacity
failures, published/unpublished traps, suspended realloc teardown and allocation
failure sweeps. Windows ASan passed 14 cases with 713 assertions; related
Component/WASI/resumable/host-wait/memory/link/fuel/interrupt regression passed
80/80 targets in 3.79 seconds. For `bf22bae`, the
[native run](https://github.com/qigao/turbowasm/actions/runs/37698899854)
passed all five jobs. Linux MIR passed 198/198 targets in 2.41 seconds and macOS
MIR passed 198/198 in 2.82 seconds, including the compiled async-lower suite
(0.04 and 0.03 seconds respectively).

Local async calls now also move stream/future readable endpoints, including
endpoints nested in composite values. Each invocation owns separate parameter
and result reservation scopes; task.return uses an instance-owned lift-only codec.
Partial conversion failure rolls back unpublished destination handles, and consumed
source owners close exactly once. Suspending in a later string realloc retains
the earlier endpoint reservation until commit or execution unwind.

The expanded Windows ASan suite passed 23 cases with 1,703 assertions, including
memory-free endpoint lifts/returns, memory32-to-memory64 mixed tuples, deferred
callbacks, future peer retention, exported endpoint lifetime, partial input/result
failure, abort in parameter and result realloc, repeated fuel yields and allocation
failure sweeps. The related regression passed 80/80 targets in 3.95 seconds.
Reproduce with `cmake --build --preset win-core3-asan-user` and
`ctest --preset win-core3-asan-user -R component_async_lower --output-on-failure`.
For `dbe1e8f`, the [native run](https://github.com/qigao/turbowasm/actions/runs/37700265372)
passed all five jobs. Linux MIR passed 198/198 targets in 1.30 seconds and macOS
MIR passed 198/198 in 3.15 seconds, including the compiled async-lower suite
(0.05 and 0.06 seconds respectively).

Local own/borrow transactions are now connected as well. Own values reserve
inaccessible handles before conversion commits; resources and endpoints in a
mixed tuple share one transaction. Local borrow lowering passes the representation
to the defining instance while the source lender remains held through terminal
delivery. Lifted resource owner records are quota-bounded and retain the exec even
after their canonical handles leave the table. Exec destruction also refuses live
guest resource handles instead of discarding them.

Destructors use fresh synchronous context slots and preserve live-task fuel and
interruption control. They cannot task.return or block the enclosing async task;
unwind restores the auxiliary state and does not retry the destructor. External
resource-value destruction consumes its logical owner even when guest cleanup fails.

Windows ASan passed the expanded async-lower suite: 36 cases, 2,758 assertions.
The related regression passed 80/80 targets in 4.21 seconds. New cases cover own
round trips, mixed resource/endpoint commit and rollback, borrow lender timing,
premature guest drop, host-owner lifetime/quota/failing destructor, inaccessible
reservations, parameter/result realloc unwind, destructor context/fuel/trap/wait
and allocation failures. At `8534118`, the
[native run](https://github.com/qigao/turbowasm/actions/runs/37702027034)
passed all five jobs. Linux MIR passed 198/198 targets in 1.96 seconds and macOS
arm64 MIR passed 198/198 in 3.08 seconds. The compiled async-lower suite passed in
0.13 seconds on each platform, including assertions for compiled resource callees
and destructors.

## Instantiated ownership-bearing stream/future payloads

Local stream/future builtins now retain independent resource/endpoint codecs for
each admitted operation. Contexts share the instance handle quota as a bound and
remain alive through event/error/cancellation delivery, including after the
initiating task exits. Failed admission releases the prepared context immediately.
Mixed payload conversion commits all handle reservations together or rolls them
back before releasing values; suspended guest realloc retains both endpoint guards.
Numeric and string-only copies keep their existing allocation behavior.

`component_async_payload_test.c` executes validated Component binaries with
memory32/64, owned-resource batches, mixed resource/endpoint/string values and
owned-resource futures in both directions. It checks independent concurrent reads,
realloc suspension/unwind, context lifetime after task exit, cancellation delivery,
out-of-bounds admission, quota rollback, the same-Component nonnumeric copy trap,
and allocation failures. Every case checks allocation cleanup. Windows ASan passed
12 cases with 867 assertions; the related regression passed 81/81 targets in
4.36 seconds. At `89293b4`, the
[native run](https://github.com/qigao/turbowasm/actions/runs/37704068382)
passed all five jobs. Linux MIR passed 200/200 targets in 2.62 seconds and macOS
arm64 MIR passed 200/200 in 3.39 seconds. The compiled payload suite passed in
0.05 and 0.07 seconds respectively. Its allocation-failure sweep exhausts failure
positions through success without assuming the same allocation count for each
execution backend.

## Private async instance imports

The capability router can now resolve async instance-function imports to an
initialized provider's lift. Instantiation validates signatures, rejects ambiguous
providers, and retains selected execs through consumer teardown. Outgoing calls
stay on the consumer's bounded progress list; their tasks use the provider's quota.
Early results retain the callee until Core exit. Imported resource identities and
borrowed handles remain excluded from this private boundary.

Return conversion uses per-call realloc context and the actual callee's execution
control, entering the consumer's memory32 from the provider's memory64. Both
domains retain auxiliary guards through fuel suspension, and context slots are
isolated and restored on success/unwind. Endpoint values use the existing
transaction codecs in each instance's canonical handle table.

`component_async_import_test.c` instantiates two independently validated Component
binaries and checks eager/deferred/early results, string and nested-endpoint round
trips, cancellation including a competing normal return, provider traps and quota,
result realloc suspension/unwind, ambiguous/incompatible imports, and allocation
failures in calls and partial instantiation. It checks provider reference counts,
task/handle cleanup and allocation balance. Import admission also rejects resource
leaves hidden inside endpoint payloads even if separately decoded graphs reuse
the same numeric resource ID. The shared type walker keeps this full-tree query
separate from transfer carrier features. Windows ASan passed 15 import cases with
1,919 assertions and 19 async-type cases with 2,565 assertions; related regression
passed 82/82 targets in 4.64 seconds. The MIR
variant checks compiled caller/provider functions and callbacks; native
macOS arm64 qualification at `c6ec840` passed 202/202 targets in 3.01 seconds,
including the compiled import suite in 0.03 seconds, in the
[native run](https://github.com/qigao/turbowasm/actions/runs/37705223445).
Linux MIR and Android jobs were still running when this result was recorded.

A further interruption case resumes a suspended consumer realloc with the
provider task's new interrupt policy. It verifies the interruption yield reason,
both retained domain guards, successful continuation without replay and complete
endpoint cleanup. Windows ASan passed all 16 import cases with 2,086 assertions;
production code was unchanged after the preceding related regression. At
`c1f1725`, Linux x64 MIR passed 202/202 targets in 2.60 seconds and macOS arm64
MIR passed 202/202 in 3.44 seconds, including the compiled import suite in
0.04/0.05 seconds. All five jobs passed in the
[native run](https://github.com/qigao/turbowasm/actions/runs/37705735849).

Private async instantiation now binds resource declarations to generative
instance keys through an exec-owned type view. Alias classes, including nested
interface types, retain one key; distinct instantiations cannot exchange a
resource-bearing endpoint merely because their declaration IDs match. Numeric
declaration IDs and the decoded metadata remain unchanged. Resource-free binaries
continue to borrow their decoded graph without extra allocation. The new formal
type-view suite passes 5 cases and 4,171 assertions under Windows ASan, including
every allocation failure through view creation and Core instantiation, retained
source metadata, and actual endpoint admission across two instances of one binary.
Related regression passed 83/83 targets in 6.14 seconds before the final added
constructor failure sweep; that additional case passed separately. At `7b836b2`,
Linux x64 MIR passed 203/203 targets in 2.78 seconds and macOS arm64 MIR passed
203/203 in 6.34 seconds, including the type-view suite in 0.05/0.07 seconds.
All five jobs passed in the
[native run](https://github.com/qigao/turbowasm/actions/runs/37707338136).

Private resource import resolution now retains initialized providers and maps
consumer aliases to their runtime resource identities before function binding.
Owned values, including resources alongside strings, cross instance tables with
different declaration IDs. Imported destruction follows the provider chain and
shares the guest caller's fuel/interruption control and trap destination. A
foreign destructor's temporary synchronous context blocks sibling task entry
without marking the provider as executing between resumes. Public async entry
points remain closed.

The new `component_async_resource_import.wat` fixture passes wasm-tools 1.261.0
validation and exercises the existing resource provider through real canonical
imports. Windows ASan passed 13 cases with 2,267 assertions: own round trips,
host result retention, memory32/memory64 mixed transfers, transitive provider
lifetimes, ambiguous/mismatched providers, table quota rejection, suspended
result rollback, fuel-suspended destructor completion/unwind, destructor traps,
and allocation failures throughout construction and transfers. Related regression
passed 84/84 targets in 7.03 seconds. The MIR variant requires compiled caller,
provider and destructor handles. At `92eff58`, Linux x64 MIR passed 205/205
targets in 2.73 seconds and macOS arm64 MIR passed 205/205 in 3.12 seconds,
including the compiled resource-import suite in 0.07/0.05 seconds. All five jobs
passed in the [native run](https://github.com/qigao/turbowasm/actions/runs/37708969643).

Private imported borrow parameters now use task-scoped non-owning handles outside
their defining instance. Terminal delivery retains transitive source loans;
failure teardown unwinds child calls before clearing a parent's borrowed handles.
Pinned children or still-lent handles keep the task and caller boundary alive for
a later cleanup retry. Child dependencies have a 256-level admission bound and
share the existing exec-owned call frames. Results and endpoint payloads still
reject borrow leaves.

The extended fixture validates with wasm-tools 1.261.0. Windows ASan passed
20 resource-import cases with 4,844 assertions, including direct/three-instance
borrows, fuel suspension, premature drop/return traps, forced transitive unwind,
subsequent frame collection and an allocation-failure sweep. Task tests cover
atomic scope clearing, counter overflow, pinned-child retry and retention of the
incoming caller while a borrowed handle remains lent. Async-call tests cover the
dependency quota before publication. Related regression passed 84/84 targets in
8.03 seconds. At `67a6ceb`, Linux x64 MIR passed 205/205 targets in 2.82 seconds
and macOS arm64 MIR passed 205/205 in 3.15 seconds, including the compiled
resource-import suite in 0.12/0.10 seconds. All five jobs passed in the
[native run](https://github.com/qigao/turbowasm/actions/runs/37710876080).

Private cross-instance payload rendezvous now receives its driving task explicitly,
including copies triggered by a third-instance forward. Operation-local realloc
options and copy scopes retain source/destination auxiliary guards through fuel
and interruption yields. Source cleanup destructors share that driver's control;
all guards restore after commit, rollback or forced unwind. Pending buffers retain
their exec/options but no pointer to a task that may have already exited.

The expanded wasm-tools-validated fixture exercises owned-resource streams and
futures, mixed resource/endpoint/string payloads, both memory directions and
rendezvous orders, exited asynchronous readers, suspended conversion unwind,
foreign failure destructors, third-instance forwarding and allocation failures.
Windows ASan passed 26 resource-import cases with 7,575 assertions and the related
84/84 targets in 7.60 seconds. Native MIR qualification for this extension is pending.

Async host capability callbacks,
nested Component instantiation, public task/endpoint owners, synchronous
destructor-to-async lowering and the broader Component/WASI gaps remain open.
