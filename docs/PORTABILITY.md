# TurboWasm portability and embedding boundary

TurboWasm keeps the installed Runtime core independent of direct desktop OS
APIs. The portable baseline is C11 plus the explicitly linked Salts SDK
capabilities.

## Runtime baseline

`TurboWasm::Runtime` requires:

- a C11 compiler and standard integer/size types;
- the hosted C memory/string subset currently used by Runtime
  (`malloc`, `calloc`, `realloc`, `free`, `memcpy`, `memmove`,
  `memset`, `memcmp`);
- `Salts::CMeta` and `Salts::SIMD` as public semantic/SIMD dependencies;
- `Salts::Coroutine` and `Salts::Platform` only as private Runtime
  implementation dependencies;
- `libm` on Unix-like targets where the toolchain separates it.

The Runtime source does not directly include Win32, pthread, POSIX file, socket,
`mmap`, or `VirtualAlloc` APIs. Platform-heavy services are carried by Salts
or by optional TurboWasm adapter targets.

The current remaining hosted-libc assumption is allocation: Runtime validation,
linking, instance state, and restartable execution still allocate through the
standard C heap. Caller-supplied allocation/resource control is tracked
separately by #123.

## Optional target boundary

| Target | Extra host dependency | Runtime required? |
| --- | --- | --- |
| `TurboWasm::Runtime` | Salts CMeta/SIMD; private Coroutine/Platform | yes |
| `TurboWasm::WASI` | Runtime only | no |
| `TurboWasm::WASIHostFS` | private `Salts::Core` root-relative filesystem capability | no |
| `TurboWasm::WASILittleFS` | externally supplied littlefs source | no |
| `TurboWasm::CFlow` | `Salts::CFlow` | no |
| `TurboWasm::NativeIO` | `Salts::NativeIO` | no |
| `TurboWasm::WASINativeIO` | WASI + NativeIO adapters | no |
| `TurboWasm::WASIThreads` | `Salts::CFlow` + private Platform | no |
| MIR backend | external MIR package/cache | no; not exported by Runtime |

No native fd/HANDLE, MIR type, littlefs type, or NativeIO/CFlow implementation
type enters the public `TurboWasm::Runtime` ABI.

## Interpreter-first embedded profile

The supported portability qualification includes an Android arm64-v8a
cross-build that intentionally disables:

- MIR JIT;
- CFlow and NativeIO adapters;
- WASI threads and WASI NativeIO;
- littlefs;
- command-line tools;
- executable tests/conformance runners.

It builds and installs `TurboWasm::Runtime`, `TurboWasm::WASI`, and
`TurboWasm::WASIHostFS`, then cross-compiles installed-package consumers with
the Android NDK. This is the canonical non-desktop, interpreter-first
qualification. Android binaries are not executed on the Linux CI host.

## Tools and tests are not Runtime requirements

Developer tools and conformance tests may use host stdio/filesystem APIs such
as `fopen`, and some tests contain explicit Win32/POSIX setup code. Those
dependencies are outside the installed Runtime library and are disabled by the
embedded Android profile.

The DataBind CMeta adapter-plan generator is also tooling-only. The checked
Preview1 plan is plain C11 data; normal Runtime/WASI consumers do not link
SaltsUtils/DataBind.

## Host services and unsupported scope

Host services are capability opt-ins. A build that omits an adapter does not
implicitly gain that host service.

- filesystem access requires a WASI filesystem provider; the generic WASI
  target has no ambient host filesystem access;
- async host I/O requires the optional NativeIO bridge;
- WASI threads require the optional threads adapter;
- MIR is optional and restartable execution remains interpreter-based;
- memory64, WebAssembly GC, and the Component Model are not part of the current
  completed Runtime surface.

## Dependency acquisition

CMake package discovery is intentionally unversioned. CI restores current
published SDK/package artifacts and validates target/capability presence.
TurboWasm CMake must not add numeric dependency versions or `EXACT` package
constraints.
