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
