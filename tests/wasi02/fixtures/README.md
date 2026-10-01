# WASI 0.2 toolchain smoke fixtures

These sources are Component Model text inputs. The checked-in `.wasm` files are
generated with a pinned upstream `wasm-tools` release by the manual
`wasi02-fixture-generator.yml` workflow.

Generation is intentionally separate from normal CI. Normal tests consume the
checked-in binaries and perform no network fetch.

Initial W5c1 coverage:
- `monotonic-clock.wat`: `wasi:clocks/monotonic-clock@0.2.8.now`
- `random-u64.wat`: `wasi:random/random@0.2.8.get-random-u64`
- `cli-exit.wat`: `wasi:cli/exit@0.2.8.exit`

Resource-bearing filesystem/preopens and streams/poll fixtures are tracked as
the W5c2 continuation because their nominal-resource/canonical-memory source
must remain explicit rather than being approximated by scalar smoke code.
