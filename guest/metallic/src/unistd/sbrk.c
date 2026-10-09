#include <stdint.h>
#include <stddef.h>
#include <errno.h>

uintptr_t __metallic_brk;

void* __sbrk(intptr_t increment)
{
    const uint64_t pagesize = 64 * 1024;
    uintptr_t previous = __metallic_brk;
    uint64_t capacity = pagesize * __builtin_wasm_memory_size(0);
    int64_t requested = (int64_t)previous + increment;
    extern unsigned char __heap_base;
    if (requested < (int64_t)(uintptr_t)&__heap_base || requested > UINTPTR_MAX) {
        errno = ENOMEM;
        return (void*)-1;
    }

    if ((uint64_t)requested > capacity) {
        uint64_t excess = (uint64_t)requested - capacity;
        size_t pages = (size_t)((pagesize - 1 + excess) / pagesize);

        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) {
            errno = ENOMEM;
            return (void*)-1;
        }
    }
    __metallic_brk = (uintptr_t)requested;
    return (void*)previous;
}
