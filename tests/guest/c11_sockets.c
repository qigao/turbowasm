#include "../../guest/metallic/src/wasi/wasi.h"
#include <threads.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "internal.h"

#define REQUIRE(x) do { if (!(x)) return __LINE__; } while (0)
#define SOCKET_IMPORT(name) __attribute__((import_module("wasi_snapshot_preview1"), import_name(#name)))
SOCKET_IMPORT(sock_recv) extern uint32_t socket_recv(uint32_t, const __wasi_iovec_t *, size_t, uint32_t, size_t *, uint16_t *);
SOCKET_IMPORT(sock_send) extern uint32_t socket_send(uint32_t, const __wasi_ciovec_t *, size_t, uint32_t, size_t *);
SOCKET_IMPORT(poll_oneoff) extern uint32_t socket_poll(const void *, void *, size_t, size_t *);
__attribute__((import_module("test"), import_name("recv_pending"))) extern int recv_pending(void);
extern uintptr_t __metallic_brk;
extern unsigned char __heap_base;
extern void __wasm_call_ctors(void);
static atomic_uint started;

int initialize(void) {
    __metallic_brk = (uintptr_t)&__heap_base;
    __metallic_threads_init(); __wasm_call_ctors(); return 0;
}
static void store32(uint8_t *p, uint32_t value) { memcpy(p, &value, sizeof(value)); }
static int receive(uint32_t fd, const char *expected) {
    _Alignas(8) uint8_t subscription[48] = {0}, event[32] = {0};
    store32(subscription, 42); subscription[8] = 1; store32(subscription + 16, fd);
    size_t count = 0;
    REQUIRE(socket_poll(subscription, event, 1, &count) == 0 && count == 1);
    REQUIRE(event[0] == 42 && event[8] == 0 && event[9] == 0 && event[10] == 1);
    char bytes[4]; __wasi_iovec_t vector = {bytes, sizeof(bytes)}; uint16_t flags = 0;
    REQUIRE(socket_recv(fd, &vector, 1, 2, &count, &flags) == 0 && count == 4 && flags == 0);
    REQUIRE(memcmp(bytes, expected, 4) == 0); return 0;
}
static int send(uint32_t fd, const char *bytes) {
    __wasi_ciovec_t vector = {bytes, 4}; size_t count = 0;
    REQUIRE(socket_send(fd, &vector, 1, 0, &count) == 0 && count == 4); return 0;
}
static int receiver(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 16; ++i) {
        int error = receive(4, "ping"); if (error) return error;
        error = send(4, "pong"); if (error) return error;
    }
    return 0;
}
static int sender(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 16; ++i) {
        int error = send(3, "ping"); if (error) return error;
        error = receive(3, "pong"); if (error) return error;
    }
    return 0;
}
int sockets(void) {
    thrd_t a, b; int first, second;
    REQUIRE(thrd_create(&a, receiver, NULL) == thrd_success);
    REQUIRE(thrd_create(&b, sender, NULL) == thrd_success);
    REQUIRE(thrd_join(a, &first) == thrd_success);
    REQUIRE(thrd_join(b, &second) == thrd_success);
    REQUIRE(__metallic_threads_drain(NULL) == thrd_success);
    return first ? first : second;
}
static int blocked_receiver(void *unused) {
    (void)unused;
    atomic_store(&started, 1);
    __builtin_wasm_memory_atomic_notify((int *)&started, UINT32_MAX);
    char bytes[4]; __wasi_iovec_t vector = {bytes, sizeof(bytes)};
    size_t count; uint16_t flags;
    return (int)socket_recv(4, &vector, 1, 0, &count, &flags);
}
static int exiting(void *unused) {
    (void)unused;
    while (!atomic_load(&started)) __builtin_wasm_memory_atomic_wait32((int *)&started, 0, -1);
    while (!recv_pending()) thrd_yield();
    __wasi_proc_exit(23);
}
int exit_waiters(void) {
    thrd_t a, b; int result;
    REQUIRE(thrd_create(&a, blocked_receiver, NULL) == thrd_success);
    REQUIRE(thrd_create(&b, exiting, NULL) == thrd_success);
    (void)thrd_join(a, &result); (void)thrd_join(b, &result);
    return __LINE__; /* The root's group policy interrupts join, never success. */
}
