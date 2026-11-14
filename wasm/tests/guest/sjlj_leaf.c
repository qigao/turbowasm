#include <setjmp.h>
#include <stdint.h>

/* Separate TU and a volatile indirect call keep the transfer from reducing to
 * local control flow in either the unoptimized or LTO fixture. */
__attribute__((noinline)) void sjlj_leaf(jmp_buf target, int value) {
    void (*volatile jump)(jmp_buf, int) = longjmp;
    jump(target, value);
}

__attribute__((noinline)) void sjlj_recursive(jmp_buf outer, unsigned depth) {
    jmp_buf inner;
    volatile unsigned char storage[1024];
    storage[0] = (unsigned char)depth;
    if (setjmp(inner) == 0) {
        if (depth) sjlj_recursive(outer, depth - 1);
        sjlj_leaf(outer, -9);
    }
    /* An inner handler must rethrow a jump addressed to the outer frame. */
    if (storage[0] == depth) __builtin_trap();
}

__attribute__((noinline)) void sjlj_stack(jmp_buf target, uintptr_t *first) {
    volatile unsigned char space[4096];
    space[0] = 42;
    uintptr_t address = (uintptr_t)&space[0];
    if (!*first) *first = address;
    if (*first != address || space[0] != 42) __builtin_trap();
    sjlj_leaf(target, 7);
}
