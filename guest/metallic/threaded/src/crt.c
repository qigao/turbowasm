#include "internal.h"
#include <stdint.h>

extern uintptr_t __metallic_brk;
extern unsigned char __heap_base;
void __wasm_call_ctors(void);

void __metallic_threads_start(void) {
    static atomic_uint initialized;
    unsigned expected = 0;
    /* Shared across siblings: reject duplicate or recursive CRT entry before
     * touching the allocator or root registry, including after a failed ctor. */
    if (!atomic_compare_exchange_strong(&initialized, &expected, 1)) __builtin_trap();
    __metallic_brk = (uintptr_t)&__heap_base;
    __metallic_threads_init();
    __wasm_call_ctors();
}
