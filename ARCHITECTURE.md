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
executable MIR mapping budget           implemented

WASI Preview 1 capability layer         implemented
WASI filesystem provider ABI            implemented
Salts HostFS / optional littlefs         implemented
WASI NativeIO async fd projection       implemented
threads/shared memory/atomics           implemented + upstream qualified
legacy WASI threads adapter             implemented
caller allocator/resource policy        implemented
memory64                                unsupported; tracked by #304
WebAssembly GC                          future work
Component Model                         tracked by #307
WASI 0.2 typed interfaces               tracked by #308
native vector JIT backend               backend-dependent future work
```
