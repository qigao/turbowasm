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
  -> private retained interpreter/native frames for backend-neutral resumable execution

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
Resumable execution uses the tiered entry described below; native frame sources
remain registered while the owning coroutine is suspended.
Rollback removes reference admission and emission together with frame allocation
and root registration; it must not leave admitted code with untraced cells.

### Resumable native execution

The execution handle owns copied arguments, result storage, execution control and
one bounded Salts coroutine stack. The Runtime dispatcher selects the tier at
entry. Backends must separately opt into resumable execution; ordinary execution
control support alone does not certify native frame suspension. MIR uses the
same checkpoint and host-wait callbacks as the interpreter. Yield retains the
native return address, scalar registers, invocation-local vector slots, reference
cells and lexical EH frames; resume continues that callback without replaying an
instruction or host effect. Reconstructing frames in a second continuation engine
would duplicate Runtime control/ownership state and is unnecessary with the
existing stackful coroutine.

All operations remain on the execution/store owner thread. The instance, module,
linked providers and attached backend must outlive the execution, including its
suspended state; backend destruction is a quiescent control-plane operation.
The execution control owns every active frame's root registration. Those sources
borrow stack contexts and heap cells whose lifetime crosses yield, allowing store
collection between resumes. The existing stack size, call-depth, allocation and
store-root budgets bound retained storage; exhaustion keeps the existing explicit
error/trap semantics. No new queue, lock, allocator or unbounded continuation list
is introduced. Yield preserves state; only the next resume replaces fuel and
interrupt policy. Pending host-wait completion records status without executing
Wasm. Destroy resumes the suspended callback with INTERRUPTED and follows normal
native/interpreter cleanup before releasing the coroutine stack. Trap, exception
and allocation failure are terminal, not suspension reasons.

The installed signatures and handle layout do not change. Calls with an attached
resumable backend now participate in its existing per-function tiering policy;
without one the interpreter behavior remains. Tests require actual MIR compilation
and exercise repeated fuel/interruption, nested/mixed-tier host waits, vectors and
GC collection during suspension, EH, tail calls, cancellation and allocation
failure. Linux and macOS native CI qualify generated frames; Windows ASan checks
the common ownership path. Rollback restores the interpreter entry and removes
MIR's resumable opt-in together, without changing stored data or public handles.

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

### MIR exception boundary

Native `throw`, `throw_ref` and `try_table` share Runtime exception objects,
resolved tag identities and the instance's pending-exception state. A private
throw adapter creates the same payload representation as the interpreter. A
catch adapter tests clauses in binary order and commits a matched payload into
the invocation's rooted scratch before clearing the pending exception. No public
ABI, exception ownership or cross-instance propagation contract changes.

MIR emits one static handler chain per lexical try scope. Ordinary direct,
indirect and reference calls route only `EXCEPTION` to the nearest enclosing
chain; other errors keep their existing return path. Unmatched exceptions pass
to the outer chain or caller. Handler payloads use validated target signatures
and existing branch merge cells, including vectors/references. Tail calls leave
the current frame and its handlers before dispatch, matching Runtime tail
semantics. No dynamic handler stack or native unwinder is introduced.

The alternative native unwinder would add platform-specific lifecycle rules;
interpreting protected regions would leave the native-coverage gap unresolved.
Static handlers add bounded code and payload scratch derived from retained
validation metadata. Runtime remains the sole mutable exception owner and no
helper holds a lock or suspends. Tests cover all catch kinds, nesting/order,
rethrow/null throw, scalar/vector/reference payloads, imported tag identity,
mixed-tier direct/indirect/tail calls, collection, allocation failures and fuel.
Rollback removes EH admission/emission and its private adapters together.

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

## Component async tasks and endpoint ownership

The remaining async work follows the pinned [Component Model binary format](https://github.com/WebAssembly/component-model/blob/a25fc0b372dd21f07f0242c46e98bd0f1ea0c0e1/design/mvp/Binary.md)
and [Canonical ABI](https://github.com/WebAssembly/component-model/blob/a25fc0b372dd21f07f0242c46e98bd0f1ea0c0e1/design/mvp/CanonicalABI.md).
Runtime fuel/host-wait suspension alone does not implement Component tasks,
subtasks, waitable sets, callbacks, backpressure or future/stream endpoints.
The user approved this extension of the synchronous host-value boundary on
2026-10-08. Public integration remains under implementation.

The Component instance owns task admission, canonical handle tables, waitable
membership and a cooperative ready queue. All mutation and guest execution stays
on its existing execution owner thread. External I/O reports completion through
the existing host-wait bridge; it does not mutate Component state from another
thread. Runtime continues to own native/interpreted frames, fuel, interruption
and cancellation unwind. Component scheduling must preserve that control across
guest entry, callbacks, realloc, post-return and resource destruction.

Planned public additions are opaque task, endpoint and transfer owners; appended
host-value kinds for readable future/stream endpoints; an instance-options entry
point with explicit async limits; and create/resume/state/result/cancel-request/
destroy operations for tasks and endpoint transfers. Existing synchronous APIs
retain their contract. Async-typed exports use the task entry point. A host can
create a typed endpoint pair, transfer its readable end through a move call,
write through its writable end, and later close that end. Endpoint payload types
come from the loaded Component type graph, including composite and own values;
no parallel host type registry is introduced. A future accepts one value (or one
unit completion); a stream supports partial transfers and end-of-stream.

Readable endpoints are unique movable owners. Copying the C carrier does not
duplicate ownership; move admission preflights the entire value tree before
consuming anything. Endpoints and outstanding tasks/transfers retain the backing
instance. A transferring endpoint cannot move or drop until the operation has
completed or cancellation has been acknowledged. Payloads cannot contain borrow
values. Host write admission copies ordinary values and explicitly moves own or
endpoint leaves into transfer-owned storage; read results are privately owned
until published once. Guest memory is represented by instance plus checked
offset/length and is reacquired after reentry or suspension, never by a retained
raw address. Already transferred elements remain committed on partial completion
or cancellation; untransferred owned elements are returned or destroyed exactly
once according to the transfer's terminal result.

Task cancellation is a request with a later terminal acknowledgement, distinct
from Runtime's immediate execution unwind. Task/subtask state and cancellation
delivery follow the pinned ABI, including cancellation before entry and before
return. Borrow loans remain live through resolve-event delivery. Waitable sets
store membership and pending event state, not duplicated event payloads;
completion, cancellation and drop cannot publish a second terminal event.
Destroying a live public async owner reports a busy state without mutation;
the host requests cancellation, drives progress, takes/releases results and
then destroys it. An explicit instance shutdown operation rejects new admission,
requests cancellation, drains terminal obligations and reports completion only
after outstanding handles and loans are released. Guest traps preserve the
primary error while terminal cleanup releases host-owned storage.

Async options bound live tasks, canonical handles (including waitable sets and
endpoint ends), outstanding transfers and retained host payload bytes. Counts
and byte products are checked before allocation or ownership transfer. Defaults
are finite; options cannot request unlimited async storage. Exhaustion reports
`TURBOWASM_OUT_OF_MEMORY`, preserves unadmitted inputs and becomes
retryable after owners are released. Runtime allocation/stack budgets apply in
addition. Explicit and implicit guest backpressure use the same bounded pending
task state and preserve queued admission order; they do not create a worker pool
or an unbounded event queue. No new logging subsystem is required.

### Public async C interface

The approved host boundary is implemented in the installed
`turbowasm/component.h`. The declarations below describe that public interface.
The ordinary synchronous loader/constructor contract is preserved, with explicit
async admission. Private canonical
types, handle tables, buffers, task pointers and budget objects never appear in
the public signatures. The implementation continues to use the existing
Component owners and type graph as its authorities.

The alternatives considered were extending `component_call`, exporting the
private host-owner signatures, and introducing separate public async owners.
The separate owners are selected: `component_call` retains its synchronous
restartable-call contract, while task cancellation, callback scheduling and
transfer completion have distinct lifecycles. Exporting private signatures
would make callers own budgets and canonical buffers and would expose mutable
runtime state. Separate owners add a small public surface and require a joined
adapter, but keep those obligations inside Component. No executor, worker pool
or second type registry is added.

#### Common rules and bounded instance creation

All opaque owners start as `{0}`, have unique ownership and are moved by copying
the carrier and clearing the source, never by retaining a shallow copy. API
output cells and move input trees must be exclusive during the whole operation,
including allocator callbacks. Operations run on the instance execution owner
thread; external threads enqueue I/O completion to that thread. Inspection is
allocation-free and returns a snapshot, not a mutable internal view.

Except for documented terminal cleanup, failure preserves input ownership and
all output cells. `INVALID_ARGUMENT` covers an empty/stale owner, wrong role,
busy/reentrant operation or invalid limit; `TYPE_MISMATCH` covers a valid value
that does not match the declared Component type. `OUT_OF_MEMORY` covers finite
quota exhaustion as well as allocation failure. `YIELDED` means progress or
external completion remains pending. Existing `turbowasm_status` values are
reused; no new public busy status is required.

```c
typedef struct turbowasm_component_async_task { void *impl; }
    turbowasm_component_async_task;
typedef struct turbowasm_component_async_endpoint { void *impl; }
    turbowasm_component_async_endpoint;
typedef struct turbowasm_component_async_transfer { void *impl; }
    turbowasm_component_async_transfer;

typedef struct turbowasm_component_async_options {
    uint32_t tasks;
    uint32_t handles;
    uint32_t transfers;
    size_t host_bytes;
} turbowasm_component_async_options;

void turbowasm_component_async_options_init(
    turbowasm_component_async_options *options);

turbowasm_status turbowasm_component_load_async_borrowed(
    turbowasm_component *component, const uint8_t *bytes, size_t size);
turbowasm_status turbowasm_component_load_async_borrowed_with_config(
    turbowasm_component *component, const uint8_t *bytes, size_t size,
    const turbowasm_runtime_config *config);

turbowasm_status turbowasm_component_instance_create_async_with_options(
    turbowasm_component_instance *instance,
    const turbowasm_component *component,
    const turbowasm_component_async_options *options);
```

The loader outputs must be empty; source bytes are immutable and borrowed until
the last derived instance/task/transfer/value releases its reference. Config and
options are copied before allocator callbacks; allocator contexts remain
borrowed through the last release. A NULL config or options selects initialized
defaults. `async_options_init(NULL)` is a no-op. Defaults are 64 tasks, 4,096
canonical handles, 64 transfers and 16 MiB of logical host storage. Zero counts,
unlimited sentinels and values beyond the canonical representation are rejected.
Tasks/handles/transfers retain their quota until their actual retirement, not
merely until terminal notification. The constructor owns one stable byte budget;
callers neither pass nor mutate it.

Host storage covers async owner bodies, snapshots, copied payloads and retained result
allocations. Successful publication transfers each remaining reservation to
the returned allocation/resource/endpoint owner until its actual destruction or
subsequent move. It must not return the charge merely because a task or transfer
hands out a value. Runtime allocation and stack limits apply in addition. This
is implemented through private Runtime allocation finalizers for ordinary
returned string/composite/resource storage and direct endpoint-body ownership.
Each allocation retains the instance/budget and releases its charge after the
allocator's deallocation callback. Detached subtrees therefore remain charged
independently, without a public ownership field or parallel value registry.
Synchronous exports invoked on an async instance use the same finalizers for
their returned storage; invoking a synchronous export cannot bypass the retained
result quota. Existing synchronous instances retain their original Runtime
allocation contract. Whole-tree output charging commits before public delivery;
quota failure discards unpublished synchronous results through their actual
cleanup authority.

An async-capable instance may also run synchronous exports through the existing
sync APIs. Async-typed exports require `async_task`; the sync APIs return
`UNSUPPORTED` for them. The existing synchronous loader and constructor continue
to reject async components. The explicit loader makes that behavior stable and
allows rollback by withholding the new installed surface until its gates pass.

#### Type discovery and endpoint identity

Pair creation needs a type from the receiving instance, including a future or
stream nested in a function parameter/result. A graph index alone is not a
public type identity. A copyable token borrows its live instance; it carries no
cleanup obligation and cannot be used after that instance's backing state has
been released. All token consumers take the expected live instance and validate
the scope before interpreting its id. The token contents are opaque to callers.

```c
typedef struct turbowasm_component_type_token {
    const void *scope;
    uint32_t id;
    bool inline_type;
} turbowasm_component_type_token;

typedef enum turbowasm_component_type_edge {
    TURBOWASM_COMPONENT_TYPE_EDGE_ELEMENT,
    TURBOWASM_COMPONENT_TYPE_EDGE_FIELD,
    TURBOWASM_COMPONENT_TYPE_EDGE_CASE,
    TURBOWASM_COMPONENT_TYPE_EDGE_SOME,
    TURBOWASM_COMPONENT_TYPE_EDGE_OK,
    TURBOWASM_COMPONENT_TYPE_EDGE_ERROR,
    TURBOWASM_COMPONENT_TYPE_EDGE_PAYLOAD
} turbowasm_component_type_edge;

turbowasm_status turbowasm_component_instance_parameter_type(
    const turbowasm_component_instance *instance, turbowasm_name export_name,
    size_t parameter_index, turbowasm_component_type_token *out);
turbowasm_status turbowasm_component_instance_result_type(
    const turbowasm_component_instance *instance, turbowasm_name export_name,
    turbowasm_component_type_token *out);
turbowasm_status turbowasm_component_instance_type_child(
    const turbowasm_component_instance *instance,
    turbowasm_component_type_token parent, turbowasm_component_type_edge edge,
    size_t index, turbowasm_component_type_token *out);

typedef struct turbowasm_component_async_endpoint_type {
    bool future;
    bool has_payload;
    turbowasm_component_type_token payload;
} turbowasm_component_async_endpoint_type;

turbowasm_status turbowasm_component_instance_endpoint_type_get(
    const turbowasm_component_instance *instance,
    turbowasm_component_type_token type,
    turbowasm_component_async_endpoint_type *out);
```

`ELEMENT` selects a list element; `FIELD` selects an ordered record/tuple field;
`CASE` selects a variant's payload; `SOME`, `OK` and `ERROR` select option/result
payloads; `PAYLOAD` selects a future/stream payload. Only FIELD/CASE use a
nonzero index. A wrong edge/kind or absent payload/result returns
`TYPE_MISMATCH`; an out-of-range index or foreign token returns
`INVALID_ARGUMENT`. An unresolved function export returns `LINK_ERROR`.
Endpoint type inspection returns `TYPE_MISMATCH` for a non-endpoint type and a
zero payload token when `has_payload` is false. These allocation-free queries
project the existing type graph; they do not construct reflected metadata.

Resource identity remains generative and is validated at value admission.
Foreign endpoint values may move only when the existing structural type and
resource-identity checks admit them; a foreign token cannot create a pair in an
unrelated instance. Retain the pair's creation domain independently of the host
carrier's receiving instance. Public type queries and provider link adapters
must use the same instance type view, including its bound resource identities.

#### Task creation, progress and result delivery

```c
typedef enum turbowasm_component_async_wait_reason {
    TURBOWASM_COMPONENT_ASYNC_WAIT_NONE,
    TURBOWASM_COMPONENT_ASYNC_WAIT_FUEL,
    TURBOWASM_COMPONENT_ASYNC_WAIT_INTERRUPTION,
    TURBOWASM_COMPONENT_ASYNC_WAIT_HOST_IO,
    TURBOWASM_COMPONENT_ASYNC_WAIT_COMPONENT_EVENT,
    TURBOWASM_COMPONENT_ASYNC_WAIT_BACKPRESSURE,
    TURBOWASM_COMPONENT_ASYNC_WAIT_COOPERATIVE
} turbowasm_component_async_wait_reason;

typedef struct turbowasm_component_async_task_state {
    turbowasm_execution_state execution;
    turbowasm_component_async_wait_reason wait_reason;
    bool terminal;
    bool cancellation_requested;
    bool cancelled;
    bool result_taken;
    size_t result_count;
    turbowasm_status status;
    turbowasm_trap trap;
} turbowasm_component_async_task_state;

turbowasm_status turbowasm_component_async_task_create(
    turbowasm_component_async_task *task,
    turbowasm_component_instance *instance, turbowasm_name export_name,
    const turbowasm_component_host_value *arguments, size_t argument_count);
turbowasm_status turbowasm_component_async_task_create_move(
    turbowasm_component_async_task *task,
    turbowasm_component_instance *instance, turbowasm_name export_name,
    turbowasm_component_host_value *arguments, size_t argument_count);
turbowasm_status turbowasm_component_async_task_resume(
    turbowasm_component_async_task *task,
    const turbowasm_execution_options *options);
turbowasm_status turbowasm_component_async_task_state_get(
    const turbowasm_component_async_task *task,
    turbowasm_component_async_task_state *out);
turbowasm_status turbowasm_component_async_task_request_cancel(
    turbowasm_component_async_task *task);
turbowasm_status turbowasm_component_async_task_take_result(
    turbowasm_component_async_task *task,
    turbowasm_component_host_value *out, size_t *out_result_count);
turbowasm_status turbowasm_component_async_task_destroy(
    turbowasm_component_async_task *task);
```

Create admits only async-typed function exports, copies ordinary argument
storage and takes a snapshot before any later execution. The const entry rejects
all nested own/endpoint leaves. The move entry performs complete validation and
allocation before committing every own/endpoint leaf together; those leaves are
zeroed on success, while caller string/composite containers remain caller-owned.
Borrow arguments pin their source own through terminal resolution. Create does
not enter guest code; the caller may immediately reuse non-own input storage.

Resume drives one entry/callback quantum or the retained Core continuation.
Options are borrowed only for that resume; NULL means unlimited fuel/no
interruption callback, not an implicit blocking event loop. `YIELDED` preserves
the owner; state reports why it yielded. Internal waitable waits are distinct
from external host I/O. Callers drive other tasks/transfers as needed and resume
again when the relevant event is ready. Callback stacks, realloc, post-return
and destructor stacks obey the same execution control and cannot replay effects.

State `status` is `YIELDED` before terminal completion; afterwards it is the
final status, with acknowledged cancellation represented as `INTERRUPTED` and
`cancelled=true`. `result_count` is zero until successful terminal completion,
then 0 or 1 according to the signature, and stays observable after delivery.
`result_taken` tracks successful delivery, including a successful unit delivery.
Task.return/result resolution alone is insufficient: `terminal` requires
entry/callback exit and the associated loan/cleanup obligations to finish.

Cancel is an idempotent request on a live task, not acknowledgement. A terminal
task accepts it without changing its outcome. A result committed before
cancellation wins; merely requesting cancellation cannot discard that result.
Take-result returns `YIELDED` while nonterminal and the final error/cancellation
status after unsuccessful completion. On successful completion, an empty output
receives the single result once; a unit function allows out=NULL and writes count
0. Failure, including result allocation/quota exhaustion, leaves output/count
and canonical ownership unchanged for retry. Returned values use the existing
`host_value_destroy` contract, extended to endpoint leaves and charge retirement.

Destroy on NULL is invalid; on an empty carrier it succeeds. A live, driving or
reentrant task returns `INVALID_ARGUMENT` unchanged. A terminal destroy consumes
the owner and any untaken result even if cleanup returns an error; primary guest
failure precedes a cleanup failure. Callers inspect the cleared carrier to
distinguish consumed cleanup failure from a busy rejection. No raw private task
view is installed.

#### Authenticated external waits

```c
typedef struct turbowasm_component_async_wait {
    const void *owner;
    uint64_t generation;
    uint64_t continuation;
    turbowasm_host_wait wait;
} turbowasm_component_async_wait;

bool turbowasm_component_async_task_pending_host_wait(
    const turbowasm_component_async_task *task,
    turbowasm_component_async_wait *out);
turbowasm_status turbowasm_component_async_task_complete_host_wait(
    turbowasm_component_async_task *task,
    turbowasm_component_async_wait wait, int completion_status);
```

Tickets borrow the live task/instance, do not retain them, and are passed back
unchanged. Query returns false without touching out for internal Component waits,
no pending wait or reentrant access. Completion allocates nothing and enters no
guest code: it records completion for a subsequent resume. Authenticate owner,
admission generation, Core continuation generation and Runtime wait before any
mutation. A foreign/stale/duplicate ticket is invalid, including repeated Runtime
tokens across entry/callback continuations and reused task storage in the same
live domain. Never use a ticket after its owner/domain is destroyed. Shutdown
uses the same public ticket carrier but distinct owner authentication, so a task
ticket cannot complete a destructor wait. Completion must remain available after
shutdown closes admission, while the actual owner remains pending.

#### Endpoint owners and host values

```c
typedef struct turbowasm_component_async_endpoint_state {
    bool future;
    bool readable;
    bool has_payload;
    bool peer_dropped;
    bool finished;
} turbowasm_component_async_endpoint_state;

turbowasm_status turbowasm_component_async_endpoint_pair_create(
    turbowasm_component_async_endpoint *reader,
    turbowasm_component_async_endpoint *writer,
    turbowasm_component_instance *instance,
    turbowasm_component_type_token type);
turbowasm_status turbowasm_component_async_endpoint_state_get(
    const turbowasm_component_async_endpoint *endpoint,
    turbowasm_component_async_endpoint_state *out);
turbowasm_status turbowasm_component_async_endpoint_into_value(
    turbowasm_component_async_endpoint *reader,
    turbowasm_component_host_value *out);
turbowasm_status turbowasm_component_async_endpoint_from_value(
    turbowasm_component_async_endpoint *reader,
    turbowasm_component_host_value *source);
turbowasm_status turbowasm_component_async_endpoint_destroy(
    turbowasm_component_async_endpoint *endpoint);
```

Pair-create requires distinct empty outputs and an authenticated future/stream
token. Allocate/reserve both ends before publishing either. On success each end
retains its host instance, and the pair independently retains its creation
domain. Readable ends can be encoded as `HOST_FUTURE`/`HOST_STREAM`; writable
ends cannot be host values. Into-value moves an idle readable carrier to an
empty host-value output with its true kind. From-value authenticates that kind
and moves the embedded owner to an empty endpoint carrier. Neither operation
creates a canonical value or changes the endpoint's instance; both are
allocation-free and failure-atomic. Views of an embedded endpoint use its
address, e.g. `&value.as.future`, without shallow-copying its ownership.

State is a copy; `finished` is the future's one completed operation, not
end-of-stream. A stream reports peer drop separately and may be reused after
successful transfer completion. Destroy closes an idle end and notifies its
peer of drop (the writer's close is stream end-of-stream); empty succeeds,
NULL is invalid. A transferring carrier has been moved to its transfer and is
empty. A busy embedded endpoint blocks `host_value_destroy` before any sibling
string/resource/endpoint is consumed. Once complete preflight passes, freeze all
owned leaves through callbacks, release the whole tree despite destructor
errors, clear it, and report the first cleanup error. Public carrier operations
never expose raw canonical end handles.

#### Transfers and atomic terminal delivery

```c
typedef enum turbowasm_component_async_transfer_outcome {
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_PENDING,
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_COMPLETED,
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_PEER_DROPPED,
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_CANCELLED,
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_FAILED
} turbowasm_component_async_transfer_outcome;

typedef struct turbowasm_component_async_transfer_state {
    uint32_t length;
    uint32_t progress;
    bool readable;
    bool terminal;
    bool result_taken;
    turbowasm_component_async_transfer_outcome outcome;
    turbowasm_status status;
} turbowasm_component_async_transfer_state;

typedef struct turbowasm_component_async_transfer_result {
    turbowasm_component_async_endpoint endpoint;
    turbowasm_component_host_value values;
    uint32_t first_index;
    uint32_t logical_count;
} turbowasm_component_async_transfer_result;

turbowasm_status turbowasm_component_async_transfer_read(
    turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_endpoint *reader,
    uint32_t count, size_t payload_bytes);
turbowasm_status turbowasm_component_async_transfer_write(
    turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_endpoint *writer,
    const turbowasm_component_host_value *values, uint32_t count);
turbowasm_status turbowasm_component_async_transfer_write_move(
    turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_endpoint *writer,
    turbowasm_component_host_value *values, uint32_t count);
turbowasm_status turbowasm_component_async_transfer_poll(
    turbowasm_component_async_transfer *transfer);
turbowasm_status turbowasm_component_async_transfer_state_get(
    const turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_transfer_state *out);
turbowasm_status turbowasm_component_async_transfer_request_cancel(
    turbowasm_component_async_transfer *transfer);
turbowasm_status turbowasm_component_async_transfer_take_result(
    turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_transfer_result *out);
turbowasm_status turbowasm_component_async_transfer_result_destroy(
    turbowasm_component_async_transfer_result *result);
turbowasm_status turbowasm_component_async_transfer_destroy(
    turbowasm_component_async_transfer *transfer);
```

All three admission calls explicitly consume the appropriate endpoint on
success; they preserve it on failure. Write additionally snapshots ordinary
values; only write-move consumes nested own/endpoint leaves. Borrow leaves are
not transfer payloads. All source trees and endpoint identities are checked,
all storage reserved and all fallible staging completed before the whole move
commit. If rendezvous subsequently fails, return an admitted transfer that owns
its terminal failure, rather than an admission error that falsely promises the
now-consumed inputs are intact. A copied source root never becomes caller-owned
canonical storage. Drivers and retained guest buffers remain internal.

Read `payload_bytes` reserves a finite cumulative payload allowance beyond
separately charged top-level cells. It may be zero for scalar/unit data; SIZE_MAX
is invalid. Limits/checked sizing reject impossible reservations before move.
When incoming owned payload exceeds the allowance, terminate the operation with
the real error, keep its committed prefix, and clean up any rejected staging
according to the canonical copy contract. A future requires count=1. A unit
future/stream accepts values=NULL and counts logical completions without payload
cells. Streams allow count=0 with canonical readiness semantics; count must fit
the packed canonical copy length. Closing the writer, not a zero-length write,
signals end-of-stream.

Poll acknowledges a pending endpoint event at most once and frees its buffer
lease; it does not run arbitrary guest tasks or guest realloc. Peer tasks must
be driven separately, using their Runtime execution budgets. Pending returns
`YIELDED`; a successfully terminal completed/dropped/cancelled operation returns
OK, with its distinct outcome in state. A failed copy returns its primary status.
Later polls return the recorded terminal status without consuming a second event.
State is observational and cannot acknowledge an event. Cancel is idempotent and
requires later polling to confirm terminal completion. Never free a leased
buffer or infer cancellation acknowledgement from an OK cancel-request.

Take-result requires terminal event acknowledgement and an empty result struct.
It atomically publishes the endpoint plus the complete value batch once:

| Direction | first_index | logical_count | values for a payload-bearing type |
| --- | --- | --- | --- |
| Read | 0 | progress | Owned HOST_LIST of all received values |
| Write | progress | length - progress | Owned HOST_LIST of the entire untransferred tail |

The outer HOST_LIST is a batch envelope, not the declared element's Component
list type. For a unit endpoint values is empty (kind 0), even if logical_count
is positive. For payload-bearing endpoints a zero-length batch is an owned empty
HOST_LIST. The returned endpoint is the original read/write role, including a
completed future that can only be closed. `take_result` may succeed for a failed
operation so callers can reclaim its prefix/tail and endpoint; operation failure
remains in transfer state. It returns the delivery error only if publication
itself fails. Allocation/quota failure changes neither endpoint nor batch nor
output and permits retry. A second take is invalid. No borrowed canonical-cell
array is exposed. Already transferred elements are never returned in a write
tail or transferred a second time.

The result struct owns its nonempty fields until callers move them onward or
call result-destroy. That helper preflights endpoint and all value leaves
together; busy rejection preserves the whole struct. After preflight it consumes
both fields, clears the struct and reports the first cleanup error. A terminal
transfer may also be destroyed without taking a result; it destroys the tail or
received values and closes its endpoint exactly once. Live/driving destruction
fails unchanged. Terminal destruction consumes the transfer even when returning
the saved copy error or a cleanup error. The transfer's reservation is retired or
handed off to the returned owners, never duplicated.

#### Shutdown and orderly release

```c
typedef struct turbowasm_component_async_shutdown_state {
    bool requested;
    bool complete;
    turbowasm_component_async_wait_reason wait_reason;
    turbowasm_status status;
} turbowasm_component_async_shutdown_state;

turbowasm_status turbowasm_component_instance_request_shutdown(
    turbowasm_component_instance *instance);
turbowasm_status turbowasm_component_instance_poll_shutdown(
    turbowasm_component_instance *instance,
    const turbowasm_execution_options *options);
turbowasm_status turbowasm_component_instance_shutdown_state_get(
    const turbowasm_component_instance *instance,
    turbowasm_component_async_shutdown_state *out);
bool turbowasm_component_instance_shutdown_pending_host_wait(
    const turbowasm_component_instance *instance,
    turbowasm_component_async_wait *out);
turbowasm_status turbowasm_component_instance_shutdown_complete_host_wait(
    turbowasm_component_instance *instance,
    turbowasm_component_async_wait wait, int completion_status);
```

Request closes admission before requesting registered cancellation; repeated
requests are idempotent and an error after closure never reopens admission.
New tasks, endpoint pairs and transfers are rejected. Existing tasks may resume,
external waits may complete, transfers may poll/take, and all values/owners may
be released. Result promotion during shutdown is delivery, not new admission.

Poll requires a prior request. It drains local guest handles and retained cleanup
with the supplied fresh execution budget, but does not consume host result
carriers or acknowledge host transfer events. It returns `YIELDED` while host
owners, loans, endpoints or destructor execution remain pending. State reports
HOST_IO for an authenticated destructor wait, FUEL/INTERRUPTION for Runtime
suspension, and COMPONENT_EVENT when external owner retirement is required.
Pending status is YIELDED; the first saved cleanup error is returned only after
all obligations drain, with complete=true. Startup allocation failure returns
its error without losing handles and is retryable. Complete polling is
idempotent and returns the saved terminal status.

Keep the public instance handle until shutdown completes. Existing void
instance-destroy remains a reference release; it cannot imply cancellation/drain.
If explicit shutdown has been requested but is incomplete, it preserves the
carrier as the current private contract does. After completion, destroy releases
it. Releasing the public handle without requesting shutdown is permitted when
all remaining owners retain the instance, but does not perform cooperative
guest cleanup; applications that need orderly teardown use this protocol.

#### Example: pass a future to an async export

The following sketches the API sequence for an async export `consume` whose
parameter 0 is `future<u32>`. It is design documentation, not an executable
fixture. Every actual application must check each API result; this example shows
the admitted path after loading and constructing an async instance.

```c
turbowasm_component_type_token type = {0};
turbowasm_component_async_endpoint reader = {0}, writer = {0};
turbowasm_component_async_task task = {0};
turbowasm_component_async_transfer write = {0};
turbowasm_component_async_transfer_result tail = {0};
turbowasm_component_host_value argument = {0}, answer = {0};
turbowasm_component_host_value value = {
    .kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = 42
};
size_t result_count;
/* 'instance' is a live turbowasm_component_instance. */
turbowasm_name consume = {(const uint8_t *)"consume", 7};

/* Check OK after each admission; failure leaves its sources untouched. */
turbowasm_component_instance_parameter_type(&instance, consume, 0, &type);
turbowasm_component_async_endpoint_pair_create(&reader, &writer, &instance, type);
turbowasm_component_async_endpoint_into_value(&reader, &argument);
turbowasm_component_async_task_create_move(&task, &instance, consume, &argument, 1);
turbowasm_component_async_transfer_write(&write, &writer, &value, 1);

/* The application's owner-thread loop interleaves task_resume and transfer_poll.
 * It handles task host I/O tickets and supplies fresh fuel for each resume.
 * When both owners report terminal, reclaim their results: */
turbowasm_component_async_transfer_take_result(&write, &tail);
turbowasm_component_async_task_take_result(&task, &answer, &result_count);
turbowasm_component_async_transfer_result_destroy(&tail);
turbowasm_component_host_value_destroy(&answer);
turbowasm_component_async_transfer_destroy(&write);
turbowasm_component_async_task_destroy(&task);

/* Request shutdown, drive/cancel/release remaining owners, complete any
 * destructor host-wait, and poll until shutdown_state.complete is true.
 * Then release the public instance and component handles. */
```

#### Implementation gates, migration and qualification

The shape above is additive and follows the previously approved lifecycle
extension. Component consumers recompile against the expanded host-value
surface; existing synchronous function signatures and ownership behavior remain.
Do not install unimplemented declarations or lift public loader gates ahead of
the complete joined surface. This document is reviewable while implementation
continues. Revert the new adapter/header additions and retain the explicit async
gate if qualification fails; no serialized data migration is introduced.

The joined implementation includes authenticated public type queries and
state snapshots; all-or-nothing host task/transfer value conversion; reservation
handoff through returned string/composite/resource/endpoint destruction; atomic
transfer result delivery; public wait-ticket adapters; and options/shutdown
adapters. Provider linking and nested Component instantiation remain required
parts of the wider completion goal: they must share this typed ownership/wait
boundary and the same generative resource view, not expose private imports or
replace missing providers with successful stubs. WASI-specific constructors must
eventually compose their capability owners with the async options/lifecycle.

The joined declarations are in `include/turbowasm/component.h`; adapters reuse
the private owner bodies. Windows ASan qualification passes the formal host
task, argument/result and options/adapter targets, including actual memory32/64
guest echoes of mixed string/own/future values, detached-string charging, typed
and unit futures, partial transfers, repeated cancellation and exhaustive
composite write admission allocation failures. The result handoff now keeps
returned allocations charged until destruction. Async tasks use the transfer
type predicate; synchronous calls retain their existing type gate. A composite
transfer test exposed stale admission references after result promotion; payload
handoff now retires those references before the canonical cells can be freed.
Public wait/shutdown suspension tests exercise actual imported I/O, stale
continuations and delayed destructor failure. The installed C/C++ tests pass
16/16 and exercise public typed endpoint round trips, tasks, transfer batches,
shutdown and C++ linkage/layout. Commit `86561c4` passes all five
[native CI profiles](https://github.com/qigao/turbowasm/actions/runs/37764949070):
Windows 172/172 plus 16 installed tests, Linux MIR 213/213 in 3.30 s and macOS
MIR 213/213 in 3.67 s. Android builds the full configured graph and installed
consumers without a host execution claim. This qualifies the installed host
surface; nested providers and WASI-specific async constructors remain separate.
Commit `407bb4e` also passes all five
[native CI profiles](https://github.com/qigao/turbowasm/actions/runs/37765881571),
including the synchronous-result quota fix: Windows 172/172 in 2.58 s plus
16 installed tests in 0.14 s, Linux MIR 213/213 in 3.31 s and macOS MIR 213/213
in 3.98 s. Local Windows ASan passes 172 regular tests and both pinned Core 3
conformance suites; installed consumers pass 16/16. The final focused suites
pass 107 host-task, 26 options/adapter, 58 argument/result and 11 shutdown-wait
cases, covering exact synchronous composite byte limits and detached storage.
Post-admission copy failures are
reported by the accepted transfer, preserving its received prefix and unsent tail.

The release gate is formal behavioral coverage of scalar/composite/own/borrow
task arguments, nested future/stream values, memory32/memory64, unit operations,
partial progress, zero-length stream readiness, cancellation before entry and
after return, busy whole-tree destroy, stale/foreign wait tickets, quota boundaries
and allocation failure at every staging point. Returned values must survive task,
transfer and public instance-handle release while remaining charged; returning or
destroying a prefix/tail must drop each owned leaf once. Exercise shutdown with
guest destructor fuel/I/O suspension and outstanding host results. Qualify the
same behavior under interpreted execution, Windows ASan and Linux/macOS MIR,
then run existing installed C/C++ consumer tests against the actual new public
surface. Green private-owner tests alone do not open the loader gate.

### Internal adapters and qualification history

The following notes record the individual private integration stages and their
named qualification commits. Earlier closed-gate statements refer to those
stages; the current installed host boundary and remaining provider/WASI scope
are described in the public interface section above.

The private instance-options path separates task, canonical-handle, transfer and
host-byte limits. Defaults are 64 tasks, 4,096 canonical handles, 64 retained
transfers and 16 MiB of logical host owner/payload storage, in addition to Runtime
allocation and stack limits. Zero limits, maximum-value unlimited sentinels and
handle counts beyond the packed canonical representation are invalid. Options
are copied before instantiation; callers need not retain or stabilize them.
Task/domain and canonical-table counters remain their existing authorities;
transfer count remains held through terminal delivery until transfer destruction.
The instance owns one stable host-byte budget used by parameter snapshots,
results, host task/endpoint bodies and transfer storage. All admitted owners
retain the instance, so that budget outlives every reservation. Options-backed
instances reject a substitute caller budget before allocation or move; legacy
private constructors preserve their explicit test-budget contract. No public
option or async loader is exposed until host values and public owners are joined.
The constructor retains the immutable Component binary/config before its first
allocation, so an allocator may release the loader carrier without invalidating
instantiation. The options snapshot also precedes allocator callbacks. Formal
`component_async_options_test` cases cover independent task/handle/transfer
exhaustion, exact byte boundaries, retryable result promotion, foreign-budget
rejection across every host admission, deferred string storage after public
carriers close, first-allocation loader closure and complete allocation rollback
for constructors, tasks, endpoint pairs and transfers. Windows ASan passes 15
cases and 64,155 assertions; related Component/WASI/Runtime regression passes
88/88 in 10.54 s. The MIR variant requires actual compiled task bodies.
Commit `3883644` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37744041741):
Linux MIR 213/213 in 3.22 s and macOS MIR 213/213 in 2.88 s, including both
interpreted and compiled instance-options suites. This qualifies private quota
ownership and constructor retention before the public adapters above were joined.

Task host-wait tickets borrow the stable task and its live instance domain. They
carry both the domain-assigned task generation and the generation of the current
Core execution, in addition to Runtime's host-wait token. Each callback creates
a new Core execution, whose Runtime wait generations may repeat; task storage
may also be reused after destruction. Neither repetition authenticates an old
completion. Domain task generations and per-task Core generations never wrap;
exhaustion fails before task admission or consuming a callback event/cancellation.
Tickets are valid only during the lifetime of their instance domain and must not
be carried across destruction/reinitialization of that domain. They do not retain
owners or allocate storage. Query and completion run on the instance owner
thread, execute no guest code, preserve outputs on failure, and reject reentry,
foreign tasks and Component builtin waits. Only the task driver completes builtin
waits. External host I/O retains its existing completion/cancellation authority;
requesting Component cancellation does not acknowledge or complete that I/O.
The public wait adapter above uses this authentication and the same retained
host values, endpoint/transfer ownership and task state.
Formal `component_task_test` cases exercise entry-to-callback and repeated-callback
Runtime token collisions, sibling tasks, reused task storage, repeated waits in
one execution, duplicate completion, reentry, builtin isolation and nonwrapping
admission/callback exhaustion. `component_host_task_test` adds real imported
host I/O through retained root/callback owners, with shutdown requests and closed
public carriers. Windows ASan passes 73 task cases (8,545 assertions) and 95 host
owner cases (65,152 assertions); related Component/WASI/Runtime regression passes
88/88 in 8.79 s. The existing MIR variants require actual compiled Core entries
and callbacks. Commit `70293a3` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37752379838)
with restored Salts 3.0.0: Linux MIR 213/213 in 1.88 s, macOS MIR 213/213 in
4.09 s, and Windows 172 main tests in 3.20 s plus 16 installed-package tests in
0.20 s. This qualifies task/host-owner wait authentication, including real
compiled callback suspension before the joined public surface was installed.

Future/stream host-value leaves carry an opaque readable endpoint owner by value.
The carrier is unique: copying it does not create ownership. Argument admission
requires an explicit move and shares the complete value tree's existing commit
flag. Preparation freezes each source; any later conversion/type/allocation
failure destroys the proxies and restores every source. Whole-tree commit clears
all owned source leaves without callbacks; borrowed resource loans retain their
existing lifetime. Endpoint records and admission entries count against the
snapshot budget, while endpoint bodies keep their original instance/budget.
Results allocate new endpoint bodies before touching canonical leaves. Staging
failure frees only those wrappers. Successful promotion transfers the body byte
reservation from result staging to the new endpoint owners; it survives result
delivery and returns only when the endpoint is moved/published or destroyed.
The source and destination trees are exclusive throughout admission/promotion.
Host-value destruction preflights and freezes every owned leaf before releasing
any; a busy endpoint rejects the whole destruction unchanged. Explicit public
async loading uses this complete task/transfer host-value boundary.

The private endpoint argument adapter now separates preparation, admission and
guest publication. Preparation allocates an authentic canonical owner record,
freezes the source host body and holds an instance activity. The enclosing
argument snapshot accounts for the record bytes; the existing host body retains
its budget reservation and instance reference. Preparation failure preserves the
source and output, including allocator reentry that invalidates the admission
flag. The admission owns that flag and its stable lifetime; after whole-tree
preflight it changes once from false to true and clears all prepared source
carriers without allocation or callbacks. Commit returns preparation activity;
the deferred canonical input remains an idle registered cancellation root, so
it does not prevent instance shutdown requests before guest entry.

Unadmitted proxy destruction restores source ownership without closing the
reader. Admitted unpublished destruction closes it once and retires the host
body. Taking an admitted tail restores direct endpoint ownership and retires
the old body, permitting allocation-first promotion into another host owner.
Taking or publishing unadmitted values is rejected; canonical lower rollback
removes reservations and preserves both the canonical proxy and its host source.
Guest publication first commits every endpoint handle, then invokes host cleanup
through a bounded intrusive notification chain. Allocation-free begin hooks
establish every receiver cleanup guard before the first cleanup callback; close
also establishes its guard before the creation instance can release pair storage.
Retired owner records defer
release while notification is active, so cleanup reentry may destroy the current
carrier or later carriers without freeing a pending notification. Host cleanup
uses an activity guard or an already active shutdown/activity guard. Instance
and original type-graph keepalives survive public carrier closure and publication.
The existing immediate private `into_value` path retains its contract.
Formal host-task and endpoint-codec suites cover prepare/commit/rollback, duplicate
adoption, publication before admission, whole-codec publication with cleanup
reentry, cancellation cleanup, tail-promotion OOM retry, preparation allocation
failure, invalidated admission flags, public-handle closure and foreign creation
instance teardown with receiver allocator reentry. Windows ASan passes 90
host-task cases (61,398 assertions) and 18 endpoint-codec cases (743 assertions);
related Component/WASI/Runtime regression passes 88/88 in 10.67 s.
Commit `89fa006` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37748449762)
with the newly published Salts 3.0.0 SDK: Linux MIR 213/213 in 1.96 s,
macOS MIR 213/213 in 4.03 s, and Windows 172 tests plus 16 installed-package
tests. The MIR suites include actual compiled task-body assertions. The new
address assertions explicitly use TinyTest's `const void *` trait on every
compiler; they do not rely on implicit pointer-to-integer conversion.
This supplies transactional endpoint ownership for the remaining whole-tree
host-value conversion, now joined to public async values and opaque owners.

Alternatives rejected: treating a fuel yield as async completion loses task
state; detached threads violate instance affinity; implicitly cloning endpoints
breaks unique ownership; unbounded buffering hides backpressure. The chosen
model costs retained instances and bounded task/transfer bookkeeping. There is
no performance claim. Host-value additions require Component consumers to
rebuild; old synchronous entry points and Core Runtime ABI remain stable. There
is no persisted-data migration. Before publication, rollback removes async
entry points and restores explicit rejection of async binaries together with
their task/endpoint machinery; it must never leave accepted partial execution.

Implementation order is private type validation, canonical signatures/decoding,
task and waitable state, endpoint transfers, then public host integration and
installed C/C++ consumers. Private future/stream type metadata can be tested
before runtime integration; the synchronous feature predicate and binary loader
continue to reject them until their complete execution path is available.

Deferred host argument admission owns a private snapshot before a task can wait
behind backpressure. The existing public-value converter copies strings and
composite storage in this mode; synchronous calls keep their borrowing contract.
A shared owner-thread byte budget accounts for the snapshot header, canonical
cells, resource-admission records and copied UTF-8 bytes. Checked size preflight
reserves the entire amount before allocation or resource reservation. Zero and
unlimited budgets are invalid; exhaustion returns OUT_OF_MEMORY without changing
input owners. Runtime allocator limits additionally bound allocation overhead.
Preparation retains the instance and reserves own/borrow leaves without consuming
them. Explicit commit clears moved host cells only after all admission succeeds;
canonical publication transfers own obligations to the callee. Borrow loans and
snapshot bytes remain retained until terminal delivery. Failure before commit
unreserves inputs; cancellation after commit but before publication destroys owned
resources exactly once. Destruction releases all obligations and reports its first
cleanup error. No source pointer is used after commit. This private admission
boundary precedes public task/endpoint integration and does not open async loading.
The host-argument suite exercises delayed canonical execution after caller storage
changes, nested composites, empty admissions, shared byte exhaustion and reuse,
overflow, cyclic inputs, instance retention, own/borrow rollback and publication,
destructor failure, and every snapshot allocation failure. Windows ASan passes
17 cases and 11,831 assertions; the related Component/WASI/Runtime graph passes
85/85 in 7.84 s. Public task/endpoint integration remains a separate step;
these initial snapshot tests do not claim an available public async execution API.
Commit `19b5dde` passed all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37718126233):
Linux MIR 206/206 in 2.52 s and macOS MIR 206/206 in 2.51 s, including the
host-argument suite on both platforms. The suite uses the existing synchronous
canonical execution path to verify snapshot lifetime; compiled async integration
continues to be covered separately by the Runtime task/import suites.

Deferred admissions can adopt their resource leaves into the existing async
resource codec. Each adopted owner carries its instantiated identity and a
borrowed pointer to the admission's commit state; lowering before commit is
invalid. The codec reserves/publishes the same canonical handles used for
guest-to-guest calls. An infallible publication hook forgets the original host
owner immediately after the codec commits the handle, so the same rep can return
as a new result without appearing twice in the host resource list. A finalizer
releases the original owner on cancellation, with no second destructor after
publication. Codec storage stays retained through terminal delivery.
Host borrow loans stay with the admission until terminal delivery; they never
create a fictitious guest lender handle. Adopted storage is charged during the
initial byte preflight and additionally consumes the exec's resource-owner quota.
Partial adoption failure destroys temporary codec owners and unreserves original
inputs. Snapshot destruction preflights every adopted leaf: an outstanding lower
reservation returns INVALID_ARGUMENT without changing storage, loans or quota.
All mutations and finalizers run on the instance owner thread. The admission
outlives every codec borrow, and its canonical cells cannot be moved independently.
Public task/endpoint owners will enforce that lifetime at their boundary.
The same suite now covers adopted own publication, canonical own results, local
host borrows, generative identity rejection across instances, byte/owner quota
exhaustion, busy destruction without mutation, nested cleanup failures, every
adoption/retained-instance allocation failure, cancellation before entry under
backpressure, real host-wait completion/cancellation and forced unwind before
borrow release. Windows ASan passes 33 cases and 42,724 assertions; related
Component/WASI/Runtime regression passes 85/85 in 9.60 s. Its MIR registration
attaches real backends and requires compiled canonical callers, resource consumers
and defining destructors. Public async task/endpoint/transfer owners and shutdown
remain to be integrated; the ordinary loader stays gated.
The host argument resource bridge is qualified by all five native CI jobs at
[`7297810`](https://github.com/qigao/turbowasm/actions/runs/37720891458): Linux
MIR 207/207 in 1.74 s and macOS MIR 207/207 in 3.11 s, including compiled host
argument callers, consumers and defining destructors. Inline Core export bags
have no executable instance and are skipped when attaching test backends.

Async result promotion stages the existing host value tree before changing the
canonical result. Allocation, byte quota, type or resource validation failure
leaves that result available for retry. Fresh canonical own leaves are wrapped
with their original release obligations and retain the public instance; adopted
argument proxies, borrowed leaves, published owners and active lower reservations
cannot become independent results. Publication of the staged tree moves strings
and resources without another allocation. A private result owner charges its
tree, bytes and resource wrappers to the shared owner-thread budget until take
or destruction; take transfers ordinary host-value destruction obligations.
Destroying a wrapped resource uses its canonical release authority. Moving it
back into a guest detaches that authority only after canonical publication, so
publication, rollback and cancellation cannot execute a second destructor.
The private result owner retains the instance through all cleanup, reports the
first destructor failure, and releases remaining leaves and byte reservations.
The host-argument suite covers real retained task own results after host waits,
result survival through task/admission cleanup, canonical publication and rollback
of promoted own values, cancellation before publication, host borrow retention,
destructor failure after public handles close, exact byte boundaries, all nested
conversion/wrapper allocation failures, duplicate owners, busy lower reservations,
wrong nominal types and instances, malformed budgets and storage overflow.
Windows ASan passes 48 cases and 56,209 assertions; the related
Component/WASI/Runtime graph passes 85/85 in 9.39 s. Resource and composite
result promotion remains an internal implementation behind the public task and
transfer adapters; future/stream leaves use the endpoint owner described above.
Commit `73e1137` passes all five [native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37722176124):
Linux MIR 207/207 in 1.94 s and macOS MIR 207/207 in 2.73 s, including the
host result cases under interpreted and compiled callers/resource destructors.

The private host task owner joins export lookup, deferred argument admission and
terminal result promotion. Creation validates the async lift and reserves task
and byte capacity before consuming host own inputs. A stable per-task parameter
codec and copied realloc context run on the existing retained Runtime stack;
realloc uses that task's execution control. Lower reservations roll back before
any argument storage is released. Backpressure postpones preparation, while
cancellation remains a request until the task's terminal acknowledgement.
Host result delivery requires both canonical resolution and completed Core
execution. Promotion failure retains the canonical result for retry; unit
results are delivered once with count zero. Borrowed input storage remains held
through delivery. Destroy rejects live or reentrant owners without mutation,
then releases the Runtime task before parameters/results and instance retention.
Cleanup continues after destructor errors and preserves the primary guest error.
This owner implements the opaque public async task. The ordinary synchronous
loader and instance constructor preserve their original async rejection gates.
The formal host-task fixture covers memory32/64 strings, indirect parameters,
own round trips, borrowed input retention, callback cancellation, noncancellable
yield, post-return traps, quota reuse, allocation rollback, interruption-poll
reentry and destructor failure. A later string realloc trap rolls back an earlier
own reservation without publishing or dropping it twice. Result byte-quota
exhaustion leaves output/count intact and retries after another task releases its
reservation, without changing the budget limit. Windows ASan passes 22 cases and
10,651 assertions; related Component/WASI/Runtime regression passes 86/86 in
8.67 s. The MIR variant requires compiled export/callback and resource destructor
bodies. Commit `dc5ec11` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37724147564):
Linux MIR 209/209 in 2.91 s and macOS MIR 209/209 in 3.74 s.

Host-created endpoint pairs use the task domain's existing stable pair storage
and pair quota. A private factory accepts the instantiated type graph and either
publishes both guest handles or leaves both ends host-owned. Only successful
admission changes its empty output pointers. A failed allocation or registration
does not retain half a pair; fully closed earlier pairs may be collected. Moving
a readable end through the canonical codec preserves the domain-owned storage
until both ends close. This factory borrows its domain and immutable graph; the
eventual host endpoint owner must retain the corresponding instance across
operations and cross-instance publication. It does not enable public endpoint
values or async loading on its own.
Formal endpoint builtin cases exhaust host storage with zero occupied handles,
reject guest creation at that shared quota, verify invalid/allocation-failed
admission leaves outputs empty, reuse closed storage, and move a host reader
through the canonical codec into actual Core memory32/64 reads while its host
writer completes the rendezvous. Windows ASan passes 19 cases and 3,507
assertions; the related regression passes 86/86 in 9.15 s. Commit `91f7958`
passes all five [native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37724771608):
Linux MIR 209/209 in 3.02 s and macOS MIR 209/209 in 2.82 s, including compiled
Core memory32/64 rendezvous with the host writer.

Private async instances bind pair keepalive hooks to their existing reference
count. Each admitted pair acquires one instance reference before allocation;
allocation/registration failure returns it. The pair retains that creation
instance while either end is open, regardless of current handle table or host
value ownership. Close and forward notify the stable pair only after ownership
and peer state commit. Closing the second end returns the reference once; it may
destroy an otherwise unreferenced instance and invalidate both closed endpoint
pointers. Other live pairs keep the instance retained. Collection frees only
fully closed storage and does not return the reference again. Plain private
domains without hooks preserve their caller-owned graph/domain contract. The
public async shutdown step must still drain local guest ends and obligations;
pair keepalive does not substitute for that protocol.
Formal host-task cases exercise host/guest stream/future creation, allocation
rollback, reference overflow, multiple-pair retention, forwarding after public
handles close, guest-created packed handles and foreign publication followed by
actual Core reads and drops. The foreign reader verifies the payload in guest
memory as well as completion; the final foreign drop releases the original
instance. Windows ASan passes 31 cases and 17,555 assertions; the related
Component/WASI/Runtime regression passes 86/86 in 9.72 s. Commit `acad04c`
passes all five [native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37726052086):
Linux MIR 209/209 in 3.10 s and macOS MIR 209/209 in 2.78 s.

The private host endpoint owner holds an instance reference and a byte-charged
opaque body in addition to the creation pair's keepalive. Pair creation stages
both host bodies and the domain pair before publishing either owner; failure
returns every reservation. Its graph/type comes from the instance. A readable
owner moves into a canonical value only after that value's allocation succeeds;
success empties the owner and returns its host byte charge. The stable pair then
keeps the creation instance alive. Promotion of a fresh canonical readable end
requires a retained domain pair, stages the host body and retains the receiving
instance before taking the value. Failure preserves the value. Close rejects an
active operation without changing the owner and frees idle owners exactly once.
Owners and output/source cells are exclusively borrowed during transitions;
allocator callbacks cannot modify those cells. Read-only endpoint views cannot
be used to close or move the underlying end. Host-value kind integration and
transfer-owned payload storage remain the next boundary; this internal owner
does not open public async loading.
Internal submit/take/cancel retain the host body while the existing endpoint
engine borrows a stable canonical buffer through event delivery. A retained task
may drive guest realloc; there is no separate transfer scheduler. Reentrant
owner mutations are rejected throughout allocation/conversion and views are
unavailable while driving. Formal cases cover stream/future creation, all pair
allocations, exact byte quota, canonical allocation/promotion retry, reservation
and unretained-storage rejection, cancellation acknowledgement before busy
destruction, allocator reentry and independent creation/receiving instance
lifetimes. Foreign Core reads now rendezvous through the retained host writer
owner. Windows ASan passes 40 cases and 22,869 assertions; the related regression
passes 86/86 in 10.33 s. Commit `a7323f6` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37727239239):
Linux MIR 209/209 in 3.07 s and macOS MIR 209/209 in 2.91 s, including the
retained host writer under compiled foreign readers and drops.

Fresh canonical resource values have an independent creation-instance keepalive
in public async domains. A payload can leave a task or endpoint before host-result
promotion, so its release authority cannot borrow that producer's lifetime.
Lift retains before allocation and ownership extraction, returning the reference
on either failure without consuming the handle. Canonical release drops the
resource or lender, updates its owner count and frees the owner record before
returning the keepalive; that last release may destroy the exec and its Core
providers. Committed lower records keep this reference until record cleanup.
Host argument proxies keep their existing admission-owned lifetime. Raw private
execs without both domain hooks retain their explicit borrowed-owner contract.
The existing canonical owner quota bounds these references, and its record size
is included by the existing resource admission/result byte accounting. Transfer
storage and public async integration remain pending.
Formal cases cover sole canonical ownership after both public handles close,
sibling cleanup and actual compiled destructors after a trap, whole-tree cleanup,
allocator reentry closing the public handles, allocation/reference exhaustion,
failed ownership extraction under a lender, borrow release and committed lower
record cleanup. The resource suite passes 56 cases and 63,262 assertions on
Windows ASan; the related Component/WASI/Runtime regression passes 86/86 in
9.58 s. Commit `b789a98` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37728726163):
Linux MIR 209/209 in 2.36 s and macOS MIR 209/209 in 3.50 s, including actual
compiled resource destruction after both public handles close.

The private transfer boundary owns one host endpoint and stable canonical
cells, on the existing instance owner thread. Canonical write inputs are already
unique Runtime-owned trees; successful admission moves them and the endpoint,
while failed admission restores both without cleanup of caller-owned values.
The public host-value snapshot/move bridge is a separate remaining integration
step. A transfer retains the receiving instance and its endpoint's creation
instance independently of the endpoint body, preserving the original type graph
after both ends close or a returned endpoint owner is destroyed. It
reserves its body, cells and payload bytes before allocation. Read admission
reserves an explicit finite payload capacity. A host receive hook checks the
entire rendezvous batch against that capacity before moving any destination
cells. Existing Runtime budgets govern guest lift/copy scratch separately.
Fresh resource and nested endpoint leaves retain their own release authorities;
borrow leaves and values without retained provenance cannot enter this boundary.
The instance's finite transfer count is initially bounded by its async handle
limit, with a separate field for the approved public async options integration.
Endpoint/source/output cells are exclusively borrowed during admission, including
allocator callbacks. The endpoint moves into the stable transfer only after
allocation succeeds; reentry is rejected throughout allocation and copying.
The existing endpoint engine remains the progress and event fact source. Poll
releases the buffer borrow on either success or a copy error and records terminal
status exactly once. Cancellation does not release storage until poll delivers
its acknowledgement. After terminal delivery, the endpoint can move back to an
empty host owner; borrowed result/unsent-value views remain valid until transfer
destruction. Destruction releases every retained cell, returning the first
cleanup failure unless a primary copy error already exists. Live destruction
preserves the entire transfer. Public result publication, shutdown and ordinary
host-value conversion remain gated until their full contracts are integrated.
Formal host-owner cases cover both admission orders for streams/futures, unit
and zero-length transfers, cumulative partial progress, cancellation before
destruction, string/own/nested-future batches, whole-batch and cumulative byte
quota failures, finite transfer count, every allocation rollback, exact byte
capacity, invalid provenance and reentry from a peer's guest lift. Real guest
string writers exercise memory32/memory64 across fuel-suspended realloc, and
compiled foreign readers consume transfer-owned host writes. Independent
creation/receiving graph retention survives returning and closing the endpoint
and both public instances. Windows ASan passes 54 cases and 35,658 assertions;
related Component/WASI/Runtime regression passes 86/86 in 8.48 s. This qualifies
the private canonical storage boundary; public host snapshots/result publication,
explicit public options and shutdown remain required.
Commit `4347cd0` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37731018209):
Linux MIR 209/209 in 1.81 s and macOS MIR 209/209 in 2.74 s, including compiled
guest string writers for both memory widths and foreign Core readers of owned
host transfers.

The private shutdown-request transition now closes new host admission before
requesting cancellation. Already bounded task, endpoint and transfer bodies
embed non-owning intrusive registrations in their receiving instance; moving an
endpoint into a transfer replaces its registration, and taking it back restores
the endpoint registration. No extra queue, allocation or ownership authority is
introduced. All operations remain on the instance owner thread. An activity
counter covers half-built admissions, result allocation and cleanup callbacks;
live owner/Runtime guards also exclude shutdown from retained Core execution or
peer-driven copying. A rejected reentrant request leaves admission unchanged.
The finite cancellation walk excludes owner mutation and skips completed or
resolved tasks, previous cancellation requests and endpoints outside copying.
It neither delivers host events nor frees carriers, buffers, results or loans.
The same owner must drive task/IO progress, poll acknowledgements and explicitly
release storage. New tasks, pairs, transfers and synchronous calls are rejected
before consuming inputs, allocating or changing result outputs. Existing calls,
terminal result allocation and retained endpoint ownership moves remain usable
after admission closes. A successful request proves only this first transition;
local guest-handle drain, completion auditing, retained public result/options
integration and the public shutdown operation remain pending.
Formal host-owner cases qualify unchanged move inputs and output cells after
admission closes, repeated cancellation of multiple roots, partial transfer
acknowledgements and unsent tails, caller-buffer leases, endpoint registration
across moves, allocator/peer/Core reentry rejection and failed-admission activity
rollback. Callback tasks acknowledge cancellation through their actual callback;
an exclusive fuel-suspended callback accepts a request without releasing its
Core continuation, and noncancellable yields continue to a real return. A fuel-suspended resolved task
keeps its eventual primary Core trap, and existing synchronous calls and terminal
results remain deliverable. Windows ASan passes 66 cases and 43,328 assertions;
the related Component/WASI/Runtime regression passes 86/86 in 8.59 s.
Commit `486bb85` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37733404437):
Linux MIR 209/209 in 3.17 s and macOS MIR 209/209 in 4.77 s, including actual
compiled callback cancellation and post-return trap handling across fuel
suspension. This qualifies shutdown request/admission closure, not instance
drain or the full public async boundary.

Private drain extends that transition on the instance owner thread. Ordinary
argument/result staging and synchronous call bodies are passive registrations:
their lifetime blocks drain without changing their cancellation semantics.
Before starting a pass, all host registrations, canonical owners, imported calls
and guest tasks must be gone. A pass scans at most the initial finite handle
capacity, closing local guest ends and dropping sets after membership clears.
Buffer leases attached to those guest ends are cancelled and acknowledged within
the pass; the aggregate buffer-owner count must reach zero for completion, but
does not prevent the pass from releasing its own local leases.
Outstanding lends, borrowed resources, reservations and independently owned
subtasks stay pending; drain does not counterfeit their release. Resources are
removed before their defining destructor runs, so a failed destructor consumes
that obligation once, records the first error and permits sibling cleanup.
When Core is present, one internal unit host-entry task runs the pass using the
existing coroutine/control path. It retains the instance, graph and pass state
through fuel/host-wait suspension; its storage is bounded to one per instance
and subject to Runtime allocation/stack limits. Core-free graphs use the same
finite scan with their synchronous capability destructor contract. Admission
remains closed through allocation failure or interruption, and startup failure
before the scan leaves handles retryable. Handles still present after the finite
scan are handled by a later pass rather than an unbounded loop. Completion requires no
host owners, calls, canonical owners, loans, local handles or live creation
pairs. A foreign end therefore keeps its origin pending until it is released.
The first consumed cleanup failure is reported at terminal completion. Core
storage remains with the instance until normal instance destruction; no pending
owner is freed merely to make completion succeed. Destroying a requested but
incomplete private instance preserves its public carrier so that the caller can
still drive and audit completion. This remains a private
boundary until public options and value/endpoint integration are complete.
Shutdown host-wait integration uses the existing imported synchronous capability
route inside the retained cleanup task. Import descriptors are copied by exec;
their callback context remains borrowed through instance destruction. Only the
instance owner thread may query or complete a suspended cleanup wait. Callbacks,
allocator reentry and active progress reject these operations unchanged. Wait
completion records the adapter's status without resuming guest code, reopening
admission or releasing the cleanup driver. The next poll supplies a fresh fuel
and interruption budget and continues the original destructor stack.

The private wait ticket identifies both its borrowed instance and cleanup-pass
generation, in addition to Runtime's execution-local wait generation/token.
Each successfully created cleanup driver advances the instance's pass counter;
counter exhaustion traps before consuming a handle. This rejects foreign-instance
and prior-pass completion even if the adapter reuses a token and a new Runtime
execution restarts its wait generation. Within a pass, Runtime remains the sole
authority for stale, mismatched and repeated wait completion. Failed queries
preserve outputs, and completion never allocates. The caller retains its instance
carrier throughout shutdown; tickets do not independently retain storage.
Formal drain cases cover retained guest destructors across fuel and cooperative
interruption, sibling cleanup after a trap, every cleanup-driver startup
allocation failure, callback/allocator reentry, outstanding lends and borrowed
handles, unpublished reservations, active set waits, passive argument/result/call
owners, foreign creation pairs and Core-free resource definitions. Actual guest
string writes for memory32 and memory64 leave codec leases after the calling
task exits; shutdown cancels, acknowledges and releases both leases before
completion. Windows ASan passes 81 cases and 55,422 assertions; the related
Component/WASI/Runtime regression passes 86/86 in 20.20 s. The formal
`component_shutdown_host_wait_test` extends this with real imported destructor
waits, no replay while incomplete, allocation-free completion, fresh fuel and
cooperative interruption, callback/allocator reentry, post-wait traps with sibling
cleanup, matching Runtime tokens on foreign instances and later cleanup passes,
pass-counter exhaustion and every retained-import constructor allocation failure.
Windows ASan passes 10 cases and 9,142 assertions, and the related regression
passes 87/87 in 8.71 s. Nested Component provider integration remains unqualified.
Commit `8bac9a9` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37737071175):
Linux MIR 209/209 in 2.55 s and macOS MIR 209/209 in 4.02 s, including compiled
guest destructor suspension and memory32/memory64 codec-lease shutdown. This
qualifies private drain; the public async boundary remains closed.
Commit `b36a8cc` passes all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37741983427):
Linux MIR 211/211 in 2.78 s and macOS MIR 211/211 in 3.60 s. Both execute the
new shutdown host-wait suite under interpreted and compiled destructor bodies,
including a real nested guest resource.new that requires a second cleanup pass.
This qualifies the private imported host-wait boundary; it does not open public
async loading or qualify nested Component instantiation.

Private host-task execution reuses Runtime's resumable coroutine, execution
control and host-wait generation checks. An internal host-entry execution borrows
an existing Core instance for its allocator/store and caller memory; it constructs
the ordinary host-call view on that retained stack. No synthetic Wasm trampoline
or separate coroutine scheduler is introduced. Host code may wait through the
existing bridge and invoke Core with the same fuel, interruption and depth
accounting. Returning YIELDED directly is invalid: suspension must preserve the
callback stack. Forced destruction resumes the wait with INTERRUPTED and requires
the callback to unwind its temporary owners.

A private Component host-task binding has a stackful host entry instead of a
Core function/callback pair. Its preparation runs once after backpressure clears;
the host explicitly returns a typed canonical result or acknowledges delivered
cancellation through the existing task boundary. Successful entry exit without
either terminal action traps. Cancel-before-entry runs no callback, while an
external I/O wait remains owned by that adapter until real completion. The task,
binding context and instance remain stable through Core exit and caller delivery.
These internal entries do not open public async loading or expose a partial host
API; import routing and public owners integrate with the same task machinery.
`component_task_test` covers host-entry backpressure, one-time preparation,
generation-checked I/O waits, cancel-before-entry and explicit cancellation
acknowledgement, destruction before/after result publication, nested Core fuel
and interruption, exception-to-trap conversion, invalid callback exits and every
Runtime allocation failure. The Windows ASan task suite passes 64 cases and
7,581 assertions. Commit `f12a535` passed the complete Windows ASan graph
(170/170 including both Core 3 conformance suites, 232.09 s) and all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37713987970).
Linux MIR passed 205/205 in 2.94 s and macOS MIR 205/205 in 2.78 s, including
the same host-task tests with nested Core calls compiled by MIR.

Private async canonical calls can target these host entries directly. Argument
preparation lifts caller memory into retained canonical values without allocating
callee guest storage or publishing temporary guest handles. Host entries move
consumed values explicitly and stop accessing their arguments before resolving
the task; unmoved values and caller loans are released at terminal delivery or
failure teardown. Result lowering still uses the caller memory and atomic
resource/endpoint publication transaction, including guest realloc suspension.
Host bindings reject a guest parameter transaction because no such destination
exists. This call boundary is internal; public owners remain to be connected.
The Windows ASan async-call suite passes 33 cases and
2,811 assertions, covering indirect arguments, both caller memory widths, host
and result-realloc suspension, own/borrow/future movement, rollback, forced
teardown and every Runtime allocation failure. The related regression graph
passes 84/84 in 7.74 s. Commit `7f60a88` passed all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37714492087):
Linux MIR 205/205 in 2.89 s and macOS MIR 205/205 in 3.31 s, including
the host canonical-call suite with compiled callers and result realloc.

Private async capability imports select exactly one registered capability set
using the existing nominal `can_bind` router. A set supplies either an initialized
Component provider resolver or a stackful host callback for its async functions;
specifying both is invalid, and multiple claiming sets remain a link error.
Synchronous functions continue to use the separate synchronous invoke callback.
The host callback receives the instance-specific function graph, mutable retained
canonical arguments and the callee task. It moves values by clearing consumed
argument cells, explicitly resolves/cancels the task, and stops using arguments
before resolution. Callback/context ownership remains with the capability owner,
which outlives the exec and all retained calls.

Host calls use the consumer's bounded task domain and existing async-call FIFO
for ownership and quota accounting. They are marked as imports: the consumer's
guest-entry backpressure does not block an outbound host call. Ordinary guest
tasks and standalone host-domain tasks keep their existing admission gate.
Each frame copies the initiating Core instance carrier into stable frame storage
before the caller can suspend or exit; the borrowed host-call view itself is never
retained. The exec owns the underlying Core instances through frame retirement.
Per-call result codecs and realloc control stay on the host task continuation.
Quota failures precede argument lifting, failed admission leaves inputs intact,
and teardown unwinds host waits/result conversion before releasing values and
caller loans. No extra scheduler, synthetic Wasm function, or public partial API
is introduced. A single resolver with an implicit host fallback was rejected:
provider resolution failures must keep their original error semantics.
The Windows ASan import suite passes 28 cases and 3,047 assertions, and the
resource-import suite passes 30 cases and 7,902 assertions. They cover mixed
host/provider routing, ambiguous claims, callback errors, real I/O cancellation,
early caller exit, caller-backpressure isolation, nested result realloc control,
resource identity/loans, forced teardown and per-call allocation rollback. The
related Component/WASI/Runtime regression graph passes 84/84 in 9.03 s.
Commit `b9a5e23` passed all five
[native CI jobs](https://github.com/qigao/turbowasm/actions/runs/37716084457):
Linux MIR 205/205 in 2.56 s and macOS MIR 205/205 in 3.67 s. Both run the
host-import and imported-resource suites with compiled callers, result realloc
and defining-instance destructors.
Qualification must cover scalar/composite/own payloads, unit futures, nested
endpoints, memory32/64, partial transfers, all cancellation phases, event ordering,
backpressure, capacity exhaustion, allocation failure, traps and exactly-once
cleanup under interpreted and Linux/macOS MIR execution. WASI 0.2 streams remain
their existing resource interfaces; Component streams do not silently replace
those contracts.

Private canonical signature calculation distinguishes synchronous, stackful async,
and callback async ABIs. Async lowers permit four direct parameter carriers,
append a result-memory address for any non-unit result, and return an i32 subtask
word. Async lifts retain the sixteen-parameter limit, returning either no Core
values or one i32 callback control word. Their actual payload is passed separately
to task.return, with sixteen direct carriers or one memory address above
that limit. Memory64 changes address carriers but never endpoint/subtask/control
words. The existing synchronous adapter uses the same calculation in sync mode.
Signature outputs are published only on success. This calculation does not admit
async function types or canonical options; decoding, option validation, tasks and
execution remain separate integration gates.

Staged async decoding uses a private metadata-only entry point into the existing
binary parser, allocator and type graph. It retains async function flags,
future/stream payload references and async/callback canonical options, including
instance-local type aliases. Source bytes remain borrowed and graph/option arrays
remain decoder-owned; failure releases all metadata and resets the output.
Ordinary loading keeps its explicit async rejection. The execution constructor
also rejects every metadata-only object before allocating or running Core start
functions, so internal parsing cannot accidentally expose partial execution.
Option syntax, duplicate/conflict rules and index bounds belong to decoding;
actual Core callback/realloc signatures and runtime capabilities belong to
instantiation. This temporary private gate is removed only with complete async
task, canonical builtin and lifetime integration, followed by end-to-end tests.

The private decoder also owns async canonical builtin descriptors in the existing
Runtime-budgeted metadata arrays. Each successful definition consumes one Core
function index; parsing failure destroys the entire metadata object. Descriptors
retain type/result references, canonical options and context immediates, without
allocating handles or executing guest code. The decoder checks endpoint kinds,
memory/realloc requirements, duplicate options, immediate bounds and a uniform
component-wide context width. Signature calculation receives the resolved memory
address width: stream copy results follow that width, while future copy and
waitable event/control results remain i32. Unit copies without memory use i32
addresses. task.return reuses the sixteen-carrier flattening rule and requires
memory for dynamic or indirect results. Instantiation must still resolve Core
memory/realloc signatures and connect these descriptors to task/endpoint owners.
Error-context and explicit-thread proposal builtins remain unsupported by this
decoder. This metadata boundary has no additional threads, callbacks or queues.

The private async task runner owns one resumable Core execution and an optional
lifted result at a stable caller-provided address. Its domain borrows the shared
canonical handle table and may_leave flag, limits live tasks explicitly, and
tracks active/exclusive tasks on the execution owner thread. The driver selects
pending tasks; explicit backpressure delays argument preparation, so
cancellation before start cannot consume arguments. The preparation callback is
the canonical adapter boundary and runs once after admission; it owns rollback
of any argument conversion failure. Core execution still owns fuel, interruption
and host-wait continuations. No extra worker or unbounded queue is introduced.

Callback tasks retain the exclusive execution slot across Core suspension, but
release it between callback turns. WAIT pins its set until event delivery,
cancellation or teardown; YIELD gives control back to the driver. Task resolution
is distinct from thread exit: a result can be taken after task.return even while
guest cleanup continues. Cancellation is requested once, delivered as an event,
and acknowledged by task.cancel; a result may win the race. Outstanding borrowed
handles prevent either resolution. Destroy unwinds the Core callback first,
releases a pinned wait, destroys any untaken result, and removes the task from
its domain. Domains, graphs, instances, canonical codec contexts and prepared
argument owners outlive all such tasks. The private instantiation/lowering path
below selects this runner. Public retained-instance task wrappers remain
integration work.

Decoded task/context/backpressure/yield/waitable-set builtins now have a private
Core host binding. Binding checks the resolved memory address width and derives
the exact host signature from the decoded descriptor. Blocking set waits and
thread.yield use Runtime host-wait continuations; the task driver checks readiness
without consuming an event, then resumes that same callback. A set wait owns one
pin in its retained callback frame and releases it on delivery or unwind. It
does not turn cooperative task cancellation into external I/O completion.
Callback WAIT and builtin WAIT have separate pins and continuation state.

Guest-created sets use domain-owned stable storage and the existing canonical
table quota. Drop unregisters before freeing; domain teardown frees remaining
empty unpinned owned sets after tasks are gone. A nonempty or externally pinned
set must be emptied/released before teardown can finish. Thread-local context
slots live in the current task across Core and callback suspension. Event output
uses the canonical ordering: consume the event, then store its two u32 payloads,
so a second-store trap may leave the first store visible. task.return compares
resolved memory object identity through imports, not Core instance/index pairs.
Endpoint builtin bindings remain outside this private binding slice;
public Component loading/execution retains its existing async admission gate.

The private subtask adapter connects a caller-owned subtask to a callee task on
the same execution owner thread. The subtask borrows the caller's handle table,
argument preparation context and result destination; task admission and handle
publication retain their existing finite quotas. Preparation publishes STARTED
only when the callee actually starts. A resolution hook converts/moves the result
before committing RETURNED; cancellation commits only after the callee's explicit
acknowledgement. Resolution detaches the callee's hooks, allowing the subtask to
be dropped while the callee continues executing after task.return. Failed or
destroyed unresolved callees detach with a sticky error rather than fabricate a
successful/cancelled event. Loan cleanup runs once at terminal event delivery,
or during failed-owner teardown after the callee has detached. An exclusive
waitable pin spans cancellation callbacks and synchronous cancellation waits.
These internal hooks do not change existing synchronous or public task APIs.
The initial packed word suppresses a redundant STARTED notification; start and
return before polling coalesce into one terminal event. Eager results use no
handle. Deferred publication shares table allocation/quota failure semantics.
Subtask cancel/drop descriptors bind through the same host adapter. Synchronous
cancel waits retain their exclusive pin in the actual Core host frame; async
cancel returns BLOCKED when acknowledgement is pending. A ready callee callback
runs one eager quantum with the initiating Core caller's remaining fuel and
interrupt check, charging fuel back before returning. Subsequent scheduling uses
the callee's own resumable execution and never retains the caller's control
pointer. These tests exercise the internal ownership adapter; automatic canonical
argument/result wiring and retained public Component owners remain gated work.

Async argument preparation must execute inside the callee's retained Core
coroutine. The runtime reserves a correctly typed argument array before starting,
then invokes one private preparation hook before the guest entry. The hook fills
that array only after conversion succeeds; intermediate values remain owned by
the preparation frame across realloc fuel/interruption/host-wait suspension.
Preparation and guest entry share one execution control and unwind path. Core
allocation failure therefore cannot consume caller arguments. Cancellation before
start skips preparation; cancellation after preparation begins remains cooperative.
The hook is private, cannot change the function's argument count/types, and is
never replayed when the execution resumes. Existing public Core invocation APIs
and synchronous Component adapters retain their current entry semantics.

The private async canonical call frame owns one callee task/subtask pair, a
snapshot of at most four direct argument carriers plus the result address, and
the lifted parameter values. It borrows both immutable type graphs, memories,
domains and codec contexts. Structural/nominal type agreement and exact carrier
admission precede task creation. Parameter memory is read only by the retained
preparation hook; all source values are lifted before lowering into the callee.
The caller's four-carrier limit and callee's sixteen-carrier limit independently
select direct or aligned tuple representations through the existing codecs.
Parameter and result handle lowering use explicit per-call commit/rollback
hooks. Commits transfer owners while preserving lender cleanup until terminal
delivery; failures roll back staged destination handles before value cleanup.
The frame creates no queue or worker and shares the callee task/table quotas.
Its enclosing execution owner supplies stable storage and drives/resolves each
frame before reclaiming it. Binary instantiation/public task ownership will
select these frames only after the remaining async bindings are complete.
Initial nested progress saves/restores the domain's active task and inherits an
actual Core caller's fuel/interruption control, when present. Plain unbudgeted
Core invocations need no control object. Result conversion can suspend in guest
realloc just like parameter conversion; private destruction must unwind this
resolve frame before abandoning the caller, while rejecting active reentry.

Endpoint builtin bindings use the existing typed pair/copy engine. A task domain
owns a bounded list of pairs created by stream/future.new; its table quota also
bounds retained pair storage after readable ends move to another instance. Closed
pairs are reclaimed on domain collection, and domain destruction rejects any
still-live or transferred end. Each endpoint embeds one stable guest buffer
descriptor, matching the existing one-operation invariant. Async calls retain
this descriptor until event delivery; they do not retain a builtin stack frame.
Synchronous copies pin the endpoint before conversion callbacks can reenter, then
wait on the task's retained Core stack. Unwind cancels and delivers the private
copy before releasing its memory borrow; destruction is rejected if a peer is inside
conversion. Canonical resource/endpoint transactions and guest realloc contexts
remain borrowed from the binding and must outlive all pending operations.

Private async instantiation reuses the existing Core module/instance and alias
resolver. The exec owns one task domain, stable builtin bindings and async lift
bindings; tasks borrow this owner and register against its explicit task quota.
Sharing this resolver keeps function indices, memory identity and rollback under
one owner rather than introducing a second instantiation path.
The shared canonical table has an explicit handle quota. Async exports are
resolved to validated task bindings, including callback and memory32/memory64
options. Argument preparation remains invocation-owned and runs through the
existing retained task hook. Sync invocation rejects async exports.

Builtin memory options are resolved only after their provider instance exists,
before linking the consuming Core module. Realloc borrows the active task's
execution control, saves and clears its context slots and restores them when the
retained auxiliary invocation returns or unwinds. It forbids leaving the
Component during that invocation. An auxiliary owner excludes other tasks in
the domain across suspension until context restoration. No transient host-call
pointer is retained.
This stage admits same-domain task/endpoint execution and local async lowering;
the private import extension below adds retained cross-instance calls and
task-scoped resource borrowing. Local resource/endpoint call transactions
are described below. The private entry
does not change public loading or host API admission.

Exec destruction rejects registered tasks and live async table entries before
reclaiming memory. Transferred endpoints also retain their domain-owned pair and
prevent destruction even after their handle leaves the local table. The caller
must drain/drop these owners and retry; no forced cancellation or silent reclaim
is performed. Constructor failure has no active task and rolls back all allocated
bindings, Core instances and tables in the existing cleanup path.

Automatic local async lowering uses the existing async-call frame and one
instance-owned intrusive ready list. The task quota also bounds retained call
frames, including returned callees that have not exited. Eager, fully delivered
and exited calls are reclaimed inside the lower boundary without running cleanup
callbacks; pending calls retain their raw arguments, memories and stable options.
The driver polls a bounded number of call quanta in FIFO order with an explicit
per-quantum execution budget. A running frame is removed from the ready list while
it can create nested calls, preventing list traversal from retaining pointers
that nested collection could free. No worker or implicit unbounded queue exists.

Successful frames retire only after Core exit and terminal delivery plus guest
subtask drop (or eager delivery without a handle). Failed published calls retain
their error for the waiting caller; unpublished failed calls unwind during owner
collection. A private failure-abort entry propagates a non-success reason,
rejects active/pinned conversion and releases remaining calls after exported
caller tasks have been destroyed. It is not public cooperative cancellation. Exec destruction requires
the call list drained. Local and imported async lifts share this progress list;
the private import extension below keeps provider tasks in their own domains.

Local async endpoint parameters and results use two independent endpoint codecs
owned by each retained call frame. The caller-to-callee and callee-to-caller
directions reserve generation-checked handles under the instance handle quota;
only successful conversion commits them. Failure, including unwinding a suspended
guest realloc, rolls back reservations before releasing the owning values. Source
handles already consumed by lifting stay invalid on failure. This is owner-thread
state; no codec with pending lower reservations is shared between calls. A separate
instance-owned lift-only codec lets task.return consume endpoints without retaining
a transient host-call context. Ownership-bearing stream/future payload conversion
uses the per-operation transactions described below; the public async gate stays closed.

Local async resource conversion retains one owner record per lifted own/borrow
value, bounded by the exec handle quota even while values are outside the table.
These records keep the exec alive. Own lifting consumes the source handle; borrow
lifting acquires a lender that is released only at terminal subtask delivery or
failure teardown. Lowering a borrow back into its defining instance passes the
representation directly, as required by the canonical ABI, so no synthetic callee
borrow handle is created. Imported/cross-instance borrowing uses the task-bound
handle accounting described below.

Each conversion direction owns an intrusive reservation list. Own destinations
initially occupy private reservation slots, inaccessible to resource.rep/drop or
waitable operations. Commit preflights all resource reservations, commits the
endpoint scope, then publishes the already-validated resource slots without
allocation or callbacks. Rollback removes every reservation before value cleanup;
consumed source owners are destroyed, not silently restored. Cleanup propagates
destructor failure, consumes the logical owner once, and never retries guest code.

Resource destructors execute as synchronous auxiliary calls with fresh context
slots. A live async task supplies fuel/interruption control and remains the
progress owner across budget suspension; teardown/external release uses a
non-resumable synchronous call. Auxiliary depth rejects task.return and blocking
waits so a destructor cannot resolve or suspend its caller's async task. Nested
destructors save/restore the enclosing context and auxiliary owner. Explicit async
lowering from this synchronous destructor context remains gated until sync/async
cross-call integration is complete.

Ownership-bearing stream/future read/write buffers use the same combined resource
and endpoint codec as async calls, with one separately allocated context per
admitted endpoint operation. Context count is bounded by the exec handle quota;
allocation failure leaves the endpoint and source values unchanged. The builtin
prepares the context, then transfers its cleanup obligation to the endpoint's
stable guest-buffer copy only on successful submission. Failed admission frees it
immediately. Event/error delivery or cancellation releases it after any conversion
has finished; a peer suspended in realloc keeps both endpoints guarded.

All resource/endpoint reservations in a rendezvous batch commit together. Failed
lowering rolls them back before temporary values are destroyed. Pending operations
on the same builtin never share a mutable reservation list. The context keeps its
exec alive until release, and finalization performs only non-suspending rollback
and storage release. Host endpoints can exchange local resource/endpoint values;
same-Component nonnumeric guest-to-guest copies retain their specified trap.
Cross-instance resource identity and borrow cleanup extend these transactions below.

Private async instance imports reuse the existing unambiguous capability router.
An optional resolver selects an already initialized provider exec and async lift;
instantiation validates the complete signature and retains the provider until the
consumer is destroyed. Call frames stay on the consumer's bounded progress list,
while the callee task counts against the provider's task quota. Drivers explicitly
poll each exec with outgoing calls; no recursive scheduler or worker is introduced.
Resource leaves require bound instance identities. Borrow parameters use callee
task scopes; results and endpoint payloads reject borrows. Supported canonical
values use the existing per-direction transaction codecs.

Result realloc needs the callee's retained execution control while entering the
consumer's memory and builtin context. Each call therefore owns a copy of the
result realloc context with an explicit progress task. Both domains retain the
auxiliary guard through suspension; context builtins select this auxiliary task,
whose fresh slots are restored on success or unwind. The consumer's may_leave
guard prevents ordinary canonical reentry during realloc. Abort first unwinds
exported caller tasks, then their outgoing calls; provider teardown refuses live
import bindings and inbound tasks. Public async admission remains closed while
host ownership and general synchronous interop are completed.

Private async resource identity is generative per exec. A decoded binary's
numeric resource IDs describe declaration/alias relationships, not runtime type
identity. Instantiation therefore creates an immutable view when resources are
present: it copies type nodes and nested instance-type headers, borrows immutable
field/parameter/export/name arrays from the decoded owner, and binds each
resource equivalence class to an exec-owned stable identity token. Declaration
IDs remain unchanged for capability adapters; nominal value-type comparison uses
the instance key whenever either graph is instantiated. Aliases share
one token; a second instantiation gets distinct tokens. Tokens are private
in-process identities, never serialized IDs. The view and its tokens survive all
tasks, endpoint payload graphs and lifted owners and are released after exec
teardown. Resource-free binaries keep the existing borrowed graph.

Mutating the decoded graph would invalidate concurrent instances. Adding separate
resource comparison callbacks to every canonical conversion and endpoint path
would create competing type-comparison rules. The instance view instead preserves
the existing nominal equality and canonical value checks. This adds bounded
metadata allocations during instantiation; it does not copy guest memory or
create a second runtime type system. The source binary must still outlive the
exec. Imported equivalence classes are mapped to their provider's retained
tokens before binding resource-bearing calls. Rollback destroys only
view-owned nodes/headers/tokens and leaves the
decoded metadata untouched. Synchronous public admission retains its current
contracts while this private integration is completed.

Private async resource imports resolve each imported resource export explicitly
to an already initialized provider and its resource type. A consumer binds all
aliases of that declaration to the provider's instance key and retains the
provider once per imported equivalence class. Conflicting or ambiguous mappings
fail initialization; partial construction releases every acquired reference.
Mappings are immutable after initialization, bounded by decoded resource count,
and use the same exclusive execution owner as calls and resource tables. Provider
chains are acyclic because a resolver can only return an initialized instance.
Provider references retire after the consumer's tasks, handles and lifted owners.

Own values carry a private instance key in addition to their declaration ID.
Canonical validation compares the key when present; destination tables continue
to use their own declaration IDs. Lower reserves destination slots, then commits
the whole batch without allocation or callbacks. Failure rolls back reservations
and preserves uncommitted owners. Imported owned handles keep the representation
unchanged and route final destruction to the retained provider, including through
another importing instance. Runtime identity never crosses a serialized/public
boundary. Host capability values without an instance key retain their existing
nominal-ID checks. This avoids renumbering adapter IDs or using one global table.
Guest resource.drop forwards the caller's execution control and trap destination
through the provider chain. Destructors use the existing fresh synchronous context,
can retain their native stack across fuel/interruption yields, and restore provider
task/context guards when the caller finishes or unwinds. Host value destruction
uses the active source task's control when one exists; quiescent host cleanup keeps
its existing non-suspending behavior.

Imported borrow parameters use the existing resource table and task counter.
Lower into the defining instance passes the representation; lower into another
instance reserves a non-owned handle, then publishes it with the callee task's
stable borrow-counter address. Drop decrements that counter without a destructor.
The source handle stays lent through terminal event delivery, including transitive
borrowing. Results and endpoint payloads continue to reject borrow types.

Unreleased subtasks form intrusive child lists on their calling tasks. These are
borrow-lifetime dependencies, not a second execution queue: the existing exec
registry remains the progress/frame owner. Terminal delivery unlinks the child.
Failure/destruction unwinds Core and its wait pins first; a task with borrowed
handles then aborts dependent children before clearing its own borrow handles and
detaching its caller. A blocked child keeps the parent task/caller and counters
alive for a later teardown retry. Tasks without borrowed handles detach remaining
children under the existing independent-continuation contract. Private dependency
depth is bounded to 256; admission beyond that bound returns OUT_OF_MEMORY before
publishing a task. Together with task/table quotas this bounds cleanup recursion,
storage and handle counters. This introduces no workers or concurrent mutation.

Public async admission stays closed while the remaining host ownership and
payload integration is completed. Rollback can close imported resource admission
and discard all instance views without migrating decoded data or changing the
synchronous public ABI.

The first private execution primitive is allocation-free notification state
embedded in the eventual task/endpoint owner. It tracks pending progress and
terminal-event delivery separately. Subtask start and resolve notifications
coalesce into the latest state; a terminal event authorizes the enclosing owner
to release loans before returning control to the guest. Endpoint completion is
classified at delivery: a stream prioritizes peer drop, then cancellation, then
progress; a future prioritizes its one completed value, then peer drop, then
cancellation. No pointer, buffer or resource ownership moves in this primitive.
Its caller owns transfer buffers, affinity, waitable membership and generation
checks. It has no scheduler, queue or allocation; every operation is O(1), with
one pending notification per owner. Failed transitions leave state/output
unchanged. Host cancellation may await acknowledgement while retaining the copy
obligation. Canonical decoding and host admission remain closed until these
states are connected to actual transfers, waitables and execution.

Canonical async handles share the existing generation-protected resource table.
Each live slot carries an explicit kind; resource operations reject waitable and
waitable-set slots before inspecting nominal identity or invoking destructors.
The existing slot quota and nonwrapping generation retirement apply across all
kinds. The table remains the only authority for handle existence and kind.
Async slots borrow stable objects owned by the enclosing task/endpoint/instance;
these owners must unregister objects before freeing them or destroying the table.
Table growth moves slots, so callers retain handles and stable object addresses,
never pointers into the slot array across allocation or callbacks.

The private waitable layer stores each waitable's set handle and exclusive
synchronous-wait flag. Waitable sets store an active-wait count and a scan cursor;
membership and pending events are read from live table entries. Moving membership
validates both handles before mutation. A nonempty or actively waited-on set
cannot drop; a synchronous waiter prevents join/drop/event theft. Poll selects
one pending notification, advances its round-robin scan cursor and consumes the
notification through the existing state primitive. Empty polling returns the
canonical NONE tuple. No separate event payload, list or queue is allocated.
Poll/drop checks are O(table capacity), bounded by the shared slot quota; other
membership transitions are O(1). This deliberately follows the specification's
simple search model until measurement justifies an additional index. Terminal
subtask delivery invokes the owner's nonblocking loan-release hook before making
the event observable. The hook may grow the table, so delivery retains only the
stable owner pointer and copies the event before invoking it; no slot pointer
survives that callback. These interfaces remain private until canonical guest
bindings and async task/transfer owners are complete.

Private paired endpoints implement rendezvous over transfer-owned host value
arrays. Each endpoint embeds its registered waitable at a stable address and
borrows its immutable endpoint type graph. Pair construction optionally registers
both ends in their owners' canonical tables; failure removes the first handle
before returning and leaves both output owners empty. A NULL table represents a
host-owned end. No elements are buffered inside the pair and no worker is created.
The logical transfer length is bounded by the canonical 28-bit buffer limit;
actual value arrays remain subject to Runtime allocation budgets.

A submitted buffer is exclusively borrowed until event delivery. A separate
available-buffer pointer permits additional rendezvous to extend an older
operation's partial progress before its event is consumed. Delivery detaches
both pointers before the caller can reuse the array. Input arrays contain unique,
already owned canonical values; output cells must be empty. Whole-array type and
destination admission precedes any move. Copying moves each complete value and
its cleanup obligation, clearing its source cell. Untransferred source values
remain owned by the transfer caller. Unit streams/futures advance logical counts
without allocating dummy values. Future operations always request one element.

Zero-length stream operations follow canonical readiness asymmetry: when both
ends submit zero elements, only the writable end completes. Peer close, immediate
copy cancellation and event delivery reuse the existing notification precedence.
Cancellation stops further rendezvous on that buffer; delivery returns its
exclusive borrow without replaying or destroying transferred elements. Close
rejects active operations and unregisters the handle before notifying the peer.
Same-component copies of nonnumeric, nonunit payloads trap as required by the
pinned proposal. Waitable event delivery now runs the enclosing endpoint's buffer
release hook as well as terminal subtask loan release. These hooks cannot run
guest code and cannot recursively consume or destroy their own waitable.

This increment supplies actual host-value movement, not guest-memory conversion
or public async admission. Guest buffers must subsequently use checked memory
offsets and the existing canonical codecs, with resource commit/rollback and
execution control retained across realloc. Nested endpoint values also need that
codec/host-value integration. Keeping these paths private preserves explicit
rejection until the complete canonical execution boundary is available.

Private readable-end ownership transitions detach an idle end from its canonical
table to the host, or attach a host-owned idle end to a destination table. The
stable endpoint and peer connection do not move. An active operation, exclusive
waiter, delivery callback or waitable-set membership prevents either transition;
DONE ends cannot transfer. Pending peer-close notifications survive an idle
transfer and acquire the destination handle when delivered. Attach allocates the
destination slot before publishing ownership, so allocation/quota failure leaves
the end host-owned and retryable. These are internal owner operations; canonical
lift/lower admission must additionally validate the guest handle kind and payload
type before calling them. They do not admit guest binaries or expose a public API.

Nested private endpoint values use the canonical value's existing move and
release protocol. An idle, host-owned readable end can enter a value, freezing
direct endpoint operations until that value is taken back or destroyed. The
value carries a borrowed graph/type reference and opaque stable owner; the
canonical module compares value types structurally (resource leaves by nominal
identity), without depending on endpoint execution. The endpoint module supplies
the release hook. Moving a record/list moves this obligation unchanged; destroying
it closes the contained readable end and notifies its writer once. Extracting it
clears the value before restoring direct endpoint access. Graphs and stable
endpoint storage must outlive the containing value. A small Runtime-allocated
owner record belongs to each value obligation. Taking the value frees that
record; committing it to a guest clears the record's endpoint pointer. Later
destruction of the old value can therefore free its record without touching a
newly lifted owner of the same endpoint. Record allocation failure preserves the
endpoint and output. No guest code runs in these ownership transitions.

Private canonical endpoint codecs use four-byte handles in memory32 and memory64
and one i32 flat carrier, including endpoints nested in composite values. The
canonical module dispatches through explicit endpoint callbacks independently of
resource callbacks; an absent callback still returns UNSUPPORTED. Lift validates
kind, structural payload type, idle state and lack of set membership before
allocating a value owner and removing the original handle. Allocation failure
does not consume the handle. Earlier successfully lifted composite fields are
destroyed if a later field traps, following existing canonical lift semantics.

Lower conversion uses a caller-owned codec scope with a bounded intrusive chain
of endpoint reservations. The shared handle quota bounds this chain. Reserved
slots refer to stable waitables but are not published as their active table/handle,
so guest operations reject them during realloc reentry. A source value and its
owner record remain exclusively borrowed until explicit commit or rollback.
Duplicate lowering of one owner, including from another active scope, traps.
Commit preflights all reservations, publishes their table ownership, and clears
the old value records. Rollback removes reservations while preserving each input
value. Neither finishing operation allocates or runs guest code; both require
the existing exclusive Component execution thread. Partially written guest
memory is not a published result and may contain invalidated handles after an
abort. The composition layer must finish the scope before publishing results or
destroying inputs. Public async and binary admission remain closed until task,
guest-copy and lifetime integration is complete.

Private endpoint buffers distinguish host value arrays from guest memory regions.
Guest regions retain their canonical options, graph/type reference and checked
offset/count, never a raw linear-memory pointer. Admission validates the complete
nonempty typed range and conversion callbacks before lending it; unit and
zero-length buffers ignore the pointer/options as specified. Reads snapshot a
whole rendezvous batch before any destination write, preserving eager canonical
semantics even for overlapping numeric guest regions. Host-only copies keep the
existing allocation-free move. Guest snapshots use checked Runtime allocations
bounded by the 28-bit element limit and the configured allocation budget.

Guest destinations with owned resources or endpoints require explicit batch
commit/rollback callbacks from their composition owner. Lowering reserves all
handles before commit. A failed lower rolls back destination reservations; a
failed guest lift destroys values already lifted, without restoring consumed
source handles. Successful host-to-guest copies destroy the old host cells only
after the batch commit transfers their release obligations. Source and destination
progress follow their respective complete batch read/write, so a trapping write
may follow a successful destructive read. Such a trap is terminal for the pair:
both pending operations retain their buffer borrows until error delivery, report
the primary status without publishing a success/cancel event, and then permit
closure. The pair cannot start another transfer or move after that failure.
Admission failures before an operation is accepted leave the existing peer
operation unchanged. Realloc, resource and cleanup callbacks run with both
endpoints guarded against reentrant take/cancel/close or another submit. Their
enclosing execution owner must retain Component instances and callback contexts
through suspension; no independent worker or raw guest-memory view is introduced.

Cross-instance copies explicitly borrow the task that triggers the rendezvous,
including a third-instance forwarding task. The endpoint passes that task only
through the current copy stack; pending buffers never retain a task pointer.
Each exec-owned guest buffer has a bounded, operation-local conversion context
with its own realloc options. Copy entry temporarily installs the driver in the
source/destination auxiliary domains, and copy exit restores those domains in
reverse order after commit, rollback and temporary-value destruction. Realloc
and failure destructors consequently use the driver's execution budget and trap
destination even when the receiving task is suspended or has already exited.
Domain guards and endpoint delivery guards survive fuel/interruption suspension;
forced unwind restores them before operation/error delivery releases the context.
Plain host-only copies need no task. This extends the existing private buffer
transaction hooks and auxiliary-task exclusion instead of adding a scheduler or
retaining arbitrary initiating tasks until a peer arrives. Public async admission
continues to require the remaining host-owner contract.

Private endpoint forwarding consumes an idle, unjoined readable end and writable
end of the same structural type. Admission checks both registrations and their
peer links before mutation. It removes the intermediate registrations and links
the surviving endpoints directly, preserving their owners, operation borrows and
undelivered progress. If both survivors have available buffers, it copies their
minimum remaining count through the existing guarded buffer conversion and keeps
only the larger remainder available. Zero-length readiness still notifies the
writable side when both buffers are empty. Forwarding into the same pair closes
both ends; forwarding with an already absent peer propagates closure to the
remaining peer. No intermediate queue or payload allocation is added.

The exclusive execution owner performs admission and link publication without
callbacks. Conversion runs only after the intermediate ends have closed. A copy
trap after publication returns its primary status from forwarding and marks both
surviving operations failed; their buffers remain borrowed until error delivery.
This irreversible consumption is distinct from admission failure, which preserves
all endpoints and handles. The private caller must propagate the trap and drain
both operation errors before destroying storage. Guest decoding and public host
admission remain gated on the complete task/lifetime integration.

## Canonical post-return lifecycle

Synchronous lifts with `post-return` copy/lift the Core results before invoking
the cleanup function with the original flat results (including an indirect
result pointer). Its signature must be exactly those Core results to no results.
Duplicate options, missing indices and mismatched signatures fail admission.
See the pinned [Canonical ABI](https://github.com/WebAssembly/component-model/blob/a25fc0b372dd21f07f0242c46e98bd0f1ea0c0e1/design/mvp/CanonicalABI.md#canon-lift).

The call owns its lifted result until cleanup succeeds. Core failure or failed
lifting skips cleanup; cleanup failure discards the unpublished host result.
Already committed guest side effects are not rolled back. A primary call failure
takes precedence over a secondary destructor failure during unwinding.
The Component exec owns a single `may_leave` gate, cleared around post-return;
canonical lower traps before any provider invocation while that gate is clear.
The gate is restored after success, failure or cancellation unwinds the frame.
All of these transitions run on the existing instance execution owner thread.

For resumable calls, a private Runtime completion hook runs lifting and cleanup
inside the original coroutine, using the same fuel/interruption control. This
avoids a second coroutine allocation, a reset fuel budget or replayed cleanup.
Fuel/interruption may suspend the cleanup frame; canonical host calls cannot
leave the component there. The call retains the instance and its lifted result
until completion or cancellation. Capacity remains the existing bounded flat
result tuple, canonical value limits and Runtime call/stack/allocation budgets;
there is no new queue, unbounded storage or additional execution owner.

The alternative of invoking cleanup after `resume` returns would bypass its
execution budget. A separate resumable execution would require duplicating
budget and cancellation state. The private completion hook keeps Runtime free
of Component types and preserves the installed ABI. Removing the hook and
rejecting the option again is the rollback path. Validation includes direct and
indirect results, empty results, allocator/lift failures, cleanup traps/exceptions,
cross-instance cleanup, canonical-leave rejection, fuel yields and cancellation.
Cleanup binding admits Core-instance functions and canonical functions with the
required signature. Canonical lower and resource.drop targets always trap on
invocation because post-return clears `may_leave`; resource.new and resource.rep
cannot match a zero-result cleanup signature. The same leave check guards new
and drop reached indirectly through Core imports, before handle mutation or a
destructor/provider callback. Resource.rep remains permitted inside Core cleanup.
This follows the pinned resource builtin definitions, without routing a direct
canonical cleanup through an invented Core instance or additional coroutine.
Guest realloc uses the same instance gate, restored on every return path; it
cannot call canon lower, resource.new or resource.drop while canonical storage
is being allocated. This changes previously admitted invalid canonical behavior
into a trap before provider or handle side effects. The private context gains
only a borrowed gate pointer; the exec already owns and outlives that context.

## Nested canonical Core execution

Guest resource.drop consumes its canonical handle before invoking the resource
destructor. The destructor re-enters Core through a private Runtime host-call
boundary, borrowing the active call's execution control and adding one logical
call level. Fuel, interruption, host waits, tier selection and stack limits
therefore belong to the original execution. The ordinary public invoke API is
reserved for host-owned resource destruction outside a running guest call.

The borrowed host call and destructor context live on the original coroutine's
stack until the callback returns, including suspension and cancellation unwind.
They are single-threaded on the execution owner; no instance-global current-call
pointer, new execution, queue or allocation is introduced. Capacity is bounded
by the existing Runtime call/stack limits and resource-table quota. Cancellation
or failure never restores or replays an already consumed resource handle;
destructor traps propagate, and uncaught Core exceptions become Component traps.

A fresh public invocation would reset fuel and depth. A shared mutable execution
pointer would also mix independently suspended calls. The private borrowed-call
boundary avoids both while preserving the public ABI and host destruction
semantics. Validation covers suspension inside a destructor, cancellation,
exactly-once consumption, traps/exceptions and call-depth exhaustion with native
and interpreted execution. Reverting this boundary would require rejecting guest
destructor re-entry under controlled execution rather than silently resetting
the control state.

Canonical lowering also borrows the active host call for guest realloc. Each
lower invocation copies its memory/realloc options onto its own coroutine frame;
the exec's immutable binding never stores a current execution pointer. Realloc
therefore shares fuel, interruption and depth with the caller while `may_leave`
is cleared. Cancellation unwinds and restores that gate, destroys the private
host result, and publishes no partially written result tuple. Guest allocator
side effects already executed remain committed; the canonical ABI has no
rollback operation for them. Realloc called directly while preparing an ordinary
host entry retains that entry's existing synchronous semantics. Tests cover
memory32/memory64, suspension, cancellation, trap/exception conversion and depth.

## Local canonical lowering

Local canonical lowering binds a Component function index to the same lift
adapter used by host calls. Function/alias maps are built from binary metadata
before Core instantiation; adapters are admitted on first use once their earlier
Core providers exist, including consumer start functions, and all remaining
adapters are admitted before the exec is published. There is no second mutable
function registry. Each nested invocation copies the adapter and memory options
onto its coroutine frame, borrowing the original host call for its Core body,
realloc and post-return. No public ABI or new execution owner is introduced.

Local resource arguments lift through the exec's canonical table: own removes
the caller handle, while borrow lends the existing handle until return or unwind.
The callee receives ownership only after all argument lowering succeeds. Local
owned results carry a private release obligation until all caller-side result
lowering succeeds; newly created result handles are staged and removed if a
later field fails. Commit transfers them to the caller's table exactly once.
Destructors on rollback follow the same borrowed execution control. Existing
resource quotas, canonical nesting/size limits and Runtime depth/fuel limits
bound storage and work. Core failures consume already admitted own arguments;
unpublished result owners are destroyed, never replayed. Imported-provider
adapters retain their existing conversion contract.

Duplicating a local interpreter or routing through synthetic external imports
would create competing type/ownership state. Reusing lift admission and the
canonical table keeps the source of truth unchanged. Validation covers aliases,
start functions, scalar/composite/string values, memory32/64, own/borrow movement,
allocation failure, post-return, cancellation and native re-entry. Rollback is
to reject local canonical lowering before instance publication.

## Canonical parameter tuples

Canonical parameter tuples use one shared layout and memory-transfer path for
lift and lower. Indirect calls validate the complete aligned tuple range before
consuming any resource field; pointer width follows the selected memory32/64
option. The caller-provided indirect result pointer remains a separate flat
argument after the input tuple pointer. Larger logical argument lists use one
checked allocation sized from the validated function parameter count, subject to
the existing Runtime allocation limit, rather than a growing container. Small
tuples retain inline storage. Storage and lifted values belong to the active
callback frame across suspension and are destroyed on every return path; an
allocation or conversion failure never invokes the provider. Partially lifted
own values and borrow loans follow the existing local/import rollback contract.

## Canonical string encodings

The [pinned Canonical ABI](https://github.com/WebAssembly/component-model/blob/a25fc0b372dd21f07f0242c46e98bd0f1ea0c0e1/design/mvp/CanonicalABI.md#storing)
defines UTF-8, UTF-16LE and tagged Latin-1/UTF-16 strings. Binary canonical options
select the encoding; the existing canonical value boundary owns conversion.
Host values remain UTF-8. Private strings additionally retain their source
encoding, so forwarding a lifted string preserves source-dependent guest realloc
behavior. Source code-unit counts are derived from the immutable UTF-8 value,
not maintained as a second independently mutable length.

The private string codec validates Unicode and lengths before lowering. It
preserves the canonical allocation/growth/shrink sequence, source/destination
alignment and pointer-width tag. Guest realloc can move/grow memory; no guest
view survives a callback. Lift takes a bounded owned copy before conversion, so
the result never borrows guest memory. Host-side temporary/output buffers use
the existing Runtime allocator and limits, with one cleanup path. Source byte
length is bounded by the canonical string limit; transcoded storage and guest
allocation sizes use checked arithmetic. Unicode decoding and conversion take
O(n) time and bounded O(n) owned storage. Access is single-owner-threaded.

Available Salts string APIs validate/iterate UTF-8 but do not supply the required
UTF-16LE/tagged canonical allocator protocol. Adding a general encoding library
would not replace that protocol. The codec therefore factors the existing
canonical UTF-8 validation into a scalar decoder and adds the narrowly scoped
UTF-16/Latin-1 adapter; it introduces no dependency, general string container or
installed type. Invalid host strings return INVALID_ARGUMENT, malformed guest
strings/pointers trap, and allocator errors propagate. Failed lowering does not
publish a pointer/length tuple; guest allocations and realloc side effects are
not rolled back. Flat, indirect and nested composite values share this codec.

Validation covers every source/destination encoding pair on memory32/memory64,
Unicode boundaries and embedded NUL, malformed surrogates/UTF-8, tagged lengths,
alignment/bounds, moving realloc with exact callback traces, allocation failure,
and loaded components using both canonical lift and lower options. Public host
layouts remain unchanged. Rollback must revert option admission and codec use
together so an admitted encoding cannot silently take a UTF-8 path.

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

## WASI socket backend and reusable I/O readiness

The TCP implementation now provides installed `WASI02IO` and optional
`WASI02CNet` targets, reusable bounded readiness, external TCP progress and an
async WASI02 constructor. Enable `TURBOWASM_ENABLE_WASI02_SOCKET_BACKEND` only
with an SDK exporting `cnet_connection_preserve_send_on_eof`; configure checks
that capability. The Salts prerequisite is on `codex/wasi-socket-prerequisites`
at `8aeeaff7`. Published SDKs without it cannot enable this target.

Compose TCP facades with `turbowasm_wasi02_cnet_wasi02_init`. This explicitly
retains the adapter/domain and installs a private transport-terminal predicate;
state observation commits only actual termination to the authoritative WIT
table. Peer EOF and half-shutdown leave the socket connected. The installed
provider layout remains unchanged. Generic sources use
`turbowasm_wasi02_io_wasi02_init`. Existing synchronous constructors remain
available. Stream/splice and connect/accept publication reserve facade result
storage and canonical handles before consuming provider results.

Real loopback tests exercise IPv4/IPv6, inherited listener options, repeated
subscriptions, bounded transfer/flush, immediate half-close, detached streams
and shutdown with retained carriers. Pinned Component fixtures exercise WIT
connect, wait, finish-connect, write, blocking-read and cleanup through memory32
and memory64. Imported compound type aliases retain their dependency closure:
auxiliary cloned nodes preserve serialized indices during decoding and are
compacted before validation; shared dependencies are cloned once per closure,
with checked size arithmetic and the existing value-depth bound.

UDP/DNS gates C/D are implemented through the additive v2 interface described
under "Implemented UDP and DNS gate" below. Preview1 socket coverage and
Preview3 remain separate work. The sections below retain the design and
qualification requirements for each gate.

The initial deliverable was a complete native TCP path for the pinned
`wasi:sockets@0.2.8` interfaces. UDP and name lookup are separate qualification
gates. Component async host owners remain the execution/lifetime boundary;
WASI 0.2 byte-stream resources are not Component typed `stream<T>` endpoints.
Preview1 socket imports and a Preview3 WIT package are separate admission work,
not aliases added to this implementation.

### Evidence and dependency boundary

At the design baseline, `include/turbowasm/wasi02_sockets.h` described TCP
providers. `src/wasi02_sockets.c` implements the typed state machine, connects
successful connect/accept results to the existing stream resource table, and
rejects absent provider methods. `tests/wasi02_tcp_state_test.c` qualifies that
contract with a fake provider. The concrete provider in `src/wasi02_cnet.c`
then supplied create/bind/listen/options only;
`tests/wasi02_cnet_control_test.c` explicitly checks that connect, accept,
remote-address, subscribe and shutdown callbacks remain absent.

`src/wasi02_poll_native_io.c` records readiness for one terminal NativeIO
request. That contract remains useful for completion-bound sources, but cannot
represent a socket subscription reused across bind/connect/listen operations.
At that baseline the installed WASI02 facade selected only the synchronous exec
constructor; the implementation now adds a separate async-aware constructor.

The locally inspected released Salts 2.2.0 SDK exports external CNet client and
listener progress, exact bound-socket connect handoff, portable endpoint
queries, receive demand, half-shutdown, and external UDP progress. These are
capabilities verified in its installed `cnet/cnet.h`, not an assumed minimum
SDK version. Configure must compile/link-check the required public symbols
against the actual selected SDK; do not access CNet internals or provide an
alternate raw-socket implementation when a capability is missing.

At the design baseline, Salts' `cnet/src/cnet_resolver.h` was private and yielded
one selected native address; the public UDP API lacked an unbound/configurable
WASI socket contract. The prerequisite branch now adds `cnet/name_lookup.h`
and versioned datagram controls. Configure checks these actual installed SDK
capabilities before admitting the native adapter.

Normative references are the versioned upstream [TCP WIT](https://github.com/WebAssembly/wasi-sockets/blob/v0.2.8/wit/tcp.wit),
[TCP state/readiness semantics](https://github.com/WebAssembly/wasi-sockets/blob/v0.2.8/TcpSocketOperationalSemantics.md),
[I/O streams WIT](https://github.com/WebAssembly/wasi-io/blob/v0.2.8/wit/streams.wit),
[UDP WIT](https://github.com/WebAssembly/wasi-sockets/blob/v0.2.8/wit/udp.wit),
and [name lookup WIT](https://github.com/WebAssembly/wasi-sockets/blob/v0.2.8/wit/ip-name-lookup.wit).
Pin their resolved commits in the fixture provenance when implementing; the
unstable `network-error-code` export is not part of this stable gate.

### Module and state ownership

Keep `TurboWasm::WASI02` provider-neutral. Add optional installed
`TurboWasm::WASI02IO` and `TurboWasm::WASI02CNet` targets. The first supplies a
shared provider representation/notification domain; the second depends on it,
WASI02 and `Salts::CNet`. NativeIO public types appear only in the optional
CNet adapter header. No new third-party library is needed for TCP.

```text
WASI02 socket / stream / poll imports
    -> existing Component resource tables and canonical ownership
    -> WASI02IO provider-token dispatch and reusable subscriptions
    -> WASI02CNet bounded socket / connection / buffer owners
    -> public external-progress CNet APIs
    -> one caller-owned NativeIO backend

Runtime host-wait generation -> one aggregate wait route -> ready sources
Component call/task resume   <- host completes progress, then resumes explicitly
```

The existing WASI socket table is authoritative for WIT operational states.
CNet is authoritative for transport state and request completion. The adapter
owns only their translation: terminal connect/accept outcomes awaiting a WIT
method, half-close flags, byte queues, and publication obligations. A native
connect completion resolves the pending outcome; `finish-connect` commits the
WIT transition. Only actual connection termination may asynchronously mark an
already connected WIT socket closed. Do not mirror OS descriptors or maintain
a second mutable copy of the WIT state machine.

WASI02IO holds bounded provider-token slots and wait membership, not another
Component resource table or scheduler. Source handles include domain identity;
provider tokens use unique, non-wrapping identities checked against live slots
of the expected kind. Cross-domain, stale, or wrong-kind tokens fail before
callbacks. Identity exhaustion rejects admission rather than aliasing an old token.
Source readiness is queried from its owner. Change notifications are hints to
recheck, not a second authoritative boolean.

### Host interface and composition

The following TCP and I/O entry points are implemented; exact declarations and
ownership contracts are in the installed headers. UDP/DNS use the separate v2
provider/configuration entry points described below. All owner carriers are unique,
opaque and zero initialized.
Configuration structs have `size` and `api_version`, finite defaults, checked
field validation, and no implicit unlimited values. Source handles are opaque
generation-checked identities scoped to the I/O domain.

```c
/* Installed wasi02_io.h; see that header for the complete declarations. */
turbowasm_status turbowasm_wasi02_io_init(
    turbowasm_wasi02_io *io, const turbowasm_wasi02_io_config *config,
    const turbowasm_runtime_config *runtime_config);
turbowasm_status turbowasm_wasi02_io_source_register(
    turbowasm_wasi02_io *io, const turbowasm_wasi02_io_source_ops *ops,
    turbowasm_wasi02_io_source *out_source);
turbowasm_status turbowasm_wasi02_io_source_changed(
    turbowasm_wasi02_io *io, turbowasm_wasi02_io_source source);
turbowasm_status turbowasm_wasi02_io_source_close(
    turbowasm_wasi02_io *io, turbowasm_wasi02_io_source *source);
turbowasm_status turbowasm_wasi02_io_pollable_register(
    turbowasm_wasi02_io *io, turbowasm_wasi02_io_source source,
    turbowasm_value *out_owned_rep);
turbowasm_status turbowasm_wasi02_io_stream_register(
    turbowasm_wasi02_io *io, turbowasm_wasi02_io_stream_kind kind,
    const turbowasm_wasi02_stream_provider *operations,
    turbowasm_value owned_rep, turbowasm_wasi02_io_source source,
    turbowasm_value *out_domain_rep);
turbowasm_status turbowasm_wasi02_io_cli_factory_set(
    turbowasm_wasi02_io *io, turbowasm_wasi02_io_cli_kind kind,
    turbowasm_wasi02_stream_factory_fn create, void *context);
turbowasm_status turbowasm_wasi02_io_providers(
    turbowasm_wasi02_io *io, turbowasm_wasi02_stream_provider *out_streams,
    turbowasm_wasi02_poll_provider *out_poll);
turbowasm_status turbowasm_wasi02_io_advance(turbowasm_wasi02_io *io);
turbowasm_status turbowasm_wasi02_io_destroy(turbowasm_wasi02_io *io);

/* New wasi02_cnet.h; the NativeIO backend is borrowed. */
turbowasm_status turbowasm_wasi02_cnet_init_external(
    turbowasm_wasi02_cnet *adapter, turbowasm_wasi02_io *io,
    native_io_backend *backend, const turbowasm_wasi02_cnet_config *config,
    const turbowasm_runtime_config *runtime_config);
turbowasm_status turbowasm_wasi02_cnet_socket_provider(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02_socket_provider *out_sockets);
turbowasm_status turbowasm_wasi02_cnet_advance(
    turbowasm_wasi02_cnet *adapter, size_t *out_events);
turbowasm_status turbowasm_wasi02_cnet_route_completion(
    turbowasm_wasi02_cnet *adapter, const native_io_completion *completion,
    bool *out_consumed);
turbowasm_status turbowasm_wasi02_cnet_next_timeout(
    turbowasm_wasi02_cnet *adapter, uint32_t max_wait_ms,
    uint32_t *out_timeout_ms);
turbowasm_status turbowasm_wasi02_cnet_shutdown_request(
    turbowasm_wasi02_cnet *adapter);
turbowasm_status turbowasm_wasi02_cnet_shutdown_poll(
    turbowasm_wasi02_cnet *adapter, bool *out_complete);
turbowasm_status turbowasm_wasi02_cnet_destroy(
    turbowasm_wasi02_cnet *adapter);

/* Addition to wasi02.h, preserving the existing sync constructor. */
turbowasm_status turbowasm_wasi02_component_instance_create_async(
    turbowasm_component_instance *instance, const turbowasm_component *component,
    turbowasm_wasi02 *wasi02,
    const turbowasm_component_async_options *options);
```

`io_source_ops` contains a borrowed stable context and copied `ready`, `retain`
and `release` callbacks. Registration retains the source only after all slot
allocation and validation succeeds. Close consumes the registration handle,
marks the source closed/ready, and releases the context after all child stream,
subscription and route references retire. `source_changed` only marks bounded
notification work; it never executes a guest or recursively advances I/O.
`io_advance` rechecks dirty sources and completes exact retained Runtime waits.

`io_stream_register` copies the operation table, takes `owned_rep` only on
success, and returns one stream representation owned by the caller until it
is successfully transferred into WASI02. Its source reference and provider drop
obligation travel with that representation. Failure preserves caller ownership.
The data callbacks use the existing input/output/error signatures; CLI producers
are configured before provider publication with `io_cli_factory_set`; their
factories return an owned domain stream rep produced by `io_stream_register`.
Filesystem factories wrap their results into this same domain through that
registration API. Factory contexts are borrowed until domain destruction; no
factory replacement is allowed while its produced streams are live. The copied
stream operation context remains valid through the delegate's final drop, using
an explicitly retained owner when different from the readiness source context.
Error representations likewise receive kind-checked wrappers
and exactly one delegate `error_drop` obligation.

This permits socket, stdio and filesystem streams plus timer/readiness sources
in one poll wait-set. Existing arbitrary provider reps must first be explicitly
wrapped and supplied a readiness source; never overwrite configured callbacks,
guess a rep's origin, or compose opaque `arm_many` callbacks that own different
wait storage. Existing custom bundles continue to work through their existing
WASI02 configuration. Wrapping them is an explicit migration when combining
them with this adapter.

Provider tables borrow their domain/adapter. The I/O domain outlives the adapter,
and the NativeIO backend outlives every borrowed CNet owner and its real terminal
completion. Init is transactional; destroy rejects live registrations/resources,
waits or undrained requests without partly destroying the owner. The new
constructor retains the existing WASI02 context through the Component's final
reference, including tasks, shutdown work and returned resource values; failure
leaves the output empty and balances every retain.

### Progress and reusable subscriptions

One host thread performs all admission, CNet advance/routing, source publication,
wait completion and guest resume. No worker, recursive backend observation, or
cross-thread send queue is added. The host alone observes NativeIO. Route each
completion to its owning CNet client/listener/datagram owner; unrelated results
return `out_consumed=false`. After routing the full batch, advance owners, then
the I/O domain, then explicitly resume eligible Component calls/tasks. Preserve
the first error while settling the remaining owned batch entries. A stale or
duplicate terminal cannot release storage or complete a second wait.

`next_timeout` supplies a capped deadline for CNet and, later, resolver progress.
An idle drive reports zero events without inventing readiness. Backend request
snapshots are transient routing hints only; no socket subscription is permanently
bound to one request identity. An exact terminal identity remains retained until
its CNet owner acknowledges completion, including after cancellation.

Each `subscribe` allocates a child pollable retaining its logical source. The
same pollable follows later operations on that source. A connected socket's
socket pollable is ready; traffic readiness comes from its input/output sources.
Input readiness derives from readable bytes or a terminal state. Output
readiness derives from a usable permit or terminal state, respecting a pending
flush. Listen readiness derives from an accepted child or terminal accept error.

Wait-any registration preflights every handle, route/member capacity and owner
reference, links all members, then rechecks every readiness predicate before
suspending. Already-ready and completion-before-arm paths cannot lose a wake.
Return every ready input index, preserving duplicate positions. Cancellation of
one wait removes its membership and acknowledges its Runtime wait; it does not
cancel a shared socket request or drop another subscriber. Parent closure wakes
subscribers to observe the terminal outcome and keeps storage alive until their
references retire. Concurrent/callback reentry returns the existing busy/invalid
admission status; notifications never execute guest callbacks.

### TCP publication, byte ownership and shutdown

Use `cnet_listener_connect_endpoint` to consume the exact unbound/bound owner;
never close a bound socket and create another with the same address. The source
slot and observer context remain stable across handoff. A successful start
records one connection owner; its terminal outcome is consumed once by finish.
Before transport admission, reserve stream representations, readiness sources,
byte storage and publication metadata. A failed admission releases uncommitted
storage. After an irreversible handoff, preserve the actual terminal failure
and the required WIT state; do not replay connect or restore a consumed owner.

For accept, maintain at most one external accept and one pending accepted child
per listener. Reserve child/stream capacity before consuming the child. Prefer
the public direct listener-to-client accept API because it preserves inherited
listener socket policy; do not adopt through a client policy that silently
changes the child's settings. Retain the accepted owner until callback admission
and result publication settle. If canonical resource/result publication fails,
drop unpublished handles and close the accepted owner exactly once; native
side effects cannot be rolled back or returned as a fresh retryable child.

Keep the shared byte path single-producer/single-consumer on the owner thread.
Each connection has bounded receive and send storage, one receive demand at a
time, and one logical send in flight initially. Receive is admitted only after
reserving room for the maximum callback payload. Copy a borrowed CNet receive
view before callback return; never retain it or a guest linear-memory view.
Stop issuing receive demand while storage is full, letting TCP exert backpressure.

Read/skip consume in order; empty open input is distinct from EOF. The existing
provider read contract returns a borrowed view which must remain valid through
the facade's immediate result copy. The chosen change preallocates the result
nodes and a bounded byte chunk before calling the provider. Pass no more than
that chunk's capacity as `max_bytes`; a short read is permitted. After a validated
callback result, copying and completing the host result require no allocation,
so provider consumption cannot be followed by a host-result OOM. Skip similarly
preallocates its result nodes before consuming. A later guest canonical lowering
failure is an execution failure after an admitted read, not a retry that replays
bytes; previously executed guest allocator effects are also not rolled back.
Queued bytes precede remote EOF/error except explicit receive-shutdown, which
discards them. Define bounded scratch lifetime separately from the live queue.

`check-write` reserves a permit in bytes for that output stream. Validate and
copy the entire permitted write before publishing it; writes above the permit
trap, while a stream closed since the permit reports closed. The facade and
backend share one permit fact source. Reservations cannot be stolen by another
stream; a new check replaces the prior unused permit. A copied accepted write
returns promptly; CNet completes partial native sends internally. Send storage
retires only at a real terminal callback. Failed admitted sends become retained
stream errors, rather than a false pre-admission failure.

Flush captures the prior write sequence and closes write permits until that
sequence is terminally drained or failed. It does not promise peer consumption.
Blocking methods use existing Runtime host waits without blocking the host's
progress owner. A one-shot invocation requiring a wait is rejected before arm
or irreversible write admission. Zero-write/splice use the same budget/permit
logic; same-source aliases and overlapping ownership fail before mutation.

Socket, input and output resources retain one connection owner independently.
Dropping one carrier does not prematurely invalidate the others. Logical drop
consumes its handle; it must not synchronously wait for callbacks on the same
owner. A bounded retirement record retains requests/buffers/context until drain.
Half-shutdown closes the selected stream immediately, uses CNet's directional
shutdown, and leaves the socket resource alive. Send shutdown rejects new bytes
and drains previously admitted writes before FIN. Repeated directions are
idempotent. Dropping an output without flush may discard pending data according
to the WIT contract, but cancellation still waits for real transport terminals.

Adapter shutdown stops admission, closes every logical source, requests transport
cancellation, and continues external route/advance until terminal. It never
force-frees buffers, destroys the borrowed backend, or fabricates completion on
cancel-not-found. Existing public owners remain explicitly releasable; completion
requires their references, routes and retirement records to settle. Cleanup
errors remain observable independently of `out_complete`; repeated polling
cannot turn a recorded error into success. Component shutdown and adapter
shutdown have separate owners and must both reach their terminal conditions.

### Bounds, policy and errors

I/O domain config bounds source slots, stream/error wrappers, subscriptions,
simultaneous wait routes and members per route. CNet config bounds TCP owners,
pending accepts, per-connection receive/send bytes and commands, aggregate
adapter payload bytes, completion-batch work, and explicit deadlines. UDP
and DNS v2 configs add socket/message/query/name/result bounds. Defaults are finite
and published through an initializer; zero disables only an optional capability.

For configured TCP owner bound `N`, per-owner receive/send maxima `R`/`W`, and
route/member bounds `Q`/`K`, validate `N * (R + W)` and `Q * K` with checked
arithmetic. For example, 64 connections with 64 KiB in each direction reserve
8 MiB of adapter TCP payload, excluding CNet copies and metadata. This is a
capacity calculation, not a throughput claim. Bound CNet command/event storage
separately using its public configuration; account simultaneous adapter and
CNet copies instead of advertising the adapter byte limit as total process RAM.
Host-returned Component values retain their existing instance storage charges.

Proposed named defaults are listed below. Init helpers set these values, while
explicit config values are checked as hard bounds. Metadata is preallocated;
payload reservations are taken before native admission. Maximum owner counts
and a shared byte budget need not permit every owner to saturate simultaneously.

| Setting | Proposed default | Unit/full behavior |
| --- | --- | --- |
| I/O sources / stream wrappers / error wrappers | 512 / 512 / 128 | Slots; reject before taking ownership |
| Subscriptions / routes / members per route | 1024 / 64 / 64 | Slots; failed arm preserves all handles |
| TCP owners / accepts per listener | 64 / 1 | Owners; socket-limit or backpressure |
| TCP receive / send reservation per owner | 64 KiB / 64 KiB | Bytes; pause receive or return zero permit |
| Shared adapter payload budget / read chunk | 16 MiB / 64 KiB | Bytes; checked reservation or bounded short read |
| Native completions per host batch | 64 | Entries; host processes later batches without losing terminals |
| Connect deadline / read and write deadlines | 30 seconds / disabled | Explicit timeout policy, independent of stream readiness |
| UDP owners / inbound and outbound records per owner | 16 / 4 / 4 | Separate checked datagram reservation, including native receive storage |
| Max datagram / resolver queries / results per query | 65507 bytes / 16 / 64 | Hard maxima; explicit error on overflow |

These defaults are a bounded starting configuration, not performance tuning.
CNet command/event/request bounds must be derived and validated with these
reservations at init, including accept/connect and retiring owners. If the
borrowed NativeIO backend cannot cover simultaneous retained requests, fail
initialization instead of promising permits that cannot be honored. IDNA name
storage is bounded both before and after normalization (253 normalized DNS
name bytes excluding a final root dot; a separately configured UTF-8 input cap).

Pre-reserve callback output and retirement capacity: no receive/send terminal
may need unbounded allocation to become recordable. Exhaustion before admission
preserves input ownership and reports an explicit capacity/OOM result. Socket
limits map to WIT `new-socket-limit`; ordinary pending finish maps to
`would-block`. Stream backpressure is an empty read or zero permit, not an
invented closed stream. Internal invalid handles/ABI errors remain TurboWasm
status/traps; OS failures use the operation-specific network/stream error path.
The stream error payload preserves its originating error code and bounded debug
text; it is not reduced to closed or success.

At the existing facade boundary, the last granted permit remains authoritative;
the adapter's buffer reservation backs that grant rather than introducing a
second independently mutable permit. Permit release, admission and facade
accounting commit together. Error-wrapper capacity is reserved before a failed
operation needs to publish its error payload.

Network capabilities carry immutable copied policy: allowed address families,
bind/connect destinations and port ranges, accept/UDP/name-resolution permissions,
and bounded deadlines. Policy rule counts and copied bytes have hard limits too.
The new concrete adapter denies operations not explicitly
authorized by its config; this does not change existing custom-provider policy.
Check policy before native admission, preserve the same network identity across
bind/connect, and recheck DNS results when granting numeric destinations. A DNS
answer alone grants no connect capability. IPv6 scope/flow-info remains lossless;
mapped IPv6 and operation-specific invalid addresses follow the pinned WIT rules.
Socket setting queries read actual CNet/platform values after permitted clamping.
Platform unsupported settings report a real error rather than a successful no-op.

### UDP and DNS prerequisites and contracts

UDP adds separate provider callbacks/resource types for `udp-socket`, incoming
and outgoing datagram streams, plus `udp-create-socket`; byte streams are not
reused for messages. Add the public Salts portable unbound/bind, endpoint query,
peer association/disassociation and socket-option capabilities first. Filtering
in TurboWasm alone cannot replace native connected-UDP routing/error semantics.
Keep only one current pair per socket, with generation-checked replacement after
the old stream pair is released. Zero-length messages remain actual messages;
oversized/truncated messages never become successful shortened results.

Reserve room before admitting a receive callback. Each queued record owns its
payload and copied peer; stop demand when full. UDP loss outside admitted host
storage remains a transport property, not a silently dropped accepted callback.
Receive publishes a bounded list transactionally, consuming only after result
construction. Outgoing permits count datagrams. Send admits a prefix in order,
returns its exact count, and records a later error for the next operation when
the prefix is nonempty. An error before any accepted item returns that error.
Callbacks and cancellation obey the same bounded terminal ownership as TCP.

Name lookup requires a public Salts asynchronous resolver with a bounded query
owner, ordered portable IPv4/IPv6 results, numeric parsing without network I/O,
Unicode/IDNA validation, result iteration, deadline/progress integration and
cancellation with explicit terminal acknowledgement. Reuse Salts' existing
c-ares dependency behind that boundary; do not link TurboWasm to its private
resolver header or call blocking `getaddrinfo` on the progress owner. Salts must
review IDNA support in its existing dependency graph before adding a dependency;
ASCII-only successful admission must not claim complete WIT name lookup.

The WIT resolve-address-stream retains bounded copied name/result storage.
`resolve-next-address` distinguishes pending, next address, exhausted and failure;
subscribe tracks that source across results. Reject overflowing configured result
limits explicitly instead of silently truncating the address list. A resolved
result already accepted for delivery wins a later cancellation; otherwise keep
the query slot and callback context until cancellation is terminal. DNS deadlines
join the same host drive loop. Numeric fixtures and an injected resolver make
tests deterministic; external Internet/DNS availability is not a CI prerequisite.

### Alternatives, compatibility and delivery gates

Direct platform sockets would duplicate CNet policy, cancellation and portability.
A per-socket worker or executor would add another progress owner and complicate
shutdown. Reusing one-shot request pollables would yield stale readiness after
the first operation. A generic plugin registry or mutable global network facade
would hide owner identity. The chosen explicit adapter plus bounded source domain
solves the actual representation/wait-any composition boundary and preserves the
existing CNet/Runtime responsibilities.

The current sync WASI02 constructor and installed TCP provider layout stay
unchanged for the TCP gate. New owners/async constructor are additive. UDP/DNS
extend the public config through an explicitly versioned extended init/config
entry point with the old initializer preserved; do not append fields to an
unversioned struct and then read beyond an old caller's allocation. Consumers
using new gates rebuild/relink; no persistent data migration is involved.
Public API additions need approval of this concrete design before declaration.
Already approved Component async ownership remains in force.

| Gate | Complete implementation required before promotion | Qualification |
| --- | --- | --- |
| A: shared readiness | Source/domain tokens, wrapped stream/error owners, mixed wait-any, precise cancellation and retirement | Existing poll/stream tests plus reusable subscription, mixed source, duplicate/stale token, quota, reentry and OOM rollback cases |
| B: native TCP | Every installed TCP provider method, real connect/accept/read/write/flush/half-close, async WASI02 constructor, installed IO/CNet targets | IPv4/IPv6 loopback client/server Component fixtures, options inheritance, memory32/64, pause/backpressure, partial sends, delayed cancellation and detached resource lifetime |
| C: native UDP | Salts prerequisites, all stable UDP descriptors/providers/resources and publication paths | Empty and oversized messages, connected/unconnected peers, replacement, ordered partial batches, saturation, cancellation and loopback fixtures |
| D: name lookup | Public Salts resolver contract, stable lookup descriptors/provider, IDNA and bounded ordered result stream | Numeric IPv4/IPv6, Unicode/invalid names, multiple results, deterministic failures/timeouts, source reuse and terminal cancellation |

Tests must prove actual side effects and cleanup, not just non-NULL callbacks:
scripted transport terminals exercise races; real loopback fixtures exercise
the native owner. Include failed canonical result allocation after native accept,
read-result preallocation failure before consumption, one-shot wait rejection,
every destroy order,
generation exhaustion and whole-domain teardown. Add existing installed C/C++
consumer tests for the new targets and full public async socket round trips.
Run the configured CTest graph on Windows/Linux/macOS, ASan locally, and MIR
execution fixtures on Linux/macOS. Android qualification is build/install until
a runtime runner exists. CI builds the complete configured graph and runs tests
through CTest. No gate is advertised through placeholder methods.

Before publication, the build options can keep new gates private. After a gate
is published, disable its optional target/capability as the rollback path and
reject affected imports before instance publication; retain the existing custom
provider entry points. Reverting public declarations requires a versioned API
change, not silent removal. UDP/DNS prerequisites and remaining Preview1/
Preview3 work stay visible when the TCP gate alone is complete.

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
restartable interpreted/native execution implemented
typed module/host linking               implemented
optional CFlow deadline adapter         implemented
optional NativeIO host-wait bridge      implemented
lazy per-function MIR JIT               implemented
scalar/helper-backed SIMD MIR           implemented
helper-backed GC MIR                    implemented
lexical EH MIR                          implemented + upstream differential qualified
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

### Implemented UDP and DNS gate

The versioned `wasi02_network.h` provider and `wasi02_cnet.h` v2 constructors
implement the UDP/name-lookup gate described above. The existing TCP config and
provider layouts remain unchanged; callers opt into complete bundles and finite
facade resource capacities. The canonical bridge binds all four nominal resource
identities and handles own/borrow/drop through the same Component handle tables.
It reserves result storage before native admission or record/result consumption.

CNet owns native unbound/bind, connect/disconnect, IPv6 endpoint details and
socket options. TurboWasm retains fixed packet rings, explicit message grants,
ordered-prefix send errors and one live pair per socket. Readiness contexts for
closed pairs occupy bounded pair slots until every poll alias retires. Parent
socket drop does not invalidate child streams. Native cancellation does not
release request or payload storage before the actual terminal. Default policy
denies UDP bind/send/receive and DNS; optional address/name callbacks restrict
admission further. Denied incoming peers are filtered before guest publication.
UDP buffer setters implement the WIT's allowed clamping: after a native ENOBUFS
capacity rejection, halve the request with a finite integer bound until a real
setting succeeds. Other native errors propagate; getters report actual settings.

Salts exposes `cnet/name_lookup.h` using its existing c-ares owner and ICU UTS46
with nontransitional STD3/Bidi/context validation. This adds ICU behind the Salts
CNet ABI; TurboWasm does not link to ICU or c-ares. ASCII-only validation would
violate the pinned WIT Unicode contract; a second resolver would duplicate
progress, cancellation and ordering. The cost is ICU's SDK/runtime footprint.
Salts records the baseline-selected ICU version/license and installs Windows
runtime dependencies. No fallback silently relaxes IDNA validation.

Verification includes IPv4/IPv6 loopback, real empty datagrams, repeated source
readiness, exact send prefixes, connected/disconnected UDP, default-deny policy,
bounded local DNS A/AAAA responses, result overflow, deadline and dropped-query
retention. Pinned wasm-tools fixtures execute UDP message-list send/receive,
empty-message publication, ownership and DNS through
memory32/memory64 canonical ABI. The same host loop drives TCP, UDP, DNS and
shared pollables. Full Preview1 socket coverage and Preview3 remain independent
work. Rollback removes v2 admission without changing existing TCP callers or
persisted guest data.

At revision `15730ef`, [native socket CI](https://github.com/qigao/turbowasm/actions/runs/37807592953)
used Salts prerequisite revision `9cf0af75` from
[SDK preparation](https://github.com/qigao/salts/actions/runs/37805023643).
Linux passed 177 runtime tests and 21 installed-package tests; Linux MIR and
macOS MIR each reported 217 tests with no failures, including the native UDP/DNS
and memory32/memory64 Component fixtures. Android completed the runtime and
installed-consumer builds. Windows CI was explicitly omitted; the latest local
Windows ASAN runtime/conformance regression passed 178 tests. This qualifies
the new socket backend with the prerequisite SDK, not the published Salts SDK.

References: [WASI UDP 0.2.8](https://github.com/WebAssembly/wasi-sockets/blob/v0.2.8/wit/udp.wit),
[WASI name lookup 0.2.8](https://github.com/WebAssembly/wasi-sockets/blob/v0.2.8/wit/ip-name-lookup.wit).


## Preview1 socket descriptors and async polling

This is a separate admission gate after the implemented WASI 0.2 TCP/UDP/DNS
backend. The user authorized implementation of this public boundary. The existing
Preview1 configuration, initializer, imports and filesystem provider contracts
remain supported. The complete socket/poll gate passed the qualification below.

### Scope and evidence

Pin Preview1 to WebAssembly/WASI revision
`fae981bae14809d91f9bc2d63852d461f331d161`, specifically
[the import definitions](https://github.com/WebAssembly/WASI/blob/fae981bae14809d91f9bc2d63852d461f331d161/preview1/witx/wasi_snapshot_preview1.witx)
and [the type definitions](https://github.com/WebAssembly/WASI/blob/fae981bae14809d91f9bc2d63852d461f331d161/preview1/witx/typenames.witx).
The original `snapshot-01` tag lacks the later `sock_accept` addition and is
therefore insufficient as this gate's ABI source.

Before this gate, `wasi.h` and `wasi_preview1.c` supplied capability-gated fd
reads/writes and filesystem imports without socket imports, fdstat operations
or `poll_oneoff`. `wasi_fs.c` already owns a bounded descriptor table, guest fd
assignment, provider identities, generations and rights. Its provider bundle was
previously global to the filesystem. `wasi_native_io.h` explicitly excludes UDP
because one scalar I/O request cannot preserve a vectored datagram. The
implemented `wasi02_cnet.c` owns bounded TCP/UDP payloads, listener admission,
transport termination and reusable readiness; its TCP receive operation
currently consumes the receive buffer and cannot implement Preview1 PEEK by
simply calling a WASI 0.2 stream read.

Implement the four standard socket imports (`sock_accept`, `sock_recv`,
`sock_send`, `sock_shutdown`) together with socket `fd_read`, `fd_write`,
`fd_close`, `fd_fdstat_get`, `fd_fdstat_set_flags`,
`fd_fdstat_set_rights` and mixed fd/timer `poll_oneoff`. Hosts provide listening
TCP sockets, connected TCP streams and connected UDP sockets. Preview1 has no
standard socket creation, bind, connect, DNS or WebSocket imports; those remain
explicit host setup or the already implemented WASI 0.2 API. This gate does not
claim complete Preview1 filesystem coverage or add vendor socket imports.

The new guest imports use the pinned memory32 ABI. Iovec tables have 8-byte
entries; fdstat, subscription and event records have 24-, 48- and 32-byte
layouts respectively. Scalar carriers and output ranges are:

| Import | Core parameters | Outputs in guest memory |
| --- | --- | --- |
| `sock_accept` | fd i32, flags i32, result i32 | fd u32 |
| `sock_recv` | fd i32, iovecs i32, count i32, flags i32, size result i32, flags result i32 | byte count u32, result flags u16 |
| `sock_send` | fd i32, iovecs i32, count i32, flags i32, result i32 | byte count u32 |
| `sock_shutdown` | fd i32, directions i32 | None |
| `fd_fdstat_get` | fd i32, result i32 | fdstat record |
| `fd_fdstat_set_flags` | fd i32, flags i32 | None |
| `fd_fdstat_set_rights` | fd i32, base rights i64, inheriting rights i64 | None |
| `poll_oneoff` | subscriptions i32, events i32, count i32, result i32 | event records, event count u32 |

Every import returns errno i32. These remain Preview1 memory32 layouts even
when the runtime supports Core memory64; do not silently widen the ABI.

### Public boundary and descriptor ownership

Add `wasi_sockets.h` for a versioned, provider-neutral socket operation bundle
and descriptor admission. Add `turbowasm_wasi_preview1_config_v2` with
`size`, `api_version`, the existing config as `base`, and explicit socket/poll
configuration, plus `turbowasm_wasi_preview1_init_v2`. Do not append fields to
the existing config. New configurations have finite descriptor, wait,
subscription and per-call byte limits; socket/poll admission is opt-in.
Clock polling requires the configured clock capability and provider; enabling
socket polling does not grant clock access implicitly.

Generalize the existing filesystem descriptor table with additive versioned
binding that copies a per-descriptor operation bundle. Old binding functions
copy the existing filesystem-wide bundle into each new descriptor, preserving
old behavior. One table remains authoritative for guest fd allocation, provider
identity, rights and generation. File and socket descriptors can coexist and
`path_open` cannot accidentally allocate an fd already used by a socket. Do not
partition fd numbers into invented high ranges or create a second socket fd
allocator. Host-selected bindings and allocated descriptors must respect the
pinned Preview1 fd range. A slot whose generation is exhausted is retired,
rather than wrapping and making stale async identities valid again.

A new socket bind/move entry takes an owned provider socket identity, descriptor
kind, flags and base/inheriting rights. Success transfers ownership into the
reserved descriptor and zeros the caller's owned carrier; failure preserves
that carrier. Each descriptor copies its operation bundle and retains its
provider context until all active operations release it. The provider remains
responsible for its opaque transport identity. `sock_accept` reserves a table
slot, output range and all wait metadata before consuming an accepted connection;
commit publishes the fd without allocation. Abort preserves the queued accept
or closes a newly owned connection through exactly one cleanup path.

Fdstat reports the actual socket kind, descriptor flags and current rights.
Rights may only decrease; fd operations and accept/poll/shutdown verify their
specific rights before side effects. Accepted descriptors inherit only rights
applicable to the accepted stream. Their nonblocking flag comes from
`sock_accept`'s flags. Socket-specific fd dispatch must use the same table as
filesystem dispatch; existing configured stdio callbacks remain valid.

`fd_close` invalidates the guest descriptor once close admission succeeds, wakes
its waiters and retires its generation. Outstanding operations retain their
exact descriptor/provider identity until actual terminal cleanup. An errno
that leaves close ownership intact leaves the descriptor valid for retry, in
accordance with the existing filesystem close contract. Parent listener close
does not close accepted children. New shutdown/poll/destroy entry points return
status; they preserve owners while retained descriptors or waits remain.
The Preview1 facade must continue to outlive its bound consumer instances,
as required by the existing linker contract.

### CNet reuse and dependency direction

The implementation exposes a Preview1 operation factory and explicit move helpers from the existing
CNet adapter through `wasi_cnet.h`. The helpers move an owned listener, TCP
socket/input/output triple, or associated UDP socket/datagram pair into a
Preview1 provider identity. They validate origin, kind and quiescence before
consuming any carrier; failure leaves every input owned by the caller. Moving
reps already published to a live WASI 0.2 guest is forbidden. This avoids two
frontends independently consuming one receive queue.

Implement the transport-specific callbacks beside the existing CNet owners,
where their buffers and terminal state are authoritative. Add an optional
installed `TurboWasm::WASICNet` composition target linking the Preview1 facade
and existing CNet backend. The backend factory exchanges public provider types;
it does not call filesystem internals or make Runtime depend on WASI. Existing
WASI02CNet-only configurations remain buildable. The cost of this reuse is that
the optional Preview1 native composition also brings the existing WASI 0.2
backend dependencies. Extracting a new transport framework or owning another
CNet client per frontend would increase migration and duplicate progress;
this gate deliberately uses the established owner instead.

One serialized host still advances CNet, observes the external backend once,
routes each completion exactly once and advances readiness. No worker, native
fd exposure, second observation loop or raw-platform socket fallback is added.
The existing default-deny address policy also governs host setup and accepts.

### Transfer, wait and publication protocol

Validate all iovecs, result locations, enum/flag bits and checked total lengths
before native effects. The existing iovec count limit remains in force. Guest
spans and callback buffers are borrowed only for their live host-call lease.
A suspended frame retains identities, offsets, counts and bounded owned data;
it never retains an unchecked guest memory pointer across a suspension. Resume
reacquires output spans before publication and accounts for memory growth.
Faults before publication do not consume queued UDP packets or accepted sockets.

TCP receives preserve PEEK without consuming authoritative buffered bytes.
WAITALL aggregates a bounded request until filled, EOF, error or cancellation;
a partial result is committed according to the pinned Preview1/POSIX semantics.
PEEK plus WAITALL waits for the requested contiguous observation without consuming
it; a request above the configured retention limit fails explicitly. Nonblocking
operations return available progress or AGAIN and never park. Zero-length TCP
reads do not wait. Receive shutdown and peer EOF remain distinct from destruction.

UDP gathers all send iovecs into one bounded message, including an actual empty
message. Receive scatters one queued message across the destination iovecs;
a short destination reports `recv_data_truncated`, consumes the whole message
unless PEEK was requested, and never exposes its suffix as a second message.
PEEK preserves that same packet for a later receive. WAITALL does not aggregate
multiple UDP messages. TCP may publish a legal send prefix; UDP sends preserve
all-or-nothing message admission. Unsupported send flags fail with INVAL.

`poll_oneoff` copies a bounded subscription list before parking and reserves all
output events first. Per-entry fd errors become events with the original
userdata; invalid overall buffers/counts fail the call. Duplicate subscriptions
retain separate output positions. Readiness uses each descriptor's retained
source and reports available bytes and hangup without consuming payloads.
Clock subscriptions honor clock id, relative/absolute flags and precision using
checked deadlines. A clock-only poll remains valid. Socket and timer sources
join one backend-neutral Runtime host wait, using the established notification
owner rather than busy polling or sleeping on the execution thread. Cancellation
and fd close complete the exact wait generation, release source leases and
reservation slots once, and never cancel an unrelated subscriber's transport.

### Compatibility, qualification and rollback

Existing Preview1 users keep their initializer/configuration and fd policies.
New consumers rebuild to use the versioned socket config and per-descriptor
binding. No native handle or persistent data format is changed. Generated CMeta
adapter metadata retains existing ordinals and appends the admitted standard
imports; metadata, linker signatures and executable tests share the existing
manifest/generator path rather than a second signature table.

Qualification covers mixed file/socket fd allocation, rights reduction,
fdstat/nonblocking flags, accept capacity/OOM rollback, stale generations and
close during waits. Real IPv4/IPv6 TCP tests exercise vectored short transfers,
PEEK, WAITALL, EOF, half-close and inherited accept rights. UDP tests exercise
empty messages, PEEK, exact message boundaries and truncation. Mixed clock/fd
poll tests cover duplicates, terminal errors, relative/absolute deadlines,
cancellation, source retirement, guest memory growth and no premature storage
reuse. Add formal installed C/C++ consumers and execute Core guest fixtures
under interpreter and MIR. Continue Linux/macOS execution and Android
build/install qualification with Windows CI omitted by the user's current
instruction; local Windows ASAN remains available. Rollback disables the new
v2 socket/poll capability and optional composition target while retaining the
old Preview1 API and completed WASI 0.2 socket gates.

### Preview1 implementation and qualification boundary

The standard imports are implemented in `src/wasi_preview1_sockets.inc`, with
one canonical CMeta manifest in `src/wasi_preview1.c`. `wasi_fs.c` owns copied
per-descriptor providers, rights, flags, reservations, claims and retained
identities. `wasi_cnet.inc` projects the existing TCP/UDP owners without linking
back to the WASI facade. `TurboWasm::WASICNet` is an installed composition target.
The versioned facade opts in; legacy initializers and metadata ordinals remain
stable. The implementation intentionally covers the socket/poll admission
specified above, not the entire Preview1 filesystem/process import surface.

Transfer preflight rejects published facade bindings and source aliases. TCP
stream transfer removes the old IO-domain tokens atomically without dropping
native stream ownership. TCP move and accepted-child preparation reserve twice
the configured receive buffer; PEEK|WAITALL is bounded to `receive_bytes` so a
whole next CNet receive chunk fits while preserving existing bytes. UDP move
reserves one `datagram_bytes` scratch buffer for vectored send; native sends
retain their own bounded copied payloads. Failure preserves input carriers.
Cancellation ends direction claims and peek demand, while transport requests
retain their existing CNet terminal lifecycle. Providers borrow context through
all identities and leases; native adapter destruction rejects retained slots.

The owner explicitly advances readiness after transport progress and schedules
clock deadlines using the nanosecond `next_timeout` query. No new transport
observation loop or Runtime cancellation callback is introduced. A suspended
Runtime frame unwinds through host-wait's INTERRUPTED return; the adapter frees
its bounded staging and propagates that status. A copied poll list retains each
subscription separately, including duplicates. Accepted children use current
inheriting rights at publication, allowing safe rights reduction during waits.

Implementation commit `79515a51ad5e2d3c317c4f15123eeccc99ae44a8` passed
[native CI run 37818815010](https://github.com/qigao/turbowasm/actions/runs/37818815010).
The run consumed the successful platform SDK artifacts from
[Salts prerequisite run 37805023643](https://github.com/qigao/salts/actions/runs/37805023643),
commit `9cf0af7504f32ea0773830e93dfdbd10e4c15ef1`; Windows CI was explicitly
skipped. Qualification results:

- Linux Release: 179 CTest entries passed, followed by 24 installed C/C++
  consumer tests, including the public-only descriptor, real CNet and C++ tests.
- Linux MIR and macOS MIR Release: each CTest graph completed with 221 entries
  and no failures. Both new MIR variants passed; the fixtures assert that
  executed guest wrappers reached compiled MIR state.
- Android arm64-v8a: the complete configured graph built and installed,
  including both public headers. No device execution was performed.
- Local Windows ASAN: `ctest --preset win-core3-asan-user --output-on-failure`
  passed all 180 entries, including both pinned Core 3 conformance suites.
  The installed ASAN consumer graph passed all 23 entries using the
  `ci-win-user` configure/build/test presets in `tests/installed_consumer`.

The socket tests exercise actual IPv4/IPv6 TCP and connected UDP, cancellation,
partial receive, PEEK/WAITALL, truncation, empty datagrams, FIN and descriptor
ownership. Descriptor tests cover rights reduction, accepted-child rights,
guest-memory validation and growth during suspension, clocks, duplicate poll
subscriptions, capacity exhaustion, close and shutdown.

Integration with the `master` CNet substrate from #413/#414 preserves its
single-owner external progress and terminal listener-drain contracts through the
completed public CNet/IO API. The early private initializer and polling entry
points are superseded by the public IO domain, reusable pollables and explicit
shutdown API; their regression tests now exercise those public entry points.
A separate listen-ready state keeps a completed start-listen ready until
finish-listen consumes it, before accept readiness takes over. Repeated advance
does not submit duplicate accepts, and cancellation cannot reuse the physical
socket slot before its terminal completion is routed. Both ordinary close and
adapter shutdown are covered. The Runtime static library also retains #419's
position-independent-code setting for embedding in shared libraries.

## Metallic C11 command guests

The approved first three stages add an optional wasm32 guest SDK and a separate
`turbowasm-run` command. Metallic is consumed directly from `guest/metallic`,
with a local source override through `TURBOWASM_METALLIC_SOURCE_DIR`; its headers and allocator run inside
guest linear memory. Salts remains a native host dependency, never a guest libc.
Configuration does not fetch, copy or patch the sources. Libc fixes are maintained
in the local source tree and the C guest tests qualify its behavior.
The initial audit used upstream `66ea0f480a16a9341be94ed4e66be28b3c3802d5`.
LLVM produces LTO objects in a conventional
archive, with the command CRT linked explicitly. This avoids requiring
`llvm-link` while retaining the upstream compilation model. wasi-sdk remains an
alternative for applications needing its larger sysroot; importing another
libc into the runtime would duplicate guest state and is unnecessary.

The runner owns one module, instance, Preview1 adapter and bounded descriptor
table. Arguments and explicitly supplied environment entries are copied by the
adapter. Standard streams are borrowed host streams; guest close revokes their
descriptor without closing the host stream. A directory is available only when
explicitly passed by the user. Guest memory, module size, descriptor count and
execution fuel are bounded. `proc_exit` is recorded separately from traps and
fuel exhaustion. Cleanup destroys the instance before the adapter and closes
every remaining descriptor before destroying the filesystem provider.

The filesystem provider appends optional `path_rename` and `set_flags` callbacks.
Existing zero-initialized source consumers retain their behavior but must
recompile (provider struct size changes). Rename validates both descriptor
rights and paths before dispatch; different provider contexts return XDEV.
Flags are committed to the descriptor table only after the provider accepts
them. HostFS uses secure Salts root-relative operations, never host path
concatenation. Append is supported; unsupported flag combinations report NOTSUP.
HostFS refuses rename while any child directory descriptor is live with BUSY,
because these descriptors currently retain root-relative paths. The preopened
root is exempt; ordinary C file streams do not open child directory descriptors.

Metallic patches address command exit handlers, allocation failure, checked
heap growth, aligned allocation admission and temporary-file lifetime. Temporary
files are exclusively created in the explicitly preopened current directory and
unlinked immediately; neither a global host temp directory nor shell execution
is granted. Failed unlink closes the newly created descriptor and reports the
error. This profile is single-threaded and does not claim complete C11:
threads, general locale/fenv behavior and upstream long-double
soft-float gaps remain outside these stages.

Validation uses compiled C guests through the real runner and formal filesystem
tests, including exit order, allocation limits, args/environment, rename,
append, temporary files and clock conversion. The dependency and profile are
optional; disabling the guest build restores the native-only build graph.
Reverting the provider extension requires rebuilding native consumers.

Restartable character conversions retain the existing UTF-8 encoding and
32-bit `mbstate_t` layout. The caller owns explicit state; each API owns its
separate implicit state when `ps` is null. Input and output arrays are borrowed
only for the call, and a conversion retains at most one partial UTF-8 sequence
or UTF-16 surrogate in that state. Encoding uses at most four output bytes and
does not allocate. Incomplete input returns `(size_t)-2`, a pending low surrogate
returns `(size_t)-3` without consuming input, and invalid sequences report
`EILSEQ`. State after an encoding error is unspecified; callers must reinitialize
it before reuse. Null input follows the equivalent empty-string/NUL conversion
specified by C11, rather than discarding a pending partial sequence. This does
not add locale selection or thread support. The governing contracts are
[N1570 sections 7.28 and 7.29.6](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf).

### Guest stdio buffering

`setvbuf` selects unbuffered, full or line buffering before stream I/O starts.
Existing streams remain unbuffered until explicitly configured. A caller buffer
is borrowed until `fclose` or `freopen`; otherwise the stream allocates exactly
the requested capacity (BUFSIZ when size is zero). Capacity never grows, must fit
PTRDIFF_MAX, and is bounded by guest linear memory. Allocation failure leaves the
previous configuration intact. `FILE` remains opaque in installed headers;
only its private implementation changes, so guests relink the updated libc.

The single-threaded stream owns its pending output and input read-ahead state.
Only configured streams enter an intrusive list owned by guest stdio; no separate
registry allocation or host-global state is introduced. Full buffers and line
endings trigger writes; a short host write is retried, while an error retains
the unwritten suffix and sets the stream error indicator. `clearerr` followed by
`fflush` may retry that suffix. The caller must not resend a failed write's data
without accounting for partial progress. Input refills retain unread bytes;
EOF is set only after an actual zero-byte read. The pushback cache stays separate.

Position queries account for buffered bytes and do not clear EOF. Successful
position changes discard read-ahead and pushback, and clear EOF; failed seeks
retain unread data. `fflush(NULL)` drains every buffered output stream. Normal
exit runs handlers and then drains all outputs; quick exit and `_Exit` do not.
Closing or reopening unregisters the stream and frees only libc-owned buffers,
even when flushing fails. String-formatting pseudo-streams retain their existing
callbacks and are never registered. No host runtime or public WASI API changes
are needed. Regression covers visibility before/after flush, line endings,
input positions, borrowed and owned buffers, allocation and write failures,
and the three exit paths.

### Metallic Reactor and guest libraries (#425)

Metallic remains the guest C11 library for the Reactor, threads (#426) and SJLJ
(#427) work. These are separate capabilities; a single-threaded Reactor does not
require either threads or SJLJ. Existing command guests keep their startup and
exit behavior.

The Reactor links a separate `crt1-reactor.o` and exports `_initialize : () -> ()`.
Initialization seeds the heap before calling constructors and returns to its
host. A second or recursive initialization traps before changing the heap. The
host must initialize once before using business exports. There is no implicit
`main`, process exit, or destructor pass after a business call. Applications
explicitly release persistent objects and flush output in their close export;
destroying an instance does not run `atexit` handlers.

The installed CMake entry `turbowasm_add_c_reactor(name SOURCES ... EXPORTS ...)`
selects this CRT. `turbowasm_add_c_guest_library(name SOURCES ...)` compiles a
Wasm archive. Both accept `INCLUDE_DIRECTORIES` and `COMPILE_OPTIONS`; programs
also accept `LIBRARIES` naming guest library targets or explicit archive paths.
The existing `turbowasm_add_c_guest(name source)` accepts the same additional
program arguments without changing old callers. Guest targets do not link
native Salts archives. Object depfiles preserve header rebuilds, and archives
are rebuilt from their exact source list so removed members cannot survive.

The embedding example owns each instance's lifecycle and borrows its module,
source bytes, providers and capabilities. It rejects a Wasm start section and
checks `_initialize` before instantiation; initialization and business calls
use explicit fuel limits. One active operation is admitted per session, with
concurrent/reentrant operations rejected instead of queued. Successful business
error returns leave the instance usable; traps, interruption, exceptions and
fuel exhaustion retire it. Failed instances receive host teardown only, never
another guest cleanup call. Explicit destruction requires quiescence; timing
out a caller is not permission to free a running instance. Provider instances
outlive consumers. This is example orchestration over the existing Runtime API,
not a second public Runtime ABI or a tool gateway.

CMeta qualification cross-compiles portable CMeta sources with the guest CRT
and Metallic headers. Metadata, object pointers and callbacks remain guest-local;
host reflection needs an explicit ABI adapter. Cross-instance pointers require
a separately specified shared-memory/allocator contract. Native thunks remain
excluded. Rollback disables the optional guest additions; command guests and
native consumers retain their existing interfaces.

### Metallic non-local jumps (#427)

The selected implementation uses LLVM's Wasm SJLJ lowering and standard Wasm
exception handling. The compiler creates continuations in the calling function;
Metallic implements `__wasm_setjmp`, `__wasm_setjmp_test` and `__wasm_longjmp`.
The `jmp_buf` stores only guest invocation identity, a compiler label and a
two-field exception payload. No native address or host jump buffer crosses the
memory boundary. This changes the previous placeholder layout: all guest
objects and archives using `setjmp.h` must be rebuilt together.

MIR's special handling of native `setjmp` illustrates the need to preserve the
interpreter PC, but cannot restore an already returned host helper frame.
Minicoro's Wasm backend uses Asyncify to preserve coroutine stacks; adding that
whole-program transformation would introduce another tool and execution model.
Neither is required by this path. The inspected local references are
TurboScript `5e190f4d6256d67e9aea03dafdc7488d16d3b00e`'s `vendor/mir/mir-interp.c`
and Salts `25388632d80150cf3f6bd31dcf8ddc02525f732f`'s `vendor/minicoro/minicoro.h`.
Their native switching implementation is not copied into the guest library.
The implemented compiler ABI follows
[LLVM's lowering contract](https://github.com/llvm/llvm-project/blob/llvmorg-21.1.1/llvm/lib/Target/WebAssembly/WebAssemblyLowerEmscriptenEHSjLj.cpp).
The [SDK integration reference](https://github.com/WebAssembly/wasi-sdk/blob/main/SetjmpLongjmp.md)
documents the corresponding compiler and LTO flags. Metallic remains the libc.

Guest compilation requires LLVM 20 or newer and enables SJLJ with standard EH
at both compilation and LTO linking. LTO also selects `-exception-model=wasm`;
the helper definitions compile without LTO because LLVM introduces declarations
for these symbols in its late lowering pass. The linker explicitly retains the
three compiler helper symbols from the archive. There is no success-returning `setjmp`
fallback: omitting compiler lowering leaves an unresolved symbol. The helper
payload is part of the target buffer, so nested live targets need no global
scratch state or allocation. LLVM restores the guest shadow stack on landing.
The Runtime propagates ordinary Wasm exceptions through its existing unwind
paths, retaining fuel, trap and resource accounting in interpreter and JIT.

Only a still-active invocation on the same C thread is a valid target. Returning
to the host ends that invocation; Reactor persistence does not extend it.
Cross-thread, stale-buffer and cross-instance jumps are undefined C behavior
and do not acquire access outside Wasm memory. Synchronous host imports may
return normally between setjmp and longjmp; a guest jump must not unwind a live
host callback or async suspension. No host continuation API is added. C11's
volatile-local rules apply, and longjmp does not perform application cleanup:
in particular it cannot bypass outstanding CMeta structured-scope obligations.
Rollback requires rebuilding the SDK and all affected guests, not mixing old
four-byte placeholder buffers with the new helpers.

### Metallic C11 threads and bounded worker ownership (#426, selected)

The thread-enabled SDK is a separate wasm32 profile using imported shared
memory, Wasm atomics, bulk memory, native TLS and the existing legacy
`wasi.thread-spawn` / `wasi_thread_start` ABI. The single-threaded archive and
CRT remain separate. The full C11 surface is required before installing or
advertising this profile: thread lifecycle, plain/recursive/timed mutexes,
conditions, once and thread-specific storage with destructor iterations.

Runtime atomic waits currently block a native worker in
`turbowasm_instance_memory_wait_internal`. The borrowed CFlow Executor protocol
does not expose a reserved-worker count. Queuing a child on a saturated pool
while its parent waits can therefore prevent progress. Making more queue slots
available does not solve this. Converting all waits to coroutine suspension
would additionally require owner-affine resumption and wake/cancel integration.

The approved additive host entry is:

```c
turbowasm_status turbowasm_wasi_threads_init_pool(
    turbowasm_wasi_threads *threads, size_t capacity);
```

It creates a private CFlow worker pool with exactly `capacity` workers and an
admission limit of `capacity` live children. The pool is not exposed for unrelated
tasks. The root executes outside this pool. A queued admitted child always has
a worker not occupied by another live child once short task-finalization work
finishes; excess/nested spawn fails with the existing capacity error before
guest join can depend on an unadmitted child. Application lock cycles can still
deadlock. This bounds workers and avoids claiming coroutine scheduling that does
not exist. The existing initializer retains its borrowed-executor semantics.

`threads` must be zero-initialized; zero/unsupported capacity returns
`INVALID_ARGUMENT`, resource setup failure returns `OUT_OF_MEMORY`, and failure
leaves it empty. Destruction rejects live children and calls from the owned
pool. Once quiescent, it drains task finalizers before joining/destroying only
its owned executor. Module, imported backing and providers must outlive all
actual child terminals. Group exit interrupts/wakes children but does not prove
quiescence. The borrowed variant never shuts down its caller's executor.

Guest records have bounded slot/generation identities distinct from host TIDs.
Stacks/TLS are allocated before spawn with checked alignment and size arithmetic.
A failed spawn rolls back the uncommitted record. Joinable results remain until
join or detach; detached records may be reaped only after a stack-free completion
epilogue publishes terminal state. The epilogue must not access the released
stack/TLS after publication. Destructors run before publication. Normal return
and `thrd_exit` are local to a thread; process exit and abnormal termination
remain group-terminal. SJLJ targets and errno belong to each thread's TLS.

Every non-OK child invocation result is group-terminal, including uncaught Wasm
exceptions and host callback failures. An accepted executor task cancelled before
run publishes INTERRUPTED/NONE before finalization destroys its child instance.
Neither path can publish the guest epilogue's join word, so both use the existing
group interrupt and shared-memory waiter registry instead. The first fatal or
proc_exit result wins; sibling unwind and later cancellations cannot replace it.
Rejected task admission still rolls back only that spawn. This adds no asynchronous
guest cancellation and never shuts down a borrowed executor from a child callback.
Borrowed-executor cancellation is delivered by a worker; it does not interrupt
already-running callbacks. A host shutting down a saturated borrowed pool must
publish group exit before waiting for idle, so blocked children can unwind and
workers can deliver queued cancellations. The owned pool's worker-per-child
admission rule avoids dependency on an unrelated queued task for normal progress.

Root startup initializes shared data, allocator, root TLS and constructors once.
Child startup installs its own stack/TLS before entering C and never resets live
shared data. Compiler-generated shared-data initialization must be examined and
tested, including repeated child instantiation. Reactor close stops admission
and drains actual terminals before releasing the shared group; timeout retains
live resources instead of pretending teardown completed.

The libc audit covers allocator/heap locks, FILE operations and the stream list,
environment/preopen initialization, implicit conversion/time buffers, random
state and exit-handler registration. Timed waits use real clock values and
predicate loops. CMeta metadata remains guest-local; reflection neither makes
mutable objects thread-safe nor enables cross-thread managed GC stores.
Tests must include saturated/nested spawn, rollback and slot reuse, TLS/stack
isolation, destructor iterations, contended allocation/I/O and terminal wakeup.
Rollback disables the optional profile; existing guest artifacts do not relink
to its ABI implicitly.

The owned host pool is implemented. The private `tests/guest/thread_abi.c`
qualification uses fixed per-child stacks/TLS and compiler-generated shared
initialization; it does not install a partial C11 threads library. LLVM 21 emits
a `__wasm_init_memory` start function with an atomic once guard, passive data
segments and a per-instance `__tls_base`. The tests verify repeated siblings
preserve initialized and zero-filled data after mutation, run constructors only
on the root, reset child TLS on reuse, isolate errno/SJLJ/stack addresses, reject
root and nested over-capacity spawn, and reject live/worker destruction. The
internal C11 implementation below adds actual compiled guest qualification;
the full libc locking audit and installed profile remain required by #426.
The conversion-state portion of that audit now uses `_Thread_local mbstate_t`
for implicit UTF-8/UTF-16/UTF-32 and legacy multibyte conversion state. The
thread fixture interleaves distinct incomplete sequences/surrogate pairs on
the root and both children, including child TLS reuse. Explicit state still
belongs to the caller and must not be shared concurrently without coordination.

The internal C11 implementation uses 32 child records plus one root record,
128 generation-checked TSS keys and four destructor passes. A child reserves
128 KiB of shadow stack and at most 64 KiB of TLS, with linker-provided size
and alignment checked before allocation. A thread handle is a slot/generation
pair, never a host TID or native pointer; exhausted generations are retired.
One registry lock protects admission, join/detach claims and key identities.
Join waits on a release-published terminal word; only the stack-free assembly
epilogue can publish it. Detached records are reclaimed at subsequent admission
or explicit group drain, so retained storage remains bounded. A join claim
excludes detach and another join, and failed spawn frees only its reservation.
During drain, completed joinable records remain available until every child
has terminated, allowing live children to finish their own joins. Reclamation
preserves the terminal word until later admission, so a drain waiter cannot
miss completion when a concurrent join releases the same record.

Mutex owners use the current guest slot identity; recursive depth overflow is
an error. Lock/unlock use acquire/release atomics and predicate-loop wait/notify.
Conditions capture an atomic 64-bit sequence before unlocking their mutex,
wait only while that sequence matches, and reacquire before returning on either
success or timeout. Sequence exhaustion fails rather than wrapping. Timed
mutex/condition operations use absolute TIME_UTC deadlines and checked values;
sleep uses a monotonic deadline. Yield uses a short timed atomic wait to offer
the native worker to other runnable threads without requiring a nonstandard
host import. It is not a coroutine suspension facility.

Internal allocator locking uses dlmalloc's custom-lock hook, with no pthread
dependency. Registry/libc locks may acquire allocator locks, never the reverse.
Callbacks and TSS destructors run outside the registry lock. The root initializes
its record before constructors. Child return and thrd_exit converge on one TSS
destructor path; root thrd_exit drains children and exits the process with zero.
The implementation remains under a private, non-installed threaded include/source
directory until command/Reactor integration and the complete libc audit pass.
`tests/guest/c11_threads.c` exercises create/join/detach/exit, stale handles,
once, recursive/contended mutexes, timed condition waits, broadcast, sleep,
TSS deletion/destructor passes, concurrent allocation and admission recovery.
Environment initialization uses checked allocator-owned storage and call_once
publication in this profile; its established empty-on-provider-error behavior
is retained. The allocator remains the sole owner of program-break mutation.

### Shared-memory host copies for threaded WASI (#426, selected)

The first compiled C11 timed-wait test exposed an integration gap: Preview1
clock output uses `turbowasm_host_call_memory_span`, whereas Runtime deliberately
rejects shared-memory raw spans. Removing that guard would expose data races and
growth-invalidated pointers. WASI needs protected copies across its public
Runtime boundary, rather than including Runtime's private instance structures.

Approved additive entries in `link.h` use the existing host-call context:

```c
turbowasm_status turbowasm_host_call_memory_check64(
    turbowasm_host_call *call, uint32_t memory, uint64_t address,
    uint64_t length, turbowasm_trap *trap);
turbowasm_status turbowasm_host_call_memory_read64(
    turbowasm_host_call *call, uint32_t memory, uint64_t address,
    void *destination, size_t length, turbowasm_trap *trap);
turbowasm_status turbowasm_host_call_memory_write64(
    turbowasm_host_call *call, uint32_t memory, uint64_t address,
    const void *source, size_t length, turbowasm_trap *trap);
```

They support shared and unshared memories and memory32/64 addresses, reuse
Runtime's existing range checks/read-write locks, and allocate nothing. NULL
buffers are valid only for zero length. Host buffers must not alias guest
storage. OOB returns TRAPPED with MEMORY_OUT_OF_BOUNDS; invalid arguments return
INVALID_ARGUMENT. The trap is NONE on other outcomes. Failed copies do not
partially transfer. No pointer or lock survives the call. Check validates one
range without accessing data; subsequent operations recheck their own ranges.
The existing span functions remain unshared-only and unchanged.

Preview1 scalar output, arguments/environment, paths and vector I/O migrate to
these entries. Input descriptors/payloads are snapshotted before provider calls;
outputs are copied back only after provider completion. No memory lock is held
across callbacks, filesystem I/O, suspension or wait. Existing transfer limits
are retained where present; newly copied synchronous vector transfers use a
1 MiB per-call bound and return Preview1 NOMEM when over budget, without invoking
the provider. This limit and its compatibility cost must be documented for the
opt-in shared profile; unshared calls retain their existing behavior. Input
snapshots do not make concurrently mutated application buffers meaningful C.

Tests must cover actual shared-memory clock/stdio, imported memory identity,
OOB/overflow, zero length, failed copy preservation, limits before side effects,
and growth/concurrent access. Rollback removes the additive entries and leaves
the threaded profile private; it must not relax the raw-span guard.

The protected Runtime APIs, Preview1 clock, args/environment and vector
fd_read/fd_write are implemented. Tests cover shared and unshared memory32/64,
two importers observing the same storage before/after growth, failed-copy
preservation, and compiled guest threads concurrently performing short vector
I/O and first-time environment lookup. The vector tests check the exact 1 MiB
boundary, rejection above it, OOB before provider effects, zero vectors, and
provider errors/over-reported reads without publishing output. Random,
path and v2 socket/poll projections still require migration;
this checkpoint does not qualify complete threaded Preview1 or libc support.

Fixed filesystem outputs (`fd_seek`, `fd_tell`, `fd_filestat_get`,
`fd_prestat_get`) use an 8- or 64-byte call-local record. Output ranges are
checked before provider effects; the record is published with a protected
copy only on success. `fd_prestat_dir_name` checks the full requested range
but copies only the actual preopen name, leaving trailing bytes unchanged.
These paths allocate no transfer storage and retain no guest pointer across
callbacks. Provider-triggered memory growth therefore cannot invalidate the
output location. Error precedence and little-endian layouts remain unchanged
for unshared guests. Protected copies alone do not protect the descriptor table
or borrowed preopen names against concurrent close/rebind. The next section's
admission protocol supplies that lifetime protection; remaining shared path
projection and threaded guest integration are still qualification requirements.

`fd_readdir` checks the complete caller buffer and byte-count output before
provider calls, then publishes each successful entry from the existing bounded
24-byte header plus at most 255 name bytes. It holds no guest pointer while
enumerating and allocates no transfer buffer. Truncated headers/names, cookies,
embedded NUL name bytes and zero-length enumeration keep their existing ABI.
As before, a later provider failure leaves any already-written prefix in the
buffer and leaves the byte-count output unchanged; this is not an all-or-nothing
directory snapshot. A failed entry itself is never published. Different entries
are separate protected copies and do not promise an atomic directory listing.

### Concurrent filesystem admission and provider lifetime (#426, selected)

Protected guest copies alone do not protect the descriptor table: provider calls
previously borrowed a slot while close could consume its file and a later bind
could reuse its metadata. `path_open` opened the provider resource before finding
descriptor capacity, then ignored a rollback-close error. Threaded guests need
one lifetime protocol across WASI FS, HostFS and Preview1 preopen projection.

The approved change uses the existing public functions and opaque owners; it
does not change public struct layouts. Initialization and destruction remain
exclusive host lifecycle operations. Destruction still requires all identities,
reservations and in-flight calls to have drained. The caller must prevent new
API entry while destroying an owner; a mutex cannot keep a freed owner alive.

A single lock across provider I/O was rejected because one blocked file would
stop unrelated descriptors and provider reentry could deadlock. Waiting inside
close was also rejected: a callback may need its caller to make progress. Short
admission plus BUSY preserves explicit ownership and independent progress at the
cost of caller-visible retry, while per-file HostFS locks serialize only native
state that cannot safely overlap.

- A short-held table mutex protects slot state, generation, rights, flags,
  reservations, reference counts and lookup. An admitted synchronous call pins
  the generation and copies provider operations/identity before dropping the
  mutex. Provider callbacks, allocation, native I/O and waiting never execute
  under the table mutex. Pins are bounded checked counters, not heap records.
- Close with an in-flight synchronous operation, another close, or a pending
  metadata transaction returns BUSY without invoking the provider or consuming
  the descriptor. Close otherwise marks CLOSING, invokes the provider without
  the mutex, then commits retirement on success or restores LIVE on error.
  Calls attempting admission while CLOSING also return BUSY. This preserves
  the existing retryable-close ownership contract without waiting on a callback
  which might itself need the closing caller to make progress.
- Different descriptors can execute concurrently. The table does not serialize
  arbitrary custom provider I/O: providers admitted to concurrent execution
  must support that concurrency. Flags use a per-slot metadata transaction so
  provider acceptance and the table value cannot commit in opposite orders.
  Rights only decrease; already-admitted operations retain their admitted
  authority. Child publication intersects requested rights with the parent's
  current inheriting rights, as the existing socket accept path does.
- `path_open` reserves a free descriptor and fd before the provider callback.
  A full table returns MFILE with no create/truncate/open effects. Provider
  failure aborts the reservation; success publishes into that reservation with
  no further allocation or fallible bind. A successful provider open must
  produce a valid owned identity, and an error must transfer no identity.
  This deliberately changes full-table error precedence and removes the
  ignored rollback-close path. Reserved slots and retired generations cannot
  be reused; generation exhaustion fails admission rather than wrapping.
- Existing socket retain/release leases remain distinct from synchronous pins.
  They retain their close-while-parked behavior and same-progress-thread callback
  contract. Table locking does not authorize CNet callbacks on guest workers;
  the threaded owner-dispatch bridge is a separate integration requirement.
- Public descriptor-info/preopen enumeration keeps its borrowed-name contract:
  callers must coordinate close while consuming that pointer. Preview1 uses an
  internal pinned preopen snapshot through the protected copy, so no freed
  name crosses its callback boundary. No new public lifetime carrier is added.

HostFS mirrors operation admission for its root and bounded child identities.
Its table mutex only manages identity state and admission. Per-identity locks
serialize native file-position/append operations and directory cursors without
blocking unrelated files. Root-directory lazy initialization uses the same
root-operation serialization. A separate namespace admission transaction makes
the existing no-live-child-directory rename rule atomic against directory
open/close; native filesystem calls run outside the table mutex. All temporary
paths remain bounded by the configured path capacity. Close retains the
existing ownership-consuming native-close translation. External providers keep
their own cancellation/progress contracts; these locks introduce no asynchronous
cancellation guarantee for blocking native I/O.

Validation must include barrier-controlled blocked read versus close/rebind,
independent descriptor progress, callback reentry, duplicate close, close error
retry, full-table open with no provider effects, reserved-slot races, rights
reduction during open, flags commit ordering, retained preopen copies and stale
generations. HostFS tests cover concurrent files, cursor/append consistency,
root readdir initialization and rename/open exclusion. Existing v2 socket,
Preview1, WASI 0.2 and installed consumers remain regression gates; native race
diagnostics are additional evidence and do not instrument guest C code.

Migration requires callers racing close with synchronous operations to handle
BUSY and retry after their in-flight operation finishes; no ABI rebuild is
required solely for this internal layout change. Rollback keeps the threaded
profile private and removes its concurrent-filesystem claim; the single-owner
socket protocol and protected Runtime memory APIs remain independent.

### Threaded Metallic libc synchronization (#426, internal)

The internal threaded archive appends a recursive lock and flush-reference
state to its private FILE layout; the single-threaded archive keeps its layout.
Each public stream operation holds that FILE lock for its complete operation,
including provider I/O. Different FILEs remain independently usable; there is
no process-wide lock held across blocking I/O. Nested byte/wide formatting uses
the same recursive owner. LLVM cleanup attributes release guards on ordinary
C returns; guest traps/process termination retire the entire group and do not
permit subsequent use of abandoned libc state. Non-local jumps across libc or
live provider frames remain outside the supported contract.

The existing buffered-stream list has a separate short-held lock. A flush-all
walk selects its next live stream in guest-address order and acquires a bounded
temporary reference while holding only the list lock, then drops that lock
before locking/flushing the FILE. Concurrently removed streams are skipped;
streams added during traversal may or may not participate. No snapshot array,
unbounded allocation or borrowed next pointer crosses the list lock. A stream
close marks retirement and removes the stream before releasing its FILE lock,
then waits for outstanding flush references before freeing it. This prevents
both use-after-free and a close/flush lock cycle. Lock order is FILE -> list or
allocator; flush traversal never holds the list lock while acquiring a FILE.
Reopen resets ordinary stream state while preserving its live lock/reference
metadata. Application calls still must not use a FILE after it is closed.

Automatic line-buffer flushing occurs at the outermost input operation before
acquiring its FILE lock. Nested reads do not start a second cross-stream walk.
This avoids opposite FILE lock ordering between concurrent input operations.
Error flags, wide orientation, pushback, buffering and stream-list mutation are
covered by the same operation locks. Tests must include indivisible formatted
records, concurrent reads/writes on distinct streams, close against flush-all,
wide I/O, short/error returns and lock release after failure.

The two bounded exit-handler registries serialize push/pop independently and
release their locks before calling application handlers, including handlers
that register further callbacks. The random generator preserves its existing
sequence with an atomic state transition; signal-handler publication uses an
atomic function pointer and raise calls it without internal locks. Default
terminating signals call _Exit directly so abort cannot recursively raise
SIGABRT forever. Implicit strtok and calendar/text-time buffers are TLS;
the fixed C locale remains immutable. Preopen discovery uses once publication,
and temporary-file sequence allocation is atomic (exclusive creation remains
the authority against pathname collisions). These changes do not qualify the
still-pending shared filesystem/provider path or asynchronous host signals.

Preopen initialization retains provider errors, unknown tags, oversized names
and exhausted name storage as terminal initialization errors; callers never see
a partially discovered set. Formal compiled-guest tests cover the synchronization
above and controlled filesystem callbacks, including concurrent first lookup,
error retention, unknown errno/tag admission and no truncated name request.
They do not substitute for concurrent HostFS/provider qualification.
