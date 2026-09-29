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
