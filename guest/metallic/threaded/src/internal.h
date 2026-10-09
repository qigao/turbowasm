#ifndef METALLIC_THREAD_INTERNAL_H
#define METALLIC_THREAD_INTERNAL_H
#include <threads.h>
#include "../../src/internal/thread_lock.h"

enum { METALLIC_THREAD_CAPACITY = 32, METALLIC_TSS_CAPACITY = 128,
       METALLIC_THREAD_STACK_BYTES = 128 * 1024, METALLIC_THREAD_TLS_LIMIT = 64 * 1024 };
/* Called once by root CRT before constructors, with linker TLS already active. */
void __metallic_threads_init(void);
/* Close admission, wait for actual guest terminals and reclaim all child
 * storage. Root only, and application threads must cooperate. */
int __metallic_threads_drain(const struct timespec *deadline);
unsigned __metallic_thread_token(void);
/* Relative nanoseconds until an absolute TIME_UTC deadline: -1 invalid/clock
 * failure, zero expired. Large valid deadlines are checked in bounded slices. */
int64_t __metallic_deadline_ns(const struct timespec *deadline);
#endif
