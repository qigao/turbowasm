# TurboWasm Architecture

## Ownership

TurboWasm owns WebAssembly-specific semantics:

```text
Wasm bytes
   -> bounded decoder
   -> validation
   -> retained typed metadata
   -> module / instance / memory / table semantics
   -> interpreter
   -> optional native backends
```

Salts remains the shared semantic/runtime foundation:

```text
CMeta
  -> stable semantic type identity and vector/mask descriptors

Salts::SIMD
  -> portable SIMD C ABI
  -> private SIMDe/native implementation

Salts::Coroutine
  -> private retained interpreter frames for backend-neutral resumable execution

CFlow / Executor
  -> optional bounded scheduling and virtual/system clocks

Salts::NativeIO
  -> optional bounded OS async-I/O progress; NativeIO request slots remain the
     operation/completion source of truth
```

TurboWasm does **not** expose SIMDe types and does not vendor a second vector
abstraction.

## Typed SIMD

The WebAssembly binary type system has one `v128` value type. TurboWasm keeps
that storage fact but refines lane semantics after opcode validation:

```text
v128 storage
  + i32x4 operation -> I32X4 semantic value
  + f32x4 operation -> F32X4 semantic value
  + i32x4.eq        -> B32X4 mask value
```

The refinement maps to canonical CMeta vector descriptors. Execution uses
`Salts::SIMD`.

This is intentionally different from making CMeta or CFlow understand Wasm
opcodes. CMeta owns vector/mask type semantics; Salts::SIMD owns portable
execution; TurboWasm owns Wasm instruction semantics.

## Execution tiers

The current execution model is:

```text
validated Wasm function
        |
        +-- cold / unsupported shape
        |       |
        |       v
        |   interpreter
        |
        '-- hot + eligible
                |
                v
          backend-neutral JIT contract
                |
                v
               MIR
          /             \
 scalar MIR lowering   SIMD helper lowering
                         |
                         v
                 private v128 slots
                         |
                         v
                    Salts::SIMD
```

Compilation is lazy and per function. An unsupported lowering shape does not
invalidate the module and does not force unrelated functions out of the JIT.

MIR remains a code-generation backend only. TurboWasm owns and preserves:

- linear-memory bounds;
- table bounds and indirect-call type checks;
- Wasm trap behavior;
- fuel/interruption checkpoints;
- call-depth and runtime state;
- host capability boundaries;
- interpreter fallback policy.

No backend is allowed to weaken these checks.

## Structured SIMD lowering

The MIR backend does not put `v128` into the public/native function ABI.
Instead, validated structured control maps values to typed merge locations:

```text
validated block / loop / if signature
        |
        +-- scalar value -> MIR register
        |
        '-- v128 value   -> canonical invocation-local slot
```

Taken branches copy SIMD values into canonical merge slots before jumping.
`br`, `br_if`, `br_table`, block/if results, loop parameters, and
`select` therefore share the same validated control signatures as the
interpreter.

Nested compiled calls save/replace/restore the private SIMD slot frame.

### MIR scalar argument boundary

Native scalar entry points borrow the invocation's `const turbowasm_value *`
argument array instead of enumerating C function-pointer signatures by arity.
The validated module remains the owner of parameter type metadata; compiled
handles cannot outlive that module. Entry validates count and kinds before
native execution, then copies scalar payloads into private MIR locals. The
borrow ends when the entry returns, is never stored in the instance, and is
not reused when a self tail call replaces those locals. Nested calls have
independent frames. Admission remains bounded by the module's existing limits;
the bridge performs no allocation or ownership transfer and writes no input
values. Trap/status handling remains at the invocation boundary.

This removes the private native entry's arity and mixed-scalar argument limits
without changing the installed API or its data layout. Single-result admission
and the separate direct-call helper signatures are independent restrictions.
The alternative of generating every C signature combination scales
exponentially with arity and does not solve mixed types or future reference
carriers. The array boundary adds scalar loads at function entry. Regression
coverage checks mixed memory64 addresses/float payloads, more than two integer
parameters, NaN payloads, negative zero, input immutability and trap parity;
MIR tests require a compiled handle. Removing this boundary requires reverting
entry emission, invocation and admission together, never only the scanner.

## Reusable compiled-artifact policy

Validated-module artifacts and compiled-function artifacts are separate layers.

```text
exact Wasm source
    -> validated artifact identity
       (source SHA-256 + feature fingerprint)
            |
            + function index
            + backend/compiler/target fingerprint
            v
        JIT cache key
            |
      +-----+------+
      |            |
     miss          hit
      |            |
 lazy compile   backend-private
      |         restore/validation
      |            |
      +-----+------+
            v
      compiled function
```

TurboWasm owns only the backend-neutral cache key, lookup/store routing and
maximum blob-size admission. It never interprets backend-native artifact bytes.
A backend restore callback must validate its private artifact format and enforce
its executable-memory/resource policy before returning a compiled handle.
Corrupt, incompatible, oversized or failed cache restores fall back to the
ordinary lazy compile path and never bypass Wasm module validation identity.

Persistent machine code is optional. The pinned MIR v1 backend deliberately
does not implement the persistence callbacks: its public API can write/read MIR
IR and generate machine code, but it does not expose a stable public
generated-machine-code plus relocation artifact import/export contract.
TurboWasm does not use MIR's underscored private code publication/dump helpers
as a persistence ABI.

## Executable-memory policy

Executable mappings are explicitly bounded.

```text
TurboWasm MIR backend
        |
        | MIR_set_code_limit(8 MiB)
        v
pinned MIR package
        |
        +-- mapping fits -> generate native code
        |
        '-- mapping exceeds limit -> MIR_gen() fails closed
                                      |
                                      v
                              interpreter fallback
```

The limit applies to the MIR context's executable mappings, not to an estimate
derived from Wasm bytecode size. The installed `TurboWasm::Runtime` interface
remains MIR-free.

## Native vector policy

The pinned MIR v1.0 IR exposes integer, floating-point, pointer, and block
register types, but no vector/v128 register type.

Therefore the supported contract is:

```text
Wasm SIMD
   -> TurboWasm private v128 slots
   -> helper calls
   -> Salts::SIMD
```

TurboWasm intentionally does not emulate a supposed native-v128 ABI with pairs
of scalar MIR registers. A future native-vector backend requires either a MIR
vector ABI or a different backend with real vector register semantics.

## Module lifetime

A `turbowasm_module` is a zero-initialized opaque owner handle. The loader
borrows immutable input bytes while private validation and retained metadata
remain implementation details behind the public handle.

A configured load may also carry a caller-owned allocator and explicit resource
limits for module bytes, individual Runtime allocations, owned linear memories
and owned tables. Module-derived instances and restartable executions inherit
that policy; imported memories/tables retain the provider instance policy.

## Capability status

```text
binary/module bootstrap                 complete
retained typed validation metadata      implemented
core interpreter semantics              implemented for qualified surface
memory/table/reference/bulk semantics   implemented
multi-memory/custom-pages/ext-const     implemented + upstream qualified
memory64                                interpreter + shared-memory execution implemented
                                        MIR native helper lowering for unshared memory
tail calls                              implemented + upstream qualified
typed exception handling                implemented + upstream qualified
relaxed SIMD                            implemented + upstream qualified
fuel + interruption                     implemented
restartable interpreter execution       implemented
typed module/host linking               implemented
optional CFlow deadline adapter         implemented
optional NativeIO host-wait bridge      implemented
lazy per-function MIR JIT               implemented
scalar/helper-backed SIMD MIR           implemented
per-function fallback isolation         implemented
backend-neutral JIT artifact cache       implemented for opt-in backends
MIR native artifact persistence          intentionally unsupported
executable MIR mapping budget           implemented

WASI Preview 1 capability layer         implemented
WASI filesystem provider ABI            implemented
Salts HostFS / optional littlefs         implemented
WASI NativeIO async fd projection       implemented
threads/shared memory/atomics           implemented + upstream qualified
legacy WASI threads adapter             implemented
caller allocator/resource policy        implemented
WebAssembly GC / table64                implemented + Core 3.0 qualified
Component Model                         tracked by #307
WASI 0.2 typed interfaces               tracked by #308
native vector JIT backend               backend-dependent future work
```

## Salts 2.x dependency migration

Salts 2.1.0 removes the old SIMD, platform, filesystem and coroutine-header
names used by TurboWasm. The migration uses the published `cmeta_*` identifiers
and `<coro.h>` directly. Imported `Salts::*` targets and the valid `<salts/...>`
header paths remain unchanged. Keeping an older SDK would prevent the release
upgrade; adding legacy aliases would create a second API surface to maintain.

`turbowasm_v128.bits` now names `cmeta_v128`, still the aligned 16-byte CMeta
storage carrier. TurboWasm function names, Wasm algorithms, artifact formats,
state ownership, locking, cleanup and error propagation remain unchanged.
Consumers naming removed Salts types/functions must migrate and rebuild against
the same SDK. No runtime state or persisted data migration is required.

The build migration separates option defaults, reusable presets, SDK acquisition
and CI orchestration. Native CI and packaging share one workflow, while SDK
packaging retains its Runtime + Component scope. vcpkg owns optional MIR/WABT
dependencies; first-party SDKs are resolved only through explicit roots.

Validation covers the existing runtime/SIMD/threads/filesystem tests, C++ public
headers, installed-package tests, and CI conformance suites. Windows, Linux,
macOS and Android keep distinct profiles. Rollback requires reverting the API
and build migration together and selecting the matching earlier SDK; there is
no automatic dependency downgrade.

## Core 3.0 managed-reference boundary

The implemented reference-control increment adds `call_ref`, `return_call_ref`,
`ref.as_non_null`, `br_on_null`, `br_on_non_null`, bottom function/external
references and lexical initialization tracking for non-defaultable locals.
Recursive groups, composite type declarations and declared subtyping are parsed
and compared by whole-group identity. New
semantics use a distinct artifact feature fingerprint; regenerate artifacts
from source instead of restoring metadata validated by the previous rules.
Artifact schema 2 retains recursion groups, supertypes, composite kinds and
packed/mutable fields; schema 1 caches must be regenerated from source. Previous
heap-kind numeric values are preserved. Type dependency depth is bounded by the
`TURBOWASM_TYPE_DEPENDENCY_LIMIT` CMake setting (default 256).

Core 3.0 includes recursive type groups, declared subtyping, GC structures and
arrays, typed function references, and 64-bit table addresses. Function-only
types and instance-owned borrowed references could not express this contract;
the implemented composite types and store/root boundary extend them. The normative baseline is the
[Core 3.0 specification](https://webassembly.github.io/spec/core/), with tests
from the `wg-3.0` tag (`9d36019973201a19f9c9ebb0f10828b2fe2374aa`).
The pinned suite qualifies 63970 binary commands in 258 files with zero failures
and zero unsupported commands. Another 1229 text-syntax checks belong to the
wasm-tools frontend and are excluded from the binary Runtime count.

The ownership boundary is an explicit `turbowasm_store`. A store owns
the canonical recursive-type registry, managed objects, and registered roots.
Instances sharing managed references must belong to the same store; cross-store
reference admission fails with a type error. Existing instance creation keeps
its current behavior for existing value kinds. An additional creation API takes
a store for GC-enabled instances. Existing linker borrowing rules for provider
instances and modules remain in force.

The public extension consists of opaque store and root handles, store creation
and destruction, instance creation in a store, and explicit reference
retain/release operations. `turbowasm_value` gains a managed-reference carrier;
table descriptors and accessors gain 64-bit forms alongside the existing 32-bit
forms. Existing source callers need no changes unless using these capabilities,
but consumers must rebuild if exported value or descriptor layout changes.
No compatibility alias may truncate a 64-bit table index.

Managed values passed to a host callback are borrowed for that callback. Returned
managed values are borrowed until the next operation that can collect in the
same store. A host retaining a value across that boundary must acquire an
explicit root; allocation failure leaves the original borrow unchanged.
Suspended executions register their arguments, results, operand stacks, locals,
and exception payloads as roots until resume or destruction. Instance globals,
tables, and retained element segments also contribute roots. A copied C value
alone does not acquire ownership. Store destruction must reject live instances,
executions or host roots rather than leave dangling handles.

The collector uses non-moving tracing with store-local stable handles
and generation checks. Object fields and array elements have one authoritative
allocation; tracing metadata is derived from immutable type descriptors.
Recursive types compare whole recursion groups, preserving group membership,
finality and declared supertypes. The store owns immutable descriptor copies,
reuses equivalent type tables on repeat instantiation, and interns equivalent
group projections within its registry. Registry metadata lasts until store
destruction and counts against its byte budget. Comparing module-local type indices
across modules is insufficient. The interpreter, linker, validator and host
admission boundary must share the same subtype relation.

Each store has finite, explicit byte and object-count budgets covering payloads,
object metadata, root registrations and tracing worklists. Size calculations are
checked before allocation. At an allocation safe point, collect unreachable
objects, then either publish a fully initialized object or return the existing
resource/allocation error. Failed initialization does not publish a handle or
partially modify an existing object. No unbounded growth or retain-until-instance-
destruction substitute is permitted. Collection is synchronous on the store's
single owner thread; simultaneous execution in the same GC store from another thread is rejected. Same-owner nested host calls
register independent frames.
Existing shared-memory threads do not imply shared GC support.

Alternatives considered: instance-lifetime arenas cannot reclaim transient GC
objects or cycles; independent instance collectors complicate cross-instance
roots; replacing the engine adds a runtime dependency and changes the execution
and JIT architecture. A store-local tracing collector fits the current explicit
instance/execution boundaries with fewer hidden lifetime rules. Its cost is
stop-the-world collection and a new public ownership contract. MIR may continue
using its documented interpreter admission path for unsupported functions;
native GC lowering requires separate root-map validation before admission.

Migration is ordered: strict upstream reporting, reference/control validation,
canonical recursive types, store/root lifecycle, GC execution and constants,
64-bit tables, then complete upstream qualification. Validation must cover cycles,
cross-module type equivalence, failed allocations, host retention, suspended and
nested calls, import identity, null/cast/bounds traps, and collection under a
small budget. Rollback removes the new optional entry points and value kinds as
one change; existing non-GC state has no persistent migration. The complete
validation/execution regression and pinned suite are required gates. Dense tables
have a UINT32_MAX element resource ceiling, independently of their 64-bit declared
limits; no table address is truncated. Runtime limits may lower that ceiling.
Suspended execution destruction unwinds at its safe point; a host-wait callback
receives INTERRUPTED and must clean up and propagate it before destruction ends.

## Memory64 native and shared execution contract

Memory64 uses unsigned 64-bit addresses and offsets through validation, native
helper calls, and storage range checks. Effective-address addition is checked
before any host-size conversion. The existing instance memory is the sole
owner; imported memories resolve to that same backing. Shared memory supports
multiple readers/writers through its existing access lock, global SC order for
atomics, and bounded waiter registry. Growth commits pages and storage together
under the write lock; failure leaves the previous backing intact. No native
helper retains a data pointer across a call, growth, or wait. Wait releases the
data lock before sleeping and preserves interruption, timeout, and shutdown
semantics. Instance destruction still requires quiescent users.

The implementation extends the existing MIR helper boundary rather than
introducing a second memory owner. MIR continues to exclude shared memories;
shared memory64 executes through the interpreter with guarded storage access.
This costs a helper call per memory operation but shares bounds, locking, and
trap semantics with the Runtime. Public layouts and lifecycle contracts remain
unchanged. Resource limits remain the configured byte/page limits and host
address-space limit, not the width of a guest address. Validation covers
address typing, high offsets, arithmetic overflow, atomic alignment, growth
failure, imported backing, waits, and native/interpreter equivalence. Rollback
can restore feature admission without migrating stored data.

Reference: [Memory64 proposal](https://github.com/WebAssembly/memory64/blob/main/proposals/memory64/Overview.md).
