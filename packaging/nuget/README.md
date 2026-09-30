# TurboWasm.Native

Prebuilt **Runtime-only** TurboWasm SDK install trees for downstream native consumers.

The package contains four RIDs:

- `linux-x64`
- `windows-x64`
- `macos-arm64`
- `android-arm64-v8a`

Each RID contains the installed `TurboWasm::Runtime` target, public
`turbowasm/*` headers, and CMake package metadata.

This package intentionally does **not** publish NuGet dependency metadata.
Consumers restore their own latest compatible Salts SDK and discover both
packages through CMake.

The first Runtime package also intentionally excludes MIR/JIT, CFlow, NativeIO,
WASI and Component adapters. Those capabilities remain separate from the
portable Runtime contract.
