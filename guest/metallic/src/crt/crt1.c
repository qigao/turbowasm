#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>

#include "../wasi/wasi.h"

void __wasm_call_ctors(void);

static size_t strlen_(char s[static 1])
{
    for (size_t i = 0; ; ++i)
        if (!s[i])
            return i;
}

int __main_argc_argv(int, char**);

__attribute__((__weak__))
int main(void)
{
    size_t argc = 0;
    size_t buf_size = 0;
    if (__wasi_args_sizes_get(&argc, &buf_size) != 0)
        return __main_argc_argv(0, (char*[]){ NULL });

    /* Keep argv alive through exit handlers. Allocation is bounded by guest
     * memory, and failure terminates startup without touching the shadow stack. */
    if (argc > INT_MAX || argc > SIZE_MAX / sizeof(char*) - 1)
        return EXIT_FAILURE;
    char** argv = malloc((argc + 1) * sizeof(*argv));
    char* args = malloc(buf_size ? buf_size : 1);
    if (!argv || !args) {
        free(argv);
        free(args);
        return EXIT_FAILURE;
    }

    if (__wasi_args_get((uint8_t**)argv, (uint8_t*)args) != 0) {
        free(args);
        free(argv);
        return EXIT_FAILURE;
    }

    /* WASI populates argv[] with pointers into args[]; rebase them so they
     * point into the local args[] buffer rather than wherever WASI wrote. */
    if (argc > 0) {
        argv[0] = args;
        for (size_t i = 0; i + 1 < argc; ++i)
            argv[i + 1] = argv[i] + strlen_(argv[i]) + 1;
    }
    argv[argc] = NULL;

    return __main_argc_argv((int)argc, argv);
}

extern uintptr_t __metallic_brk;

/* First byte past everything the linker placed: wasm-ld links stack-first,
 * so linear memory is [shadow stack | data | __heap_base...].  Seeding the
 * break any lower lets the heap silently overwrite the stack and data. */
extern unsigned char __heap_base;

_Noreturn void _start(void)
{
#ifdef __METALLIC_THREADS__
    void __metallic_threads_start(void);
    __metallic_threads_start();
#else
    __metallic_brk = (uintptr_t)&__heap_base;
    __wasm_call_ctors();
#endif

    int rc = main();

    exit(rc);
}
