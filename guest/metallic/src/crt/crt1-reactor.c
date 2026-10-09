#include <stdint.h>

extern uintptr_t __metallic_brk;
extern unsigned char __heap_base;
void __wasm_call_ctors(void);

/* A Reactor owns one heap and constructor lifetime across all exported calls.
 * Mark initialization before constructors so recursive initialization cannot
 * reset live allocations. The host serializes calls to this single-thread CRT. */
__attribute__((export_name("_initialize")))
void _initialize(void)
{
#ifdef __METALLIC_THREADS__
    void __metallic_threads_start(void);
    __metallic_threads_start();
#else
    static volatile int initialized;
    if (initialized)
        __builtin_trap();
    initialized = 1;
    __metallic_brk = (uintptr_t)&__heap_base;
    __wasm_call_ctors();
#endif
}
