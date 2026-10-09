#include <setjmp.h>

/* The LLVM C_LONGJMP tag carries a pointer to { environment, return value }.
 * Store that payload in the target buffer, not the stack being unwound. */
_NORETURN void __wasm_longjmp(jmp_buf env, int val) {
    if (!env || !env[0].__invocation || !env[0].__label) __builtin_trap();
    env[0].__payload.__environment = env;
    env[0].__payload.__value = val ? val : 1;
    __builtin_wasm_throw(1, &env[0].__payload);
    __builtin_unreachable();
}

_NORETURN void longjmp(jmp_buf env, int val) {
    __wasm_longjmp(env, val);
}

/* LLVM 22 stopped synthesizing this tag definition in the backend. */
#if __clang_major__ >= 22
__asm__(".globl __c_longjmp\n"
        ".tagtype __c_longjmp i32\n"
        "__c_longjmp:\n");
#endif
