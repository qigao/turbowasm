# Build troubleshooting

## Slate 0.1.14 and the split SDKs

- The Windows LLVM-MinGW preset reads SDK paths from `.env`. The candidate's
  `vcpkgTriplet` is a literal identifier in Slate 0.1.14. An environment reference
  there is treated as a directory name. Keep the ABI triplet in
  `slates/Platforms.json` and select the restored payload with
  `VCPKG_INSTALLED_DIR`. Check the result with `slate doctor --preset release`.
- SaltsBase publishes `simd.h` and `native_io.h` at its include root.
  TurboWasm exposes headers with the same names. The third-party manifest
  declares Ninja copy tasks for the `salts/` include paths, with the installed
  SDK headers as inputs. TurboWasm's umbrella and WASINativeIO headers locate
  their adjacent TurboWasm headers before including Salts headers. The Release
  and Debug test presets exercise this layout.
- Chttp 0.1.6 exposes Application lifecycle and omits the engine lifecycle
  and route declarations from `http_server/http.h`.
  The opt-in `http-guest-tests` preset exposes that compile failure without
  binding to private symbols. The Chttp guest example uses the public client
  API. Migrating the server fixture requires a public Application-based
  fixture that preserves the existing timeout, drain, and no-replay checks.
- With SaltsCNet 0.1.20, Windows exit status `0xC0000135` on CNet tests
  indicated a missing DLL. The SDK's `cnet.dll` imports `salts_crypto.dll`.
  Stage both DLLs from the same `SALTSCORE_ROOT` through the CNet target's
  `runtimeBins`. The guest sockets, Preview1 CNet and WASI 0.2 CNet tests
  exercise loading from the staged paths.
- The Metallic build task takes its vendor source list from the same
  manifest that declares its Ninja inputs. Update `guest/module.json` when
  the local Metallic checkout changes. `slate test --preset guest-tests
  --label guest` verifies the command and Reactor paths plus the threaded
  CRT, filesystem, and sockets fixtures.

## CMeta 0.1.11 with Metallic C11

The published CMeta source revision compiles its existing 19-unit guest
subset with `guest/build_guest.py` in single- and multi-threaded profiles.
The source-based fixture uses the CMeta headers from the matching checkout.
Clang C11 syntax checks on all 21 units passed for 20 in both guest profiles.
The failing unit was `core_data.c`. The `-H` trace runs from
`cmeta/core_data.h` through `core/tstr.h`, `core/vstr.h`,
`memory/buffer.h`, and `threads/threads.h` to `threads/threads_types.h`.
That header includes `pthread.h` whenever `_WIN32` is absent.

The SaltsBase 0.1.11 buffer header declares only buffer types and operations.
Its implementation already includes the thread header for the native pool
locks. In an isolated header copy, replacing the public thread include with
`core/api.h` passed standalone native C11 and C++17 syntax checks. Removing
the public thread include also let Clang compile `core_data.c` to a Wasm
object. Its SaltsBase link requirements are `tstr_cmp`, `tstr_freep`,
`tstr_len`, and `tstr_new_len`. Linking the full guest target still requires
a guest-compatible string implementation.

SaltsBase `v0.1.11` did not configure for Metallic with Clang targeting
`wasm32-unknown-unknown` and CMake `Generic`. The required `FindThreads`
check failed its pthread probes before the `salts` target (`Salts::Core`)
could build. Core's default source selection includes its POSIX thread
backend. Independent C11 object builds against Metallic failed at
`pthread.h` for `core/src/tstr.c` and at `strings.h` for `core/sds/sds.c`.
Removing the public thread include alone does not provide the string
implementation needed by `core_data.c`.

Metallic's shared-memory profile supplies its own C11 thread API. The
existing `wasm/tests/guest/c11_threads.c` fixture compiled with Clang
using `-matomics` and `__METALLIC_THREADS__=1` without a `pthread.h`
dependency. The `guest` Slate suite passed all 15 targets. The SaltsBase
0.1.11 thread header also declares its own `thrd_t` and `mtx_t`, with
layouts different from Metallic's. A Core Wasm backend must select one
C11 provider and implement the remaining `sync_*` operations.
With only the buffer include corrected in a temporary header copy,
compiling `tstr.c` proceeded to the SDS dependency on `sys/types.h`.
The SDS declarations require `size_t` and signed `ssize_t`. Replacing
`sys/types.h` with `stddef.h` in a syntax-only probe left `ssize_t`
undefined. Adding Metallic's `stdio.h` made the declarations compile
and verified that its `ssize_t` is a signed 32-bit `ptrdiff_t` on wasm32.
The separate Core filesystem implementations use POSIX calls and also
consume `off_t` and `mode_t`.

The SaltsBase source refactor replaces SDS's private `ssize_t` with
`ptrdiff_t` from `stddef.h`. `tstr_split` now checks `PTRDIFF_MAX` before
converting the input length. The edited SDS header passes wasm32 C11
syntax checks with Metallic. The `debug-mingw` Slate formatting suites
pass against the edited Core. TurboWasm's selected SaltsBase 0.1.11 SDK
predates this change.

SDS uses `tolower` on unsigned bytes for Wasm case comparisons. Both
Wasm `tstr` comparison functions now delegate to SDS. The native paths
retain their CRT calls. The existing guest builder linked both SDS and
`tstr` comparison exports with the Metallic archive. Direct Wasm calls
verified case folding, ordering, bounded input, NUL and high-byte
handling. The `tstr.c` build used a temporary buffer header copy with
the pthread dependency removed. Metallic malloc's unrelated `strings.h`
include is inside its disabled `USE_BUILTIN_FFS` branch.

The SaltsBase source now includes `core/api.h` directly from `buffer.h`.
`buffer.c` retains the thread include for its pool locks and one-time
initialization. The native `debug-mingw` Slate suite passed all 121
targets. Using the updated Core headers and `tstr.c`/`sds.c` sources,
Metallic built all 21 CMeta 0.1.11 units and linked the existing command
fixture with `cmeta_tstr_cmeta_data` retained in both profiles. The
single-threaded fixture exited successfully via `turbowasm-run`.

Metallic's threaded header can be used from Clang C11 and C++17. Its
condition sequence uses Clang's named 64-bit atomic type so C++17 can
parse the declaration with `-pedantic-errors`, while the wasm32 C11
type matches the previous sequence representation. The four
Core-compatible fixed-width time entry points share the existing
Metallic wait paths. The guest suite covers invalid nanoseconds,
duration overflow and mutex reacquisition after a condition timeout.

## Eight-target native CI (split SDK migration)

The old native job referenced removed ci-* presets and Salts.Native 2.3.0-*.
The fixture generator still used fixtures/wasm/src after the source move; its
source is now wasm/tests/wasi02/fixtures/src. CI uses the exact versions in
.env.example, the shared curl-based vcpkg-cache transport, and all eight
targets in cmake/ci/native-matrix.json. Desktop targets run CTest; Android
cross-builds and installs (no device runner). Windows uses LLVM-MinGW;
Windows ARM64 has no published MIR port. Intel macOS remains a required job
but the pinned upstream releases lack x64-osx / macos-clang-x64 assets.
Publish matching upstream releases before claiming eight-target qualification.
The legacy monolithic artifact override and Metallic guest manual dispatch
are removed from the native workflow; guest qualification needs separate work.
