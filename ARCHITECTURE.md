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
argument transfer performs no allocation or ownership transfer and writes no input
values. Trap/status handling remains at the invocation boundary.

This removes the private native entry's arity and mixed-scalar argument limits
without changing the installed API or its data layout. General scalar direct
calls and tail transfers use the scratch protocols below.
The alternative of generating every C signature combination scales
exponentially with arity and does not solve mixed types or future reference
carriers. The array boundary adds scalar loads at function entry. Regression
coverage checks mixed memory64 addresses/float payloads, more than two integer
parameters, NaN payloads, negative zero, input immutability and trap parity;
MIR tests require a compiled handle. Removing this boundary requires reverting
entry emission, invocation and admission together, never only the scanner.

### MIR scalar result boundary

All native entry points return through a caller-owned `turbowasm_value` array;
the machine function itself returns void. Validated result metadata determines
the required capacity, including zero results. The invocation boundary checks
capacity before entering generated code and publishes the count only after a
successful return. A trap or pending tail transfer publishes no results. Output
stores occur after the final checkpoint, so failing execution leaves the caller's
result array unchanged. Input and output may alias because parameters have
already been copied into invocation-local registers.

Structured functions reserve typed merge registers for every function result.
Natural completion, return and branches to the function label all materialize
the same ordered result tuple; conditional branches materialize only when taken.
The module's existing result and code limits bound this register budget, with
checked capacity arithmetic. This adds no runtime allocation and transfers no
ownership. The boundary is single-threaded within an invocation; nested calls
use their own result buffers. Scalar payloads, including floating-point bit
patterns, retain their public representation.

The array ABI avoids architecture-specific native multi-return limits and an
exponential family of typed function pointers. It adds output stores in exchange
for a single private calling convention. No installed API, configuration or
serialized format changes. Verification covers empty and mixed result tuples,
natural and explicit returns, conditional/table branches, self tail calls,
capacity rejection and fuel/trap parity against the interpreter. Rollback must
revert all three emitters and their common invocation bridge together.

### MIR direct-call scratch ownership

General scalar calls marshal ordered values into two disjoint invocation-owned
arrays. Compilation records the largest argument and result tuples among direct
call sites, plus the total number of result registers needed by those sites.
These are derived from validated signatures, never independently mutable state.
The invocation bridge allocates the checked sum of both array capacities through
the Runtime allocator, once per invocation containing general calls, and frees
it on every exit. Allocation failure returns OUT_OF_MEMORY before guest execution;
the existing Runtime allocation quota and call-depth limit bound retained frames.

Generated code fills arguments, invokes the existing Runtime dispatcher, checks
status and result shape, then copies successful results into private registers.
The arrays are reused only after the synchronous call completes. Nested native,
interpreted and host calls borrow the current arguments while owning independent
scratch frames, so callbacks cannot overwrite their caller's pending values.
This is a single-threaded protocol; no pointers escape return, trap, interruption
or cancellation. Scalars need no retain/release or GC root registration. Shared
fuel, trap, exception and call-depth semantics remain owned by Runtime.

This replaces signature enumeration for general calls while preserving the
existing small straight-line specializations. No public API or wire format
changes. Tests must require compiled callers, exercise nested mixed and empty
tuples, compare fuel/trap behavior, and verify allocation failure and cleanup.
Rollback removes general-call admission, emission and scratch allocation together.

### MIR scalar tail-transfer ownership

Following the [Core tail-call execution rule](https://webassembly.github.io/spec/core/exec/instructions.html#exec-return-call),
scalar `return_call` marshals a validated argument tuple before returning from
generated code. The Runtime dispatcher owns the pending tuple until the next
callee completes or replaces it. Two values fit in the existing inline storage;
larger tuples reuse the Runtime value-stack allocator, with checked size arithmetic,
uint32 signature bounds and the configured allocation quota. Capacity is bounded
by the largest validated parameter tuple encountered in this module (rounded by
the existing reserve policy), independent of tail-chain length. The pending count
is published only after type checks, reservation and copying succeed. Allocation
failure returns OUT_OF_MEMORY without publishing a transfer or result.

The single-threaded dispatcher frees retained storage on every exit, including
interpreter/host execution, trap, fuel exhaustion and interruption. Nested ordinary
calls own separate dispatch storage. MIR call scratch may be released immediately
after the copy; no borrowed pointer into it survives. Inputs must remain valid for
the copy and cannot span a reserve operation that invalidates their own storage.
Only scalar signatures are admitted by this lowering; it adds no GC ownership
contract. Self-tail loops continue to replace locals directly.

Dispatch repeats at the same logical depth after generated frames unwind. Result
capacity and ordered result types remain those of the original invocation; zero
and multiple results require no native return-register convention. This avoids
recursive C calls and platform-specific tail ABIs without changing installed APIs
or serialized formats. Tests cover long mixed-arity chains, interpreted and host
targets, empty/mixed results, allocation cleanup and checkpoint parity. Rollback
must remove general-tail admission, emission and dispatcher storage together.

### MIR table storage boundary

Native table operations delegate to the same Runtime table objects and element
segments as the interpreter. The MIR decoder derives operand widths from validated
table limits: destinations and sources use their respective address types;
`table.copy` uses i64 length only when both tables are table64; `table.init`
keeps i32 source and length. These are the [Core table typing rules](https://webassembly.github.io/spec/core/valid/instructions.html#valid-table-copy).
Admission and emission share one decoded signature, so a rejected operation cannot
reach a mismatched helper ABI. Memory and table helpers remain separate adapters
while sharing native integer argument emission.

Helpers borrow the instance for one synchronous operation and never cache table
entry pointers. Imported tables resolve through Runtime to their original owner;
native code has no separate table size, drop state or GC reference store. Runtime
checks both ranges before copying, preserves overlap semantics, and allocates
table.init staging through its configured allocator. Resource limits, allocation
failure, and TABLE_OUT_OF_BOUNDS traps retain the interpreter contract. Each
instruction's existing checkpoint runs before the helper can mutate storage.
There are no new buffers, retained references, locks or public APIs in this boundary.

The first admitted operations were size, init, copy and element drop. The managed
reference frame below adds get/set/grow/fill using the same Runtime backing.
Indirect calls use the separate boundary below; GC instructions remain interpreted. Tests compare table
contents and drop state as well as status/results, including table32/table64
mixing, imported backing, overlap, full-width bounds, fuel and allocation failure.
Rollback removes table decoder admission and emission with the helper registration;
Runtime table ownership and the public API are unchanged.

### MIR managed reference frame

Native references use complete `turbowasm_value` cells, never unrooted integer
handles or pointers to temporary operand values. Each invocation owns separate
local and register cells plus its existing call argument/result scratch. Counts
derive from validated locals, code and merge-register budgets; checked allocation
uses the Runtime allocator and quota. Scalar-only functions keep their existing
register path. Copies materialize independent cells at local assignment, control
merges and calls, so loop iteration cannot mutate a previously saved reference.
Unused initialized cells may conservatively retain a reference until overwrite
or frame exit; retained storage is bounded by the compiled function's cell budget.

The Runtime dispatcher registers one source with the owning store while compiled
code is active. It traces current arguments, the native cell array, pending tail
arguments and completed results. The source is owned by the current execution for
coroutine teardown. Native scratch is unpublished before freeing it; successful
outputs and tail handoffs remain visible to that source during cleanup. Before
entering the interpreter the dispatcher removes its source and the interpreter
installs its ordinary frame source before allocating. Nested calls have separate
frames; tail chains reuse one dispatcher source. All access follows the store's
single-owner thread contract, and root-budget exhaustion returns OUT_OF_MEMORY.

The validated semantic types remain authoritative at entry and call boundaries:
nullability, store/generation identity and nominal function/GC subtyping must be
checked, not just the carrier byte. Reference-bearing table operations continue
to use Runtime's semantic checks and shared backing. No installed type, lifetime,
configuration or binary format changes. Tests must cover native locals/branches,
direct and tail calls, host-triggered collection, fuel/interruption, root and allocation
limits, invalid reference arguments and table32/table64 reference operations.
Public resumable executions continue to use the interpreter; this increment does
not admit native frames into the public yield/resume path.
Rollback removes reference admission and emission together with frame allocation
and root registration; it must not leave admitted code with untraced cells.

### MIR indirect call ownership

`call_indirect`, `call_ref` and their tail forms resolve targets through Runtime's
existing table lookup and defined-type matching. The interpreter and native
helper share the decoded-value resolver; MIR does not cache table entries or
generated entry addresses. This preserves table32/table64 bounds, null traps,
nominal subtyping, original function owners and imported backing. The semantic
reference is the [Core execution contract](https://webassembly.github.io/spec/core/exec/instructions.html#exec-call-indirect).

The current single execution owner borrows target instances under the existing
funcref/linker lifetime contract. Native call scratch contains the validated
parameter tuple followed by one selector cell, with checked counts and the same
Runtime allocation limits as direct calls. Call results use the existing rooted
result cells. Resolution and type errors happen before invoking a target; errors
and pending exceptions return to the originating instance. Ordinary calls add one
logical depth; tail requests copy parameters into dispatcher-owned bounded storage
and publish the target instance/index only after validation and allocation succeed.
After generated code unwinds, the dispatcher switches the active instance and
store root registration at unchanged depth. Imported function aliases resolve
in that same loop, without adding interpreter wrapper frames. Nested calls own separate frames;
tail chains retain one pending tuple. No locks, retained public handles, new
configuration or public ABI are introduced.

Using a generated entry pointer directly would bypass Runtime type, ownership,
fuel and exception boundaries; recursively implementing tail transfers would
consume the native stack. The chosen helper/trampoline approach adds target
resolution per call while preserving those invariants. Validation covers both
address widths, typed references, mixed/reference tuples, cross-instance targets,
host/interpreted targets, long tail chains, GC, traps, fuel and allocation failure.
Rollback removes these four admission cases and their emitter together; Runtime
interpreted behavior remains the compatibility baseline.

### MIR GC instruction boundary

GC lowering reuses `turbowasm_gc_execute` through a private adapter. The store
remains the sole owner of object handles, type identities, quotas and collection;
generated code neither caches object addresses nor implements a second heap.
A decoded instruction view borrows validated module bytes and field metadata for
the compiled function's lifetime. Its signature supplies complete value cells to
the existing invocation scratch/root source; the returned cell is copied into a
rooted native reference or vector location before another allocation or callback.
Execution remains single-owner, with existing fuel/interruption checkpoints and
Runtime errors. Cast branches reuse taken-only target assignments.

Signatures describe fixed, repeated-array or heterogeneous-struct operands without
allocating a descriptor array. Scratch demand is computed from reachable emission,
so polymorphic dead array constructors cannot inflate the invocation budget.
Allocation, array mutation, segment bounds and failed casts keep Runtime's error
and commit semantics. The chosen adapter adds bounded value copies and preserves
the installed API; inline object layouts were rejected because they would couple
MIR to heap lifetime and duplicate checks. Rollback removes GC admission and its
private adapter together. Validation includes all GC opcodes, packed/vector/ref
fields, both cast edges, segment/drop behavior, null/bounds/cast traps, native-only
roots under collection, quotas, allocation failures and fuel parity.

### MIR globals and nullable-reference control

Global reads and writes use Runtime's existing global accessors. The provider's
global cell remains the sole state owner, including imported aliases; generated
code copies complete values through invocation scratch and never caches a global
address. Mutability and semantic type checks precede writes. Managed values in
globals are traced by their instance/store and temporary reference cells remain
in the native frame's existing root source. Scalar/vector globals use the same
value boundary. No installed API or ownership contract changes.

`br_on_null` removes its reference from the taken edge and retains it on the
non-null fallthrough; `br_on_non_null` forwards it on the taken edge and removes
it on null fallthrough. Only a taken edge writes target merge cells, preserving
loop inputs and values still live on fallthrough. Both reuse Runtime null testing
and the existing structured target metadata. `unreachable` records its defined
trap after its instruction checkpoint and terminates the native path.

Direct field access or a second reference representation would duplicate
ownership/type rules. The chosen helper boundary adds bounded copies and calls,
but retains existing allocation limits, single-owner execution, error reporting
and cleanup. Tests cover imported/provider mutation, scalar/vector/reference
values, GC retention, both branch outcomes at block/function/loop targets, fuel,
traps and rejected writes. Rollback removes admission and emission together.

### MIR immediate SIMD boundary

Shuffle, lane extract/replace and extended SIMD memory instructions reuse the
Runtime SIMD executor through a private single-instruction bridge. Generated code
passes a bounded view of the already validated immutable instruction bytes and
at most three complete scalar/vector values; the bridge uses a fixed local stack
and commits at most one result. No host callback, suspension, GC reference or
dynamic allocation is introduced. The invocation owns call scratch and SIMD
slots; the module owns the instruction bytes and outlives the compiled function.
This is an in-process pointer, consistent with MIR's unsupported artifact
persistence contract, and is never serialized.

The existing single execution owner pays one checkpoint before each instruction.
Shared/unshared memory32/64 accesses use Runtime's existing address, bounds and
shared-byte access rules. Memory writes commit only after those checks; errors
propagate through invocation status/trap, without publishing a result. Destroying
an instance/backend requires quiescence under the existing lifecycle contract.
The alternative of duplicating immediate parsing and vector execution inside the
backend would create another semantic implementation. The bridge trades bounded
decoding/copy overhead for shared semantics and leaves installed APIs unchanged.
Validation covers every descriptor and lane, vector shape/bit results, memory
contents, overflow/bounds traps, fuel and native compilation. Rollback removes
the new admission and bridge together, preserving the previous SIMD subset.

### MIR scalar numeric boundary

Missing scalar instructions share the interpreter's numeric primitives through a
private bridge. The bridge copies at most two scalar inputs into a fixed local
stack, runs exactly one primitive, and commits one result only on success.
Division/conversion traps propagate through the existing invocation status and
trap fields. There are no callbacks, GC references, retained pointers or dynamic
allocations inside this bridge. Generated code reuses the invocation-owned call
scratch and pays the existing checkpoint once per instruction.

Existing direct arithmetic lowering remains available. Floating constants use
their binary representation in a typed scratch cell, preserving NaN payloads,
infinities and negative zero without depending on MIR text float parsing.
Duplicating numeric semantics in the backend would create a second source of
truth; the shared primitive boundary trades helper-call overhead for identical
trap and conversion rules without an installed API change. Validation covers all
scalar numeric opcodes, saturation, integer boundary pairs, float bit patterns,
fuel and compiled caller state. Rollback removes admission and bridge emission
together without changing interpreter behavior.

### MIR vector call cells

Vector parameters/results use the same private pointer-based entry ABI as scalar
and reference tuples. Generated code transfers complete `turbowasm_v128` values
between call cells and invocation-owned SIMD slots; no platform vector calling
convention or MIR vector register type is required. Each slot owns its 128 bits
and shape metadata together. Copies preserve metadata; SIMD-producing operations
derive the result shape from the shared descriptor used by the interpreter.

Vector locals occupy a bounded prefix of the slot frame, function result merge
slots follow, and expression/control/call-result slots are allocated from the
remaining validated budget. Local gets materialize new slots; local assignments,
branches, calls and self-tail resets never alias mutable operand slots. Checked
counts include wide result tuples, and the Runtime allocator/quota owns and frees
the complete frame on success, trap, interruption or allocation failure. The
single execution owner saves/restores the parent's slot frame across nested
invocation; vectors contain no GC references, while mixed reference cells remain
traced by the existing dispatcher source.

Passing platform vectors by value would introduce another target-specific ABI;
discarding shape metadata at call boundaries would diverge from Runtime values.
The chosen copy boundary preserves the installed layout and existing direct,
indirect and tail dispatch contracts, at the cost of bounded cell/slot copies.
Validation includes vector locals and control merges, bit/shape preservation,
mixed and wide tuples, host/interpreted/native callees, all tail forms, imported
targets, GC/re-entry, fuel/traps and allocation failure. Rollback removes vector
signature/local admission and marshalling together; existing helper-backed SIMD
operations retain their supported subset.

## Component host values and resource ownership (approved design)

Approved by the user on 2026-10-07. Composite host values, status-returning
value destruction, retained instances, opaque own/borrow handles and explicit move
admission are implemented. This design
extends the former scalar/string/list-only boundary in `component_api.c`; it does
not change Core Runtime value layouts. The existing canonical codec, type graph,
resource table and call-scope rollback remain the semantic implementation.
The reference contract is the [Component Model Canonical ABI](https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md).

The public `turbowasm_component_host_value` enum/union gains record, tuple,
variant, option, result, enum, flags, own and borrow. Record and tuple items use
the declared type order. Variant/option/result contain a case index and optional
payload pointer; validation rejects inconsistent payload presence and invalid
case indices. Enum carries its case index. Flags use a counted `uint32_t` word
array, with unused high bits rejected. All lengths, recursive traversal and
allocation sizes are bounded by declared types and configured Runtime limits;
no arbitrary map or raw representation integer serves as a resource value.

An own value contains an opaque, uniquely owned host resource handle. Its private
state records the originating live instance, nominal resource identity and the
abstract representation. Returned resources retain that instance, its decoded
component and capability owner until transfer or release. Callers must not copy
own handles with struct assignment; nested own values follow the same rule.
Nominal checks include instance provenance, so a matching numeric type index in
an unrelated instance cannot admit a handle. Legitimate linked identities must
be resolved through the existing instance graph, never structural equality.

The public operations are:

| Operation | Contract |
| --- | --- |
| Existing `turbowasm_component_instance_invoke` and `turbowasm_component_call_create` | Keep const, non-consuming arguments. Accept composites and borrow values; reject any own argument before lowering. |
| New `turbowasm_component_instance_invoke_move` | Same outputs/status/trap parameters as invoke, with mutable host-value arguments. Consume own leaves only after successful admission; other input storage remains caller-owned. |
| New `turbowasm_component_call_create_move` | Same parameters as call_create, with mutable arguments. Successful creation transfers own leaves to the retained call; later resume never repeats that transfer. |
| New `turbowasm_component_host_value_borrow(const own*, borrow*)` | Create a non-owning view of one live own handle; reject non-own, moved or invalid sources. The source must outlive the view's use at call admission. |
| `turbowasm_component_host_value_destroy` returns `turbowasm_status` | Recursively release returned storage/resources, clear consumed values, and report a destructor failure rather than silently discard it. Existing scalar/string/list callers may continue ignoring its always-successful normal return. |

The move transaction has three phases: validate the complete argument tree and
nominal identities; prepare canonical handles and all bounded bookkeeping; commit
ownership and clear the caller's own leaves. Failure before commit leaves those
leaves owned by the caller and rolls back prepared handles. Guest realloc may
already have run during preparation; guest memory and external side effects are
not promised to roll back. After commit, execution failure does not return
ownership to the caller. A not-yet-started restartable call owns its transferred
resources and must release them if destroyed before execution.

Borrow admission pins the originating owner for the entire call, including fuel
yields and host waits. Moving or destroying an own value with active loans
returns INVALID_ARGUMENT without changing it. A call retains the instance, so
releasing the public instance handle cannot leave a suspended call dangling.
Terminal completion, trap, cancellation and call destruction each end the borrow
scope exactly once. Borrow values cannot escape as results where the declared
Component function type forbids them.

Canonical handles have one instance-owned table, including imported resources.
Provider handles are opaque representations in that table, never guest handles.
Resource aliases resolve to the original local definition before choosing a
codec or destructor. A borrow returned to its defining instance lowers directly
to its representation; a foreign borrow creates a transient table entry that
must be dropped before the call returns. Imported callbacks acquire table loans
and release them after completion or host-wait unwinding. This replaces the old
provider-handle passthrough that could not express canonical borrow ownership.
Direct capability APIs keep their documented retryable drop operations;
canonical destruction instead consumes the logical provider handle before its
callback and reports failure without reviving that handle. A failed underlying
filesystem close remains owned by the filesystem layer for host recovery.

Destroy preflights the complete value tree for active loans before changing it.
Once destruction begins, each own handle is invalidated before invoking its
destructor, preventing reentrant double destruction. Cleanup continues through
the remaining children after a destructor failure and returns the first error;
the released value stays empty and is not retryable. No implicit guest callbacks
are added to instance teardown. Existing internal instance-owned resources remain
governed by the exec/capability teardown contract. Lift/allocation failures after
a resource has left a canonical table must release the lifted owner, including
when a later sibling conversion fails.

The model remains single-threaded per instance; retaining an instance is not a
thread-safety guarantee. Checked retain/lend counters fail before overflow.
Resource handles and borrow records use existing bounded table/call capacities
and Runtime allocation limits. Result conversion moves codec-owned storage when
its representation matches, otherwise uses bounded copies with one cleanup path.

Alternatives considered: retaining const arguments while silently consuming own
values violates the current contract; exposing raw reps loses nominal identity
and lifetime protection; duplicating owned resources invents unsupported cloning
semantics. Explicit move entry points keep existing non-consuming callers stable.
The cost is additional bookkeeping and retained instance lifetime for resources
and suspended calls. These are ownership costs, not claimed speed improvements.

Compatibility: enum additions and union growth require rebuilding Component
consumers; changing destroy's return type also affects stored function pointers.
Existing names, scalar/string/list behavior, synchronous result cardinality and
Core Runtime ABI remain unchanged. C/C++ headers, installed consumers, nested
composite round trips, wrong nominal identity, duplicate ownership, move rollback,
borrowed-owner release, destructor traps/reentry, cancellation and allocation
failure must be tested. Old source examples remain valid; ownership examples must
check destroy status. Rollback removes the added host kinds and move APIs together
with retained-owner bookkeeping; no persisted data format requires migration.
The imported-resource table routing and consuming canonical destructors must roll
back together; mixing raw provider handles with canonical handles is invalid.

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
                                        MIR helper lowering for shared/unshared memory and atomics
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
helper-backed GC MIR                    implemented
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
native GC lowering uses the invocation root source described above, qualified by
collection/quota/failure tests and complete Core 3.0 differential replay.

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

The implementation extends the existing MIR helper boundary to shared memory32
and memory64, including atomic load/store, RMW, compare-exchange, fence and
wait/notify. The retained atomic descriptor table defines operand/result types;
Runtime remains the only implementation of synchronization and waiter ownership.
Every executed instruction pays its checkpoint before any memory side effect.
Wait forwards the invocation's interruption callback without retaining native
register or backing pointers. A failed helper publishes its exact status/trap
and exits generated code before a result is committed.

Each instance and MIR backend has one execution owner. Distinct instances may
execute concurrently with distinct backends while importing the same shared
backing; sharing memory does not authorize concurrent mutation of one instance's
JIT state. This matches the upstream [MIR context threading contract](https://github.com/vnmakarov/mir/blob/master/MIR.md#mir-context).
Backends and instances are destroyed only after their workers join.
No new queue, allocator, lock order, waiter capacity or growth policy is added.
This costs a helper call per memory operation but shares bounds, locking, and
trap semantics with the Runtime. Public layouts and lifecycle contracts remain
unchanged. Resource limits remain the configured byte/page limits and host
address-space limit, not the width of a guest address. Validation covers
address typing, high offsets, arithmetic overflow, atomic alignment, growth
failure, imported backing, waits, and native/interpreter equivalence. Rollback
can restore feature admission without migrating stored data. Native regression
tests require compiled handles for both memory widths, concurrent imported atomic
increments, wait/notify across growth, alignment/bounds traps, interruption and
all atomic descriptor families. The semantics reference is the
[WebAssembly Threads execution specification](https://webassembly.github.io/threads/core/exec/instructions.html).

Reference: [Memory64 proposal](https://github.com/WebAssembly/memory64/blob/main/proposals/memory64/Overview.md).
