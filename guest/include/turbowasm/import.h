#ifndef TURBOWASM_GUEST_IMPORT_H
#define TURBOWASM_GUEST_IMPORT_H

/* Guest-only declarations for the Clang Wasm toolchain used by the SDK.
 * module/name are string literals, matched exactly by the host linker.
 * This declares a required capability; it neither loads nor authorizes it.
 * Use fixed-width scalar types, or explicitly specified guest offsets/lengths.
 * Example: TURBOWASM_IMPORT("app:clock/v1", "now_ns") int64_t now_ns(void);
 * C++ headers must also give their imported functions C linkage.
 */
#if !defined(__wasm__) || !defined(__clang__)
#error "turbowasm/import.h requires the Clang WebAssembly guest toolchain"
#endif

#define TURBOWASM_IMPORT(module, name) \
    __attribute__((import_module(module), import_name(name)))

#endif
