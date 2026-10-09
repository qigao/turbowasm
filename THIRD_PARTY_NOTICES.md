# Third-party source notices

## Metallic

`guest/metallic/src` and `guest/metallic/include` originate from
[jdh8/metallic](https://github.com/jdh8/metallic), commit
`66ea0f480a16a9341be94ed4e66be28b3c3802d5` (2026-09-20).
The upstream README is retained for reference; this repository builds the
sources through `guest/CMakeLists.txt`, not the upstream Makefile.

Metallic is MIT licensed, copyright 2017–2019 Chen-Pang He. The complete license
is retained in [guest/metallic/LICENSE](guest/metallic/LICENSE) and installed
with the guest SDK. Embedded third-party notices remain in the source, including
dlmalloc's public-domain/CC0 notice in `src/stdlib/malloc.c`.

Local changes cover command startup/exit, checked heap growth, aligned allocation
admission, fopen allocation failure, temporary files, zero-length stdio, standard
stream lifetime, stdio error propagation, freopen admission, an ENOTSUP alias
and corrected Preview1 rights. The filesystem profile uses no-follow opens to match TurboWasm HostFS
capabilities. Changes are maintained directly in this local source tree.
Additional corrections cover unsigned assembly of 128-bit shift results and
restartable Unicode conversion state, null-input handling and invalid prefixes.
Stdio now supports explicit bounded buffering, stream-wide exit flushing,
read-ahead-aware positioning, checked byte counts and short-I/O recovery.
Additional guest extensions provide a separate Reactor CRT and LLVM-assisted
`setjmp`/`longjmp` using standard Wasm EH. The SJLJ helpers implement the ABI
documented by LLVM's `WebAssemblyLowerEmscriptenEHSjLj.cpp` (LLVM 21.1.1);
the architecture document records references and compiler requirements.

This is an optional wasm32, single-threaded guest library, not a native runtime
dependency. It does not provide a shell, threads or arbitrary
locales/rounding modes. Upstream documents ten failing soft-float/128-bit shift
tests; importing its sources does not establish complete C11 conformance.
