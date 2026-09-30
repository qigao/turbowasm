# TurboWasm Component Model ownership boundary

This document defines the first Component Model implementation boundary for
TurboWasm. The baseline is the synchronous Component Model MVP at
`WebAssembly/component-model@a25fc0b372dd21f07f0242c46e98bd0f1ea0c0e1`.

## Layering

```text
Component bytes / WIT-level semantics
        |
        v
Component-owned retained type graph
        |
        +-- canonical ABI metadata
        +-- resource type identity
        +-- typed imports/exports
        |
        v
ordinary TurboWasm Core modules / instances / linkers
```

The Component layer does not replace or extend Core
`turbowasm_validation_context`. Core validation remains the source of truth
for Core Wasm modules. Component types are retained separately and refer to
each other through stable integer type ids rather than C pointers.

## C1 type graph

The initial internal graph covers:

- primitive Component scalar kinds;
- `string`;
- `list<T>`;
- nominal `resource` identities;
- `own<resource>`;
- `borrow<resource>`.

The graph is allocated as an exact set of indexed slots. Definitions may refer
to another reserved type id before that target node is defined. Final graph
validation requires every slot to be defined and every indexed reference to be
valid.

Resource identities are nominal and must be unique inside a graph. C1 does not
create runtime resource handles; generation-safe handle tables and
`resource.new/drop/rep` belong to C4.

## CMeta boundary

Primitive Component scalar kinds may project to canonical CMeta type
descriptors for reflection and host adapter matching:

```text
bool        -> cmeta_type_bool
s8/u8       -> cmeta_type_int8 / uint8
s16/u16     -> cmeta_type_int16 / uint16
s32/u32     -> cmeta_type_int32 / uint32
s64/u64     -> cmeta_type_int64 / uint64
float32     -> cmeta_type_float
float64     -> cmeta_type_double
char        -> cmeta_type_uint32 carrier
```

This projection is read-only. CMeta does not become the source of truth for
Component types, and `string`, `list`, resources, `own`, and `borrow`
are deliberately not collapsed into single C ABI descriptors.

## Allocation ownership

The graph uses TurboWasm runtime allocation helpers, so graph construction
inherits the active `turbowasm_runtime_config` allocation policy. C1 adds no
new allocator or global registry.

## C2 binary container boundary

The pinned MVP component preamble is:

```text
00 61 73 6d   # \0asm
0d 00         # pre-standard Component version
01 00         # Component layer
```

C2a reuses TurboWasm's bounded reader for the outer container. Section ids are
single bytes in the range 0 through 12 and section payload lengths are u32
LEB128. The first implementation retains borrowed section spans without
interpreting later canonical/instance semantics.

Core module sections (id 1) are special: their payload remains a borrowed span
of the original Component bytes and is validated by the existing Core module
loader. TurboWasm does not create a second Core decoder inside the Component
layer.

Custom sections validate their length-prefixed UTF-8 name but otherwise remain
semantically ignorable. Nested component sections validate the Component
preamble boundary in C2a; recursive semantic decoding is deferred.

## C2 semantic bootstrap

C2b adds retained semantic records for the synchronous MVP subset needed by
later canonical ABI work.

Supported type definitions:

- primitive scalar and string definitions;
- `list<T>`, where `T` is either an inline primitive/string or an earlier
  Component type index;
- nominal `resource` definitions with retained Core representation type and
  optional destructor index;
- `own<R>` / `borrow<R>` for an earlier resource type;
- synchronous `func` types with named parameters and zero or one result.

Component `valtype` is modeled explicitly as either an indexed type or an
inline primitive. TurboWasm does not synthesize fake primitive nodes in the
Component type index space.

The initial import/export record layer retains synchronous function imports
and function exports with optional function-type ascription. It validates type
indices against the incrementally-built type index space but does not yet
instantiate or link Component functions.

The following forms fail closed as `TURBOWASM_UNSUPPORTED` in this slice:

- aliases, because they can add type aliases to the incremental type index
  space;
- type imports/exports, which also extend that index space;
- async functions;
- record/variant/tuple/flags/enum/option/result and gated newer value types;
- component/instance type definitions and their imports/exports;
- name-attribute vectors beyond the legacy 0x00/0x01 forms.

These forms are not silently skipped: doing so would change subsequent index
meaning.

## C3 canonical ABI metadata

The first canonical ABI slice implements the deterministic, allocation-free
metadata calculations used before any guest-memory lifting/lowering occurs.

For the currently retained synchronous value subset, TurboWasm follows the
pinned MVP rules exactly:

- bool and 8-bit values: alignment/size 1, flat i32;
- 16-bit values: alignment/size 2, flat i32;
- 32-bit integers and char: alignment/size 4, flat i32;
- float32: alignment/size 4, flat f32;
- 64-bit integers: alignment/size 8, flat i64;
- float64: alignment/size 8, flat f64;
- string and dynamic list: two pointer-width fields, aligned to pointer width;
- own/borrow: alignment/size 4, flat i32.

Both i32 and i64 canonical memory pointer widths are modeled.

For synchronous function flattening, the pinned limits are:

```text
MAX_FLAT_PARAMS  = 16
MAX_FLAT_RESULTS = 1
```

If flattened parameters exceed 16, they collapse to a single pointer parameter.
If a result flattens to more than one Core value, lift returns one result
pointer while lower appends a result pointer parameter and returns no Core
result. Thus synchronous lowering can contain 17 final parameters in the
boundary case of 16 direct parameters plus one result pointer.

C3 in this slice is metadata-only: it does not read/write linear memory, invoke
`realloc`, validate UTF encodings, or create resource handles. Those execution
operations remain later C3/C4 work.

## C3 canonical ABI boundary

C3a retains deterministic canonical layout and flat-Core carrier metadata for
the supported synchronous value subset.

C3b adds the first execution-time memory codec. It operates on an explicit Core
instance/memory plus an explicit realloc callback. This keeps canonical memory
semantics independent from Component instantiation:

```text
Component value
    |
    v
canonical layout/type metadata
    |
    +-- Core instance + memory index
    +-- memory32 or memory64 pointer width
    +-- explicit realloc callback
    |
    v
Runtime memory bounds/read/write helpers
```

The first codec supports:

- Component integer/float/bool/char scalars;
- UTF-8 strings;
- dynamic lists recursively containing the currently-supported scalar/string/
  list subset.

Lifted string/list host storage uses the owning Core module's Runtime allocator
scope, so #300 per-allocation policy applies. Lowering never writes directly to
a raw guest pointer: guest storage is obtained through the realloc callback and
all reads/writes are checked by Runtime memory helpers.

Pinned safety limits are retained for strings/lists (2^28-1 bytes), invalid
UTF-8/bool/char values trap, memory32/memory64 pointer-width mismatches fail
closed, and recursive value traversal is depth-bounded.

Resource handle lift/lower is deliberately excluded from C3b and remains C4.
UTF-16 and latin1+utf16 string encodings are not yet exposed by this first
memory codec.

## C4 resource ownership boundary

Resource runtime state is stored in a Component-owned handle table rather than
Core tables or raw host pointers.

Handles are 28-bit values compatible with the Component canonical i32 carrier:

```text
bits  0..15  slot + 1
bits 16..27  generation
```

Slot zero and generation zero are invalid. A freed slot increments its
generation before reuse. When generation 4095 is consumed, the slot is
permanently retired instead of wrapping, preventing an old stale handle from
becoming valid again through ABA reuse.

Each entry retains the nominal Component resource identity, its Core
representation value, ownership state, and active lend count.

The first synchronous resource primitives implement the state needed by
`canon resource.new/rep/drop`: new creates an owned handle; rep validates
generation plus nominal resource identity; drop is rejected while lends are
active; successful drop invalidates the handle before an optional destructor
callback.

The destructor callback is an explicit semantic operation. Generic table
destruction only releases host bookkeeping and never runs guest code.
Binding a resource destructor to a Core function remains C5 composition.

The table has an explicit maximum entry count (at most 65535) and uses Runtime
allocation helpers, so #300 per-allocation limits apply. Async borrow scopes are
not introduced here; C4 exposes lend acquire/release state only, while
task/subtask borrow-scope lifecycle remains deferred to the concurrency layer.

## C5a Core call composition

The first composition adapter implements the synchronous `canon lift`
direction for the retained scalar/string/list subset.

A binding borrows:

- one retained Component function type;
- one ordinary `turbowasm_instance` and Core function index;
- canonical memory/realloc options when the flattened ABI requires memory.

Initialization computes the pinned flat signature and requires the target Core
function's reflected parameter/result carriers to match exactly. Invocation
then performs:

```text
Component values
    -> canonical lower_flat / indirect parameter tuple
    -> turbowasm_instance_invoke()
    -> canonical lift_flat / indirect result memory
    -> Component value
```

The adapter never calls interpreter/JIT internals directly and never creates a
second Core execution path. Core traps/status values propagate through the
ordinary public invocation contract.

Dynamic string/list arguments allocate through the explicit guest realloc
option. More than 16 flat parameters use the canonical indirect tuple layout.
A string/list result uses the single canonical result pointer. memory32 and
memory64 bindings are distinguished by the selected Core memory type.

Resource `own`/`borrow` transfer is intentionally rejected in C5a; it is
composed with the C4 generation-safe resource table in C5b.

## C5b1 resource call composition

Top-level Component resource values are represented abstractly by their Core
resource representation (`i32` or `i64`), never by a canonical handle index.
Canonical handles exist only inside a Component resource table.

The synchronous Core-call adapter can optionally bind a C4 resource table:

- lowering `own<R>` creates an owned canonical handle and transfers ownership
  to the callee table;
- lifting an `own<R>` result consumes an owned handle without running the
  destructor and returns the abstract representation;
- lowering `borrow<R>` creates a transient non-owned handle;
- the Core call must explicitly drop every transient borrow before returning;
  otherwise the adapter cleans the leaked transient handle and returns a
  canonical trap status;
- Component function results transitively containing `borrow` are rejected,
  matching the pinned MVP validation rule.

A private resource-type binding couples one nominal resource type to a C4 table,
its exact i32/i64 representation carrier, and an optional destructor callback.
This is the reusable boundary C5c can expose as `canon resource.new/rep/drop`
Core host functions.

C5b1 intentionally supports top-level own/borrow values only. Resource handles
nested inside lists or future composite types remain C5b2 because their
in-memory canonical representation needs an explicit synchronous borrow scope.

## C5c1 minimal executable Component binary

The first binary composition path executes a closed synchronous scalar
Component assembled from the pinned MVP binary records:

```text
embedded Core module
  -> core instance (instantiate, zero args)
  -> alias core export ... (core func)
  -> canon lift (no options)
  -> Component func export
```

The decoded binary retains explicit records for the Core instance, Core
function alias and canonical lift index spaces. Execution reloads the borrowed
Core module through the ordinary Runtime loader, instantiates it with
`turbowasm_instance_create()`, resolves the aliased Core export by name, and
constructs the existing C5a typed Core-call adapter.

No interpreter/JIT entry point is called directly. A Component export therefore
executes through the same `turbowasm_instance_invoke()` path as every other
Core call.

C5c1 intentionally rejects:

- Core instantiate arguments;
- canonical options;
- Component imports;
- non-Core-function aliases;
- unsupported Component export sorts.

C5c2 adds memory/realloc options, linked instance arguments and C5b resource
built-ins without weakening this ownership boundary.

## C5c2a linked instances and canonical memory options

The executable binary subset now accepts two additional pinned-MVP constructs.

### Core instantiate arguments

A Core instance definition may carry `with` arguments of the binary form:

```text
core-name 0x12 core-instance-index
```

Each argument must reference an already-created Core instance. At execution
time TurboWasm creates an ordinary `turbowasm_linker`, defines each provider
instance under the encoded Core module namespace, and instantiates the consumer
through `turbowasm_instance_create_linked()`. Component execution therefore
does not duplicate Core import matching or type checks.

### Canonical memory and realloc options

C5c2a decodes and retains:

- default or explicit UTF-8 string encoding;
- `memory <core-memory-index>`;
- `realloc <core-function-index>`.

UTF-16, latin1+UTF-16, post-return, async and callback options still fail
closed.

Core memory export aliases occupy their own Core-memory index space. Canonical
memory may be exported by a different Core instance than the lifted callee.
The memory's actual memory32/memory64 type selects the canonical pointer width.

The realloc option may likewise refer to a Core function from another instance.
Before use, its reflected signature must be exactly:

```text
(ptr, ptr, ptr, ptr) -> ptr
```

where `ptr` is i32 for memory32 and i64 for memory64. The guest realloc
callback invokes that function only through `turbowasm_instance_invoke()`.

This separation is important: Component canonical options name Core index-space
items, not fields implicitly owned by the lifted callee's Core instance.

## C5c2b resource built-ins in executable Components

The binary executor now retains and executes the synchronous pinned canonical
resource built-ins:

```text
canon resource.new R  -> core func (rep(R)) -> i32
canon resource.rep R  -> core func (i32) -> rep(R)
canon resource.drop R -> core func (i32) -> ()
```

These built-ins occupy the ordinary Core-function index space. An inline Core
instance may export them, and a later embedded Core module may receive that
inline instance through an ordinary Core instantiate argument. The consumer is
still instantiated with TurboWasm's existing linker; the Component layer does
not invent a second Core import path.

All built-ins share the same generation-safe C4 resource table used by C5b
`own`/`borrow` canonical transfer. Resource type bindings therefore enforce
the same nominal identity, representation carrier, stale-generation, lend and
owned-vs-borrowed rules everywhere.

When a resource type declares a Core destructor, `resource.drop` routes that
destructor through the unified Core function map and ultimately through
`turbowasm_instance_invoke()`. A borrowed handle never triggers the
destructor.

C5c2b supports inline Core instances whose exports are resource built-in Core
functions. General inline Core exports and Component-instance composition
remain C5c3.

## C5c3 inline Component instances

The first Component-instance composition slice supports the pinned synchronous
inline instance form:

```text
(component func N)
    -> inline Component instance export "name" func N
    -> alias export instance "name" (func)
    -> new Component function index
```

Decoded inline instances retain named Component-function exports. A later
Component export alias creates a new function index that resolves to the same
C5a/C5b canon-lift adapter as the source function. Adapters, Core instances,
resource tables and realloc contexts are therefore not copied when a function
is re-exported through an instance.

The executor keeps an explicit:

```text
Component function index -> canon-lift adapter index
```

map. This removes the earlier temporary assumption that a Component function
index was identical to its position in the canon-lift record array.

Nested Component instantiation (`instance 0x00 componentidx ...`) still fails
closed. It requires recursive Component execution and Component import binding,
which belongs to a later layer rather than being simulated with Core linker
state.

## C5b2 nested resource values

C5b2 extends the synchronous resource transfer boundary from top-level
`own<R>` / `borrow<R>` values into canonical memory representations such as
dynamic lists and indirect parameter tuples.

The generic canonical memory codec remains independent from C4. It accepts
optional resource lift/lower callbacks attached only by the C5 composition
layer:

```text
Component list/tuple value
    -> canonical memory recursion
    -> resource lower/lift callback
    -> one C5 synchronous call scope
    -> C4 generation-safe resource table
```

The call scope tracks every canonical handle created while lowering, including
handles nested inside lists or inside the >16-parameter indirect tuple. Its
tracking storage uses the target Core module Runtime allocation policy.

- lowering failure rolls back every handle created by that attempt;
- Core traps clean transient borrows while preserving transferred owned
  handles, matching the C5b1 top-level rule;
- a successful synchronous call must have explicitly dropped every borrowed
  handle; a leaked nested borrow is cleaned and the adapter traps;
- nested `own<R>` results consume the Core handle while recursively lifting
  the result value;
- borrow-containing result types remain rejected transitively by Component type
  validation.

Both memory32 and memory64 canonical list representations use this same scope.
No C4 dependency is introduced into the generic C3 canonical codec.

## Deferred work

C1 intentionally does not implement:

- Component binary decoding;
- canonical ABI layout/flatten/lift/lower;
- resource handle instances;
- component instantiation;
- WASI 0.2;
- concurrency/task/stream/future semantics.

Those are tracked in later #307 slices. The synchronous MVP boundary must stay
separate from the newer concurrency definitions so scheduler semantics do not
leak into Core Runtime.
