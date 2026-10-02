# TurboWasm.Native

Prebuilt **Runtime + synchronous Component facade** TurboWasm SDK install trees for downstream native consumers.

The package contains four RIDs:

- `linux-x64`
- `windows-x64`
- `macos-arm64`
- `android-arm64-v8a`

Each RID contains the installed `TurboWasm::Runtime` and
`TurboWasm::Component` targets, public `turbowasm/*` headers, and CMake
package metadata.

This package intentionally does **not** publish NuGet dependency metadata.
Consumers restore their own latest compatible Salts SDK and discover both
packages through CMake.

The package continues to exclude MIR/JIT, CFlow, NativeIO, WASI Preview1 and
WASI 0.2 adapters. The only published optional facade is the stable synchronous
`TurboWasm::Component` API; all other runtime integrations remain separate.
