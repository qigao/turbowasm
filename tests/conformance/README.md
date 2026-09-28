# TurboWasm WebAssembly spec conformance harness

This directory is test infrastructure only. It is not installed with
`TurboWasm::Runtime`.

## Data flow

```
pinned WebAssembly/spec .wast
        |
        v
wast2json (WABT from qigao/vcpkg-cache)
        |
        v
run_core.py
  JSON -> TWCF1 line manifest
        |
        v
turbowasm_spec_runner
  persistent modules / instances / linker
        |
        +-- pass
        +-- fail
        '-- unsupported
```

The Python layer does not execute WebAssembly. It only converts WABT's JSON
format into a deliberately small manifest. The C runner is the semantic test
surface and uses only public TurboWasm APIs.

## First slice

The initial `core-smoke.txt` suite intentionally covers a small, pinned set
of upstream core files. The harness currently executes:

- valid modules and persistent instances;
- module registration through `turbowasm_linker`;
- invoke actions;
- exact scalar i32/i64/f32/f64 results;
- multi-value exact results;
- null funcref results;
- mapped TurboWasm trap kinds;
- binary assert_invalid / assert_malformed;
- binary assert_unlinkable / assert_uninstantiable.

Unsupported WAST shapes are reported separately rather than counted as passes.
Current deliberate unsupported cases include text-only negative modules,
`get` actions, NaN result patterns, v128 result comparison, externref/exnref,
and non-null reference literals.

The suite should expand in small reviewable waves. A new upstream family should
not be added to the gate until its result semantics are represented correctly
by the harness.


## First baseline findings

The first pinned smoke run produced:

```
pass        1475
fail           5
unsupported  332
total       1812
```

Three failures were WABT 1.0.41 conversion gaps on newer typed-reference
syntax and are now classified as tooling-unsupported. The two remaining
failures were real Runtime validation gaps in `global.wast`; #141 added
the narrow extended-const rule for reading a prior immutable local global.

After #141, this foundation gate is expected to have zero Runtime failures
for the current smoke slice while still reporting unsupported coverage
separately.


## Wave 2

The next gate enables WABT typed function-reference parsing and adds core
control/call/integer/memory families:

- `br.wast`, `br_table.wast`, `return.wast`;
- `call.wast`, `call_indirect.wast`;
- `i32.wast`, `i64.wast`;
- `load.wast`, `store.wast`, `memory_grow.wast`.

This deliberately does not add `imports.wast` or `linking.wast` yet.
Those files mix host/provider and lifecycle semantics that remain tracked by
#122; adding them now would dilute execution-engine failures with known
composition-layer unsupported coverage.

The converter uses only `--enable-function-references`, not
`--enable-all`, so proposal syntax is enabled narrowly rather than turning
unrelated proposals into accidental test inputs.


## Wave 3

The third gate isolates baseline memory semantics:

- `address.wast`
- `align.wast`
- `endianness.wast`
- `memory.wast`
- `memory_trap.wast`
- `memory_redundancy.wast`
- `float_memory.wast`

This wave focuses on address arithmetic, memory bounds, alignment hints,
little-endian layout, memory declarations/traps, redundant memory operations,
and scalar float load/store bit preservation.

Large arithmetic suites such as `f32.wast`, `f64.wast` and
`conversions.wast` remain a separate numeric wave so memory failures are not
mixed with NaN/result-policy coverage.

`memory.wast` contains a small number of import cases. Those remain subject
to the existing explicit unsupported accounting; this wave does not broaden
the host-linking scope tracked by #122.


## Wave 4

The fourth gate isolates scalar floating-point and conversion semantics:

- `f32.wast`
- `f64.wast`
- `f32_cmp.wast`
- `f64_cmp.wast`
- `conversions.wast`

Exact non-NaN f32/f64 results continue to compare raw IEEE-754 bits. Upstream
`nan:canonical` / `nan:arithmetic` result classes are still reported as
unsupported by the manifest converter until the harness models permitted NaN
sets explicitly; they are never counted as passes.

Keeping NaN-policy work explicit lets this wave expose ordinary arithmetic,
comparison, truncation, reinterpretation and conversion bugs without weakening
the gate.


## Wave 3A

The next gate expands the memory engine before adding reference-heavy table
families:

- `memory.wast`, `memory_trap.wast`, `memory_redundancy.wast`;
- `data.wast`;
- `bulk-memory/memory_copy.wast`;
- `bulk-memory/memory_fill.wast`;
- `bulk-memory/memory_init.wast`.

This wave intentionally stops before table/ref suites. Those carry much more
`externref`, registration and host-linking coverage, so they remain a
separate Wave 3B. That keeps memory validation/execution failures attributable
to the memory engine rather than composition-layer unsupported behavior.


## Wave 3B

After memory/bulk-memory is clean under ASan, the next gate adds reference and
table semantics:

- `table.wast`, `table_get.wast`, `table_set.wast`, `table_grow.wast`;
- `ref.wast`, `ref_null.wast`, `ref_is_null.wast`, `ref_func.wast`;
- `elem.wast`;
- `bulk-memory/table_copy.wast`, `table_fill.wast`, `table_init.wast`.

The Runtime currently carries complete cross-instance `funcref` identity, so
funcref/table failures are treated as real gaps. Non-null `externref` still
has no public Runtime value carrier and remains explicit unsupported coverage.
Imports/registers that require host modules not defined by the WAST file also
remain unsupported rather than being fabricated by the harness.


## Qualified proposal gates

Proposal selectors start as manual diagnostics. A proposal graduates into the
normal pull-request/push gate only after Runtime semantics are implemented.

Tail calls are the first graduated proposal. The qualified gate runs the pinned
`WebAssembly/tail-call` suites `return_call.wast` and
`return_call_indirect.wast` and requires:

- zero conformance failures;
- at least one real upstream pass (so an all-unsupported run cannot be green);
- no Runtime-originated unsupported command.

Harness-level unsupported WAST shapes remain reported separately; they are not
silently counted as passes. MIR tail-call lowering is not required for this
gate: tail-call functions remain per-function interpreter fallback until an
exact native tail semantic is implemented.
