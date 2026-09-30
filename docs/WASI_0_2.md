# TurboWasm WASI 0.2 typed capability boundary

TurboWasm treats WASI Preview1 and WASI 0.2 as separate compatibility
surfaces.

```text
Preview1 guest
  -> wasi_snapshot_preview1 flat imports
  -> TurboWasm::WASI

Component guest
  -> typed Component import identity
  -> WASI 0.2 descriptor / capability layer
  -> TurboWasm::Component
```

The WASI 0.2 path does not extend the Preview1 import namespace and does not
place WIT types in the Core Runtime ABI.

## W1 source baseline

The first retained registry is pinned to the archived official WASI proposal
repositories at their current 0.2.8 WIT surface:

- `WebAssembly/wasi-clocks@71e486b1b44a49687dbe17d5b979d6da6112c7f2`
- `WebAssembly/wasi-random@bd54965b22082b3e157b2fb4bc77987c33ae7d5f`
- `WebAssembly/wasi-cli@e922fd7bd137cd284a5e6c4815a5a630d32fdd01`
- external `wasi:io/poll@0.2.8#pollable` identity from
  `WebAssembly/wasi-io@3983fe1feab6b3a3b4e5c47c8b13daaf22266f00`

W1 retains the stable functions from:

- `wasi:clocks/wall-clock@0.2.8`
- `wasi:clocks/monotonic-clock@0.2.8`
- `wasi:random/random@0.2.8`
- `wasi:random/insecure@0.2.8`
- `wasi:random/insecure-seed@0.2.8`
- `wasi:cli/environment@0.2.8`
- `wasi:cli/exit@0.2.8`

The unstable `cli-exit-with-code` function is deliberately excluded from the
stable W1 registry.

## Descriptor model

WIT semantic types are retained independently from the executable Component
type graph. The registry can describe:

- primitive scalars and strings;
- aliases;
- lists;
- tuples;
- records;
- options;
- results with explicit unit arms;
- cross-package resource identity.

This distinction is necessary because WASI WIT uses structural types such as
`datetime`, `tuple<u64,u64>`, `list<tuple<string,string>>`,
`option<string>` and `result` that are broader than the initial executable
Component subset.

The registry is immutable process-lifetime data. W1 performs no host call,
allocation, linker registration or guest execution.

## Layering

W1 builds only when the Component layer is enabled. Its target is internal and
not installed yet. W5 owns the final installed WASI 0.2 target and public ABI.

Later slices consume these exact descriptors:

- W2: clocks/random/CLI provider execution;
- W3: filesystem/preopen resources;
- W4: streams/poll over restartable execution and NativeIO;
- W5: installed target and real-toolchain qualification.

Sockets remain tracked independently by #309.
