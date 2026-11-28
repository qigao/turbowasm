# Native CI qualification

The native workflow builds the complete configured graph, runs formal CTest on
native desktop runners, and installs the SDK. Android x86_64 and arm64-v8a are
cross-built and installed; no device execution is claimed. Windows uses
LLVM-MinGW, Linux/macOS use the runner Clang. MIR is selected only for the
desktop platforms covered by its published port contract; Windows ARM64 and
Android use the interpreter profile. No installed-consumer project is run.

SDKs are restored from the immutable stable-release asset inventory in
`cmake/ci/sdk-lock.json`: SaltsBase 0.1.11, CMeta 0.1.11, SaltsUtils 0.1.10,
SaltsCNet 0.1.20, CHttp 0.1.6; vcpkg-cache 0.1.9. The shared curl transport
verifies each archive's digest and size and the producer manifest. Native
toolchain selection is architecture-specific. No Mono or floating Salts packages.

macOS Intel has a real x64 matrix row, but the current release family lacks its
five matching SDKs and installed-x64-osx archive. The row fails explicitly at
dependency restoration until the upstream projects publish those assets and
the SDK lock is updated. No ARM64 SDK substitution, no continue-on-error.
Android device tests and the previously optional Metallic guest qualification
are separate work; this native CI does not claim their execution.

WASI02 fixture generation and Core 3.0/spec conformance remain separate
workflows. The source relocation changed the fixture input to
`wasm/tests/wasi02/fixtures/src`.
