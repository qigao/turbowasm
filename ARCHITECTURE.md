# TurboWasm Architecture

## Ownership

TurboWasm owns WebAssembly-specific semantics:

```text
Wasm bytes
   -> bounded decoder
   -> validation
   -> typed Wasm IR
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

CFlow / Executor
  -> reusable execution orchestration where the generic model fits
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

The refinement maps to canonical CMeta vector descriptors. Execution initially
uses `Salts::SIMD`.

This is intentionally different from making CMeta or CFlow understand Wasm
opcodes. CMeta owns vector/mask type semantics; Salts::SIMD owns portable
execution; TurboWasm owns Wasm instruction semantics.

## Execution tiers

Planned execution is tiered:

```text
validated Wasm
     |
     +-> baseline interpreter
     |
     +-> hot scalar function -> MIR lowering -> native code
     |
     '--> SIMD -> Salts::SIMD initially
                  -> future MIR V128 lowering when profitable
```

MIR is a code-generation backend only. TurboWasm remains responsible for all
sandbox semantics before and during lowering:

- linear-memory bounds;
- table bounds and indirect-call type checks;
- Wasm trap behavior;
- fuel/interruption/resource policy;
- host import capability boundaries.

No backend is allowed to weaken these checks.

## Module lifetime

A `turbowasm_module` is a zero-initialized opaque owner handle. The bootstrap
loader currently validates the Wasm magic/version header and borrows immutable
input bytes. Later parsing/annotation data belongs to the private implementation
without changing the public carrier.

## Roadmap

1. Header/module bootstrap and typed SIMD foundation.
2. Full section decoder and validation.
3. Baseline interpreter with annotation/predecode support.
4. Imports/exports, tables, globals and linear memory.
5. WASI/host binding through generated CMeta/DataBind adapters.
6. Lazy MIR backend for eligible hot scalar functions.
7. SIMD region fusion and native MIR V128 lowering.
8. Threads/EH/relaxed SIMD qualification.
