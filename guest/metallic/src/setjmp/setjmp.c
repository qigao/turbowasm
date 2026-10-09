#include <setjmp.h>

/* LLVM replaces setjmp calls with these helpers and a continuation in the
 * caller. Defining setjmp itself would hide missing compiler lowering. */
void __wasm_setjmp(jmp_buf env, unsigned int label, void *invocation) {
    if (!env || !label || !invocation) __builtin_trap();
    env[0].__invocation = invocation;
    env[0].__label = label;
}

unsigned int __wasm_setjmp_test(jmp_buf env, void *invocation) {
    if (!env || !env[0].__label || !invocation) __builtin_trap();
    if (env[0].__invocation == invocation) return env[0].__label;
    return 0;
}
