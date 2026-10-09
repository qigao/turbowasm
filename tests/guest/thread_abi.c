/* Internal compiler/runtime qualification for #426, not an installed threads
 * library. Fixed storage isolates startup/TLS from the pending libc lock audit. */
#include <errno.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <uchar.h>
#include <wchar.h>

__attribute__((import_module("wasi"), import_name("thread-spawn")))
extern int spawn_thread(void *argument);
__attribute__((import_module("test"), import_name("hold")))
extern void hold(int phase);
extern void __wasm_init_tls(void *);
extern void __wasm_call_ctors(void);
extern unsigned fixture_tls_size(void), fixture_tls_align(void);
extern void sjlj_leaf(jmp_buf, int);

enum { CHILDREN = 2, STACK_BYTES = 32768, TLS_BYTES = 4096, TLS_ALIGNMENT = 64 };
typedef struct {
    void *stack_top;
    void *tls;
    _Atomic int done;
    int result;
    uintptr_t stack_address;
    uintptr_t tls_address;
    uintptr_t errno_address;
    int nested_result;
} child_record;
_Static_assert(offsetof(child_record, done) == 8, "assembly terminal offset");
_Static_assert(offsetof(child_record, tls) == 4, "assembly TLS offset");
static child_record children[CHILDREN];
static _Alignas(16) unsigned char stacks[CHILDREN][STACK_BYTES];
static _Alignas(TLS_ALIGNMENT) unsigned char tls[CHILDREN + 1][TLS_BYTES];
static _Thread_local int tls_value = 13;
static _Thread_local jmp_buf jump_target;
static volatile int data_sentinel = 37;
static volatile int zero_sentinel;
static volatile unsigned constructors;
static int root_initialized;

__attribute__((constructor)) static void record_constructor(void) { ++constructors; }

int setup(void) {
    if (root_initialized || fixture_tls_size() > TLS_BYTES ||
        fixture_tls_align() > TLS_ALIGNMENT) return -1;
    __wasm_init_tls(tls[CHILDREN]);
    if (tls_value != 13 || errno != 0 || data_sentinel != 37 || zero_sentinel) return -2;
    __wasm_call_ctors();
    root_initialized = 1;
    tls_value = 17; errno = 91;
    data_sentinel = 99; zero_sentinel = 72;
    char16_t surrogate;
    if (mbrtowc(NULL, "\xf0", 1, NULL) != (size_t)-2 ||
        mbrtoc32(NULL, "\xe2", 1, NULL) != (size_t)-2 ||
        mbrtoc16(&surrogate, "\xf0\x9f\x98\x81", 4, NULL) != 4 ||
        surrogate != 0xd83d || c16rtomb(NULL, 0, NULL) != 1) return -4;
    char encoded[4];
    if (c16rtomb(encoded, 0xd801, NULL) != 0) return -5;
    return constructors == 1 ? 0 : -3;
}

int spawn(int index) {
    if (index < 0 || index >= CHILDREN) return spawn_thread(0);
    child_record *record = &children[index];
    record->stack_top = stacks[index] + STACK_BYTES;
    record->tls = tls[index];
    record->result = -1;
    atomic_store(&record->done, 0);
    return spawn_thread(record);
}

void fixture_child(int tid, child_record *record) {
    volatile int local = tid;
    int valid = tls_value == 13 && errno == 0 && data_sentinel == 99 &&
        zero_sentinel == 72 && constructors == 1;
    record->stack_address = (uintptr_t)&local;
    record->tls_address = (uintptr_t)&jump_target;
    record->errno_address = (uintptr_t)__errno_location();
    tls_value = tid; errno = tid + 100;
    int index = (int)(record - children);
    const char *prefix = index ? "\xc2" : "\xe2";
    const char *tail = index ? "\xa2" : "\x82\xac";
    size_t tail_size = index ? 1 : 2;
    uint32_t expected = index ? 0xa2 : 0x20ac;
    char16_t surrogate;
    char encoded[4];
    valid = valid && mbrtowc(NULL, prefix, 1, NULL) == (size_t)-2 &&
        mbrtoc32(NULL, prefix, 1, NULL) == (size_t)-2 &&
        mbrtoc16(&surrogate, index ? "\xf0\x9f\x98\x82" : "\xf0\x9f\x98\x83", 4, NULL) == 4 &&
        c16rtomb(encoded, (char16_t)(0xd802 + index), NULL) == 0;
    hold(0);
    /* Both children remain admitted until the second barrier is released. */
    record->nested_result = spawn_thread(0);
    hold(1);
    wchar_t wide;
    char32_t scalar;
    valid = valid && mbrtowc(&wide, tail, tail_size, NULL) == tail_size &&
        (uint32_t)wide == expected && mbrtoc32(&scalar, tail, tail_size, NULL) == tail_size &&
        scalar == expected && mbrtoc16(&surrogate, "", 1, NULL) == (size_t)-3 &&
        surrogate == (index ? 0xde02 : 0xde03) &&
        c16rtomb(encoded, 0xdc01, NULL) == 4;
    /* Decode with explicit state to verify each thread kept its own high
     * surrogate, independently of the decoder's implicit state. */
    mbstate_t explicit_state = {0};
    valid = valid && mbrtoc32(&scalar, encoded, 4, &explicit_state) == 4 &&
        scalar == 0x10000u + ((0x2u + (unsigned)index) << 10) + 1;
    switch (setjmp(jump_target)) {
    case 0: sjlj_leaf(jump_target, tid); __builtin_trap();
    default:
        record->result = valid && tls_value == tid && errno == tid + 100 && local == tid;
    }
    /* The assembly caller publishes done after this C frame has returned. */
}

int root_intact(void) {
    return tls_value == 17 && errno == 91 && data_sentinel == 99 &&
        zero_sentinel == 72 && constructors == 1;
}

int root_conversions(void) {
    wchar_t wide;
    char32_t scalar;
    char16_t surrogate;
    char encoded[4];
    mbstate_t explicit_state = {0};
    return mbrtowc(&wide, "\x9f\x98\x80", 3, NULL) == 3 && wide == 0x1f600 &&
        mbrtoc32(&scalar, "\x82\xac", 2, NULL) == 2 && scalar == 0x20ac &&
        mbrtoc16(&surrogate, "", 1, NULL) == (size_t)-3 && surrogate == 0xde01 &&
        c16rtomb(encoded, 0xdc01, NULL) == 4 &&
        mbrtoc32(&scalar, encoded, 4, &explicit_state) == 4 && scalar == 0x10401;
}

int results(void) {
    if (!root_intact()) return -1;
    for (int i = 0; i < CHILDREN; ++i) {
        const child_record *r = &children[i];
        if (!atomic_load(&r->done) || r->result != 1 || r->nested_result != -2) return -2;
        if (r->stack_address < (uintptr_t)stacks[i] ||
            r->stack_address >= (uintptr_t)(stacks[i] + STACK_BYTES)) return -3;
        if (r->tls_address < (uintptr_t)tls[i] ||
            r->tls_address >= (uintptr_t)(tls[i] + TLS_BYTES)) return -4;
        if (r->errno_address < (uintptr_t)tls[i] ||
            r->errno_address >= (uintptr_t)(tls[i] + TLS_BYTES)) return -5;
    }
    return 0;
}
