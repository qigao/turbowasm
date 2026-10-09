#include "reactor_library.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((import_module("test"), import_name("control"))) int control(int);
void _initialize(void);
static int constructors;
static int *state;
static char output_buffer[32];

__attribute__((constructor)) static void construct(void) {
    ++constructors;
    int mode = control(0);
    if (mode == 1) __builtin_trap();
    if (mode == 2) { volatile int loop = 1; while (loop) {} }
    if (mode == 3) _initialize();
    state = calloc(1, sizeof(*state));
    if (!state || setvbuf(stdout, output_buffer, _IOFBF, sizeof(output_buffer)))
        __builtin_trap();
}
int step(int amount) {
    if (!state || amount < 0 || amount > 100 || *state > 1000000) return -1;
    *state += reactor_library_step(amount);
    return *state;
}
int initialized(void) { return constructors; }
int ask_host(void) { return control(1); }
int allocation_cycle(void) {
    for (int i = 0; i < 64; ++i) {
        unsigned char *p = malloc(32768);
        if (!p) return -1;
        memset(p, i, 32768);
        if (p[0] != i || p[32767] != i) { free(p); return -2; }
        free(p);
    }
    return 0;
}
int finish(void) {
    free(state);
    state = NULL;
    if (fwrite("closed", 1, 6, stdout) != 6) return -1;
    return fflush(NULL) ? -2 : 0;
}
void fail(void) { __builtin_trap(); }
void terminate(void) { _Exit(37); }
void forever(void) { volatile int loop = 1; while (loop) {} }
