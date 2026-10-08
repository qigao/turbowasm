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
the private import extension below adds retained cross-instance calls. Imported
resource borrowing remains gated until its loan owners are integrated. Local resource/endpoint call transactions
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
borrow handle is created. Imported/cross-instance borrowing requires separate
task-bound handle accounting and is not admitted by this local codec.

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
Cross-instance resource import identity and progress are separate integration work.

Private async instance imports reuse the existing unambiguous capability router.
An optional resolver selects an already initialized provider exec and async lift;
instantiation validates the complete signature and retains the provider until the
consumer is destroyed. Call frames stay on the consumer's bounded progress list,
while the callee task counts against the provider's task quota. Drivers explicitly
poll each exec with outgoing calls; no recursive scheduler or worker is introduced.
Resources, including resource leaves inside nested endpoint payloads, remain
rejected at this boundary until imported nominal identities and
borrowed-handle accounting are connected. Other supported canonical values use
the existing per-direction transaction codecs.

Result realloc needs the callee's retained execution control while entering the
consumer's memory and builtin context. Each call therefore owns a copy of the
result realloc context with an explicit progress task. Both domains retain the
auxiliary guard through suspension; context builtins select this auxiliary task,
whose fresh slots are restored on success or unwind. The consumer's may_leave
guard prevents ordinary canonical reentry during realloc. Abort first unwinds
exported caller tasks, then their outgoing calls; provider teardown refuses live
import bindings and inbound tasks. Public async admission remains closed while
host ownership, imported resources and general synchronous interop are completed.

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
exec. Imported equivalence classes will be mapped to their provider's retained
tokens before binding resource-bearing calls; until then those imports remain
closed. Rollback destroys only view-owned nodes/headers/tokens and leaves the
decoded metadata untouched. Synchronous public admission retains its current
contracts while this private integration is completed.

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
