# Preview1 smoke corpus

The normal TurboWasm test build consumes the checked-in Wasm byte arrays in
`wasi_smoke_corpus_test.c`. It does not download or select a wasi-sdk, WABT,
or another Wasm tool version.

The fixtures are intentionally tiny and reviewable. They test composition
through the public Preview1 linker rather than compiler-specific libc behavior:

- **process-info** calls `args_sizes_get`, `environ_sizes_get`,
  `clock_time_get`, and `random_get` from one guest start function.
- **stdio** constructs guest iovecs and calls `fd_write` and `fd_read` from
  one guest start function.
- **preopen-memory-fs** calls `fd_prestat_get`,
  `fd_prestat_dir_name`, and `fd_readdir` against a deterministic
  in-memory provider. It cannot reach ambient host paths.

## Regeneration policy

Regeneration is a developer aid, not a CI dependency. Any WebAssembly tool
capable of producing the equivalent MVP modules may be used. Before replacing
a byte array:

1. keep the import namespace exactly `wasi_snapshot_preview1`;
2. preserve the documented import signatures and start-function behavior;
3. inspect the binary diff and keep fixtures small;
4. run `turbowasm_wasi_smoke_corpus_test` plus the normal test suite;
5. do not add a pinned tool version, manifest baseline, or download step to
   TurboWasm CMake/CI solely to regenerate these fixtures.

The fixture source semantics are encoded in comments and assertions beside the
byte arrays. Host-provider behavior is deterministic so the smoke test remains
portable across Linux, Windows, and macOS.
