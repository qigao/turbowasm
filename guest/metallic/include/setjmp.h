#ifndef _SETJMP_H
#define _SETJMP_H

#include "bits/specifiers.h"

/* Private LLVM Wasm SJLJ state. All pointers refer to guest linear memory.
 * A target is valid only while its saving invocation remains active on the
 * same C thread. Recompile every consumer when changing this layout.
 */
typedef struct {
    void *__invocation;
    unsigned int __label;
    struct {
        void *__environment;
        int __value;
    } __payload;
} jmp_buf[1];

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((returns_twice)) int setjmp(jmp_buf env);
_NORETURN void longjmp(jmp_buf env, int val);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* setjmp.h */
