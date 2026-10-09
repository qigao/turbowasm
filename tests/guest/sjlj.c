#include <setjmp.h>
#include <stdint.h>
#include <limits.h>

void sjlj_leaf(jmp_buf target, int value);
void sjlj_recursive(jmp_buf target, unsigned depth);
void sjlj_stack(jmp_buf target, uintptr_t *first);
__attribute__((import_module("test"), import_name("control"))) int control(int);

int sjlj_values(int value) {
    jmp_buf target;
    switch (setjmp(target)) {
    case 0: sjlj_leaf(target, value); return -1;
    case 1: return value == 0 || value == 1 ? 0 : -2;
    case 7: return value == 7 ? 0 : -3;
    case -9: return value == -9 ? 0 : -4;
    case INT_MIN: return value == INT_MIN ? 0 : -5;
    case INT_MAX: return value == INT_MAX ? 0 : -6;
    default: return -7;
    }
}

int sjlj_nested(void) {
    jmp_buf outer, inner;
    volatile int state = 0;
    switch (setjmp(outer)) {
    case 0:
        if (setjmp(inner) == 0) { state = 10; sjlj_leaf(inner, 7); }
        if (state != 10) return -1;
        state = 20;
        sjlj_recursive(outer, 8);
        return -2;
    case -9: return state == 20 ? 0 : -3;
    default: return -4;
    }
}

int sjlj_repeated(void) {
    jmp_buf target;
    volatile int count = 0;
    if (setjmp(target) != 0) ++count;
    if (count < 128) sjlj_leaf(target, 7);
    return count == 128 ? 0 : -1;
}

static uintptr_t stack_address;
int sjlj_restore_stack(void) {
    jmp_buf target;
    volatile int count = 0;
    stack_address = 0;
    if (setjmp(target) != 0) ++count;
    if (count < 128) sjlj_stack(target, &stack_address);
    return count == 128 ? 0 : -1;
}

int sjlj_callback(void) {
    jmp_buf target;
    volatile int answer = 0;
    if (setjmp(target) == 0) {
        answer = control(1);
        sjlj_leaf(target, 7);
    }
    return answer == 42 ? 0 : -1;
}

int sjlj_trap(void) {
    jmp_buf target;
    if (setjmp(target) == 0) __builtin_trap();
    return -1;
}

int sjlj_unrelated_exception(void) {
    jmp_buf target;
    static int payload;
    if (setjmp(target) == 0) __builtin_wasm_throw(0, &payload);
    return -1;
}

int sjlj_fuel(void) {
    jmp_buf target;
    if (setjmp(target) == 0) sjlj_leaf(target, 7);
    sjlj_leaf(target, 7);
    return -1;
}
