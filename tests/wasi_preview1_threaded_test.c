#include "wasi_preview1_socket_fixture.h"
#include <salts/thread.h>
#include <stdatomic.h>
#include <string.h>

enum { WORKERS = 6 };
typedef struct threaded_probe {
    turbowasm_wasi_preview1 wasi;
    turbowasm_wasi_fs fs;
    turbowasm_wasi_descriptor_ops ops;
    p1_guest guests[WORKERS];
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    const void *owner;
    size_t wakes, done;
    unsigned wrong_owner, reads, retains, releases, finishes;
    bool ready, block_scan, scan_entered, release_scan, interruption_seen, premature_return;
    bool open[WORKERS];
    atomic_bool interrupt;
    turbowasm_status reentrant_advance, reentrant_destroy;
} threaded_probe;
typedef struct threaded_work {
    threaded_probe *p;
    unsigned index, operation;
    bool interruptible;
    turbowasm_status status, wrong_advance, wrong_timeout, wrong_shutdown, wrong_destroy;
    turbowasm_trap trap;
    uint32_t error;
} threaded_work;

static void wake_owner(void *context) {
    threaded_probe *p = context;
    cmeta_mutex_lock(&p->mutex); ++p->wakes;
    cmeta_cond_broadcast(&p->changed); cmeta_mutex_unlock(&p->mutex);
}
static void owner_callback(threaded_probe *p) {
    if (p->owner != cmeta_thread_current_token()) ++p->wrong_owner;
}
static uint32_t close_socket(void *c, turbowasm_wasi_fs_file f) {
    threaded_probe *p = c; owner_callback(p);
    if (f.object < WORKERS) p->open[f.object] = false;
    return 0;
}
static uint32_t retain_socket(void *c, turbowasm_wasi_fs_file f) {
    (void)f; threaded_probe *p = c; owner_callback(p); ++p->retains; return 0;
}
static void release_socket(void *c, turbowasm_wasi_fs_file f) {
    (void)f; threaded_probe *p = c; owner_callback(p); ++p->releases;
}
static void finish_socket(void *c, turbowasm_wasi_fs_file f, uint8_t direction) {
    (void)f; (void)direction; threaded_probe *p = c; owner_callback(p); ++p->finishes;
}
static uint32_t ready_socket(void *c, turbowasm_wasi_fs_file f, uint8_t direction,
    turbowasm_wasi_readiness *out) {
    (void)f; (void)direction; threaded_probe *p = c; owner_callback(p);
    p->reentrant_advance = turbowasm_wasi_preview1_advance(&p->wasi);
    p->reentrant_destroy = turbowasm_wasi_preview1_destroy_checked(&p->wasi);
    cmeta_mutex_lock(&p->mutex);
    if (p->block_scan) {
        p->scan_entered = true; cmeta_cond_broadcast(&p->changed);
        while (!p->release_scan) cmeta_cond_wait(&p->changed, &p->mutex);
    }
    cmeta_mutex_unlock(&p->mutex);
    *out = (turbowasm_wasi_readiness){.ready = p->ready, .bytes = p->ready ? 4 : 0};
    return 0;
}
static uint32_t recv_socket(void *c, turbowasm_wasi_fs_file f,
    const turbowasm_wasi_buffer *b, size_t n, uint16_t flags, bool nonblock,
    uint32_t *out, uint16_t *out_flags) {
    (void)f; (void)flags; (void)nonblock;
    threaded_probe *p = c; owner_callback(p); ++p->reads; *out = 0; *out_flags = 0;
    if (!p->ready) return TURBOWASM_WASI_ERRNO_AGAIN;
    if (n && b[0].size >= 4) { memcpy(b[0].data, "pong", 4); *out = 4; }
    return 0;
}
static uint32_t read_socket(void *c, turbowasm_wasi_fs_file f,
    const turbowasm_wasi_buffer *b, size_t n, uint32_t *out) {
    if (f.object == WORKERS) {
        threaded_probe *p = c;
        cmeta_mutex_lock(&p->mutex);
        p->scan_entered = true; cmeta_cond_broadcast(&p->changed);
        while (!p->release_scan) cmeta_cond_wait(&p->changed, &p->mutex);
        cmeta_mutex_unlock(&p->mutex); *out = 0; return 0;
    }
    uint16_t flags; return recv_socket(c, f, b, n, 0, false, out, &flags);
}
static uint32_t write_socket(void *c, turbowasm_wasi_fs_file f,
    const turbowasm_wasi_const_buffer *b, size_t n, uint32_t *out) {
    (void)f; owner_callback(c); *out = 0;
    for (size_t i = 0; i < n; ++i) *out += (uint32_t)b[i].size;
    return 0;
}
static uint32_t shutdown_socket(void *c, turbowasm_wasi_fs_file f, uint8_t how) {
    (void)f; (void)how; owner_callback(c); return 0;
}
static uint32_t clock_time(void *c, uint32_t id, uint64_t precision, uint64_t *out) {
    (void)c; (void)id; (void)precision; *out = 100; return 0;
}
static turbowasm_wasi_preview1_config_v2 config(threaded_probe *p, size_t capacity) {
    turbowasm_wasi_preview1_config_v2 v; turbowasm_wasi_preview1_config_v2_init(&v);
    v.base.filesystem = &p->fs; v.allow_sockets = v.allow_poll = true;
    v.base.allow_fd_read = v.base.allow_fd_write = true;
    v.base.allow_clock = true; v.base.clock_time = clock_time;
    v.wait_capacity = capacity; v.io_bytes = 32; v.pending_bytes = 4096;
    return v;
}
static void setup_with_budget(threaded_probe *p, size_t count, size_t capacity, size_t budget) {
    *p = (threaded_probe){.owner = cmeta_thread_current_token()}; atomic_init(&p->interrupt, false);
    cmeta_mutex_init(&p->mutex); cmeta_cond_init(&p->changed);
    p->ops = (turbowasm_wasi_descriptor_ops){.size = sizeof(p->ops), .api_version = 1,
        .file = {.context = p, .close = close_socket, .read = read_socket, .write = write_socket},
        .retain = retain_socket, .release = release_socket, .finish = finish_socket,
        .ready = ready_socket, .recv = recv_socket, .send = write_socket, .shutdown = shutdown_socket};
    turbowasm_wasi_fs_config f = {WORKERS + 1, p->ops.file};
    check_equal(turbowasm_wasi_fs_init(&p->fs, &f), TURBOWASM_OK);
    turbowasm_wasi_preview1_config_v2 v = config(p, capacity);
    v.pending_bytes = budget;
    turbowasm_wasi_preview1_threaded_config t = {32, 1000000, wake_owner, p};
    check_equal(turbowasm_wasi_preview1_init_threaded(&p->wasi, &v, &t), TURBOWASM_OK);
    for (size_t i = 0; i < count; ++i) {
        turbowasm_wasi_fs_file file = {i, 1}; turbowasm_wasi_fs_descriptor descriptor;
        check_equal(turbowasm_wasi_fs_bind_socket_move(&p->fs, (uint32_t)i + 4, &p->ops, &file,
            TURBOWASM_WASI_FILETYPE_SOCKET_STREAM, 0, UINT64_MAX, 0, &descriptor), TURBOWASM_OK);
        p->open[i] = true; p1_guest_init(&p->guests[i], &p->wasi); p1_iovec(&p->guests[i], 4);
        p1_store(&p->guests[i], 128, 0x12345678);
    }
}
static void setup(threaded_probe *p, size_t count, size_t capacity) { setup_with_budget(p, count, capacity, 4096); }
static void teardown(threaded_probe *p, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        p1_guest_destroy(&p->guests[i]);
        if (p->open[i]) check_equal(turbowasm_wasi_fs_close_fd(&p->fs, (uint32_t)i + 4), 0u);
    }
    check_equal(p->retains, p->releases); check_equal(p->wrong_owner, 0u);
    check_equal(turbowasm_wasi_preview1_destroy_checked(&p->wasi), TURBOWASM_OK);
    check_equal(turbowasm_wasi_fs_destroy(&p->fs), TURBOWASM_OK);
    cmeta_cond_destroy(&p->changed); cmeta_mutex_destroy(&p->mutex);
}
static bool should_interrupt(void *context) {
    threaded_probe *p = context; bool value = atomic_load(&p->interrupt);
    if (value) {
        cmeta_mutex_lock(&p->mutex); p->interruption_seen = true;
        cmeta_cond_broadcast(&p->changed); cmeta_mutex_unlock(&p->mutex);
    }
    return value;
}
static void worker(void *context) {
    threaded_work *w = context; threaded_probe *p = w->p;
    uint64_t delay;
    w->wrong_advance = turbowasm_wasi_preview1_advance(&p->wasi);
    w->wrong_timeout = turbowasm_wasi_preview1_next_timeout(&p->wasi, &delay);
    w->wrong_shutdown = turbowasm_wasi_preview1_shutdown_request(&p->wasi);
    w->wrong_destroy = turbowasm_wasi_preview1_destroy_checked(&p->wasi);
    uint32_t values[] = {w->index + 4, 0, 1, 0, 128, 132}; size_t n = 6;
    if (w->operation == P1_POLL) { values[0] = 1024; values[1] = 2048; values[2] = 1; values[3] = 128; n = 4; }
    if (w->operation == P1_CLOSE) n = 1;
    if (w->operation == P1_READ) { values[3] = 128; n = 4; }
    turbowasm_value args[6], result = {0}; size_t results = 0;
    for (size_t i = 0; i < n; ++i) args[i] = p1_i32(values[i]);
    turbowasm_execution_options options = {0};
    options.should_interrupt = w->interruptible ? should_interrupt : NULL; options.interrupt_context = p;
    w->status = turbowasm_instance_invoke_with_options(&p->guests[w->index].instance,
        P1_WRAPPER_BASE + w->operation, args, n, &result, 1, &results, &w->trap, &options);
    w->error = (uint32_t)result.as.i32;
    cmeta_mutex_lock(&p->mutex); ++p->done; ++p->wakes;
    cmeta_cond_broadcast(&p->changed); cmeta_mutex_unlock(&p->mutex);
}
static void start(threaded_work *w, cmeta_thread_t *t) { check_equal(cmeta_thread_create(t, worker, w), SALTS_OK); }
static void join(cmeta_thread_t *t) { check_equal(cmeta_thread_join(t), SALTS_OK); cmeta_thread_destroy(t); }
static void drive(threaded_probe *p, size_t done, size_t wakes) {
    size_t seen = 0;
    cmeta_mutex_lock(&p->mutex);
    while ((!done || p->done < done) && (!wakes || p->wakes < wakes)) {
        while (p->wakes == seen) cmeta_cond_wait(&p->changed, &p->mutex);
        seen = p->wakes;
        cmeta_mutex_unlock(&p->mutex);
        check_equal(turbowasm_wasi_preview1_advance(&p->wasi), TURBOWASM_OK);
        cmeta_mutex_lock(&p->mutex);
    }
    cmeta_mutex_unlock(&p->mutex);
}
static void check_worker(threaded_work *w, turbowasm_status status, uint32_t error) {
    check_equal(w->status, status); check_equal(w->trap, TURBOWASM_TRAP_NONE);
    if (status == TURBOWASM_OK) check_equal(w->error, error);
    check_equal(w->wrong_advance, TURBOWASM_INVALID_ARGUMENT);
    check_equal(w->wrong_timeout, TURBOWASM_INVALID_ARGUMENT);
    check_equal(w->wrong_shutdown, TURBOWASM_INVALID_ARGUMENT);
    check_equal(w->wrong_destroy, TURBOWASM_INVALID_ARGUMENT);
}
static void cancel_during_scan(void *context) {
    threaded_probe *p = context;
    cmeta_mutex_lock(&p->mutex);
    while (!p->scan_entered) cmeta_cond_wait(&p->changed, &p->mutex);
    atomic_store(&p->interrupt, true);
    while (!p->interruption_seen) cmeta_cond_wait(&p->changed, &p->mutex);
    p->premature_return = p->done != 0;
    p->release_scan = true; cmeta_cond_broadcast(&p->changed); cmeta_mutex_unlock(&p->mutex);
}

spec("Threaded Preview1 socket waits") {
    it("parks synchronous workers and delivers readiness with owner-only callbacks") {
        threaded_probe p; setup(&p, 1, 1);
        threaded_work w = {.p = &p, .operation = P1_RECV}; cmeta_thread_t thread; start(&w, &thread);
        drive(&p, 0, 3); /* retain, recv and wait registration each wake the owner */
        check_equal(turbowasm_wasi_preview1_destroy_checked(&p.wasi), TURBOWASM_INVALID_ARGUMENT);
        p.ready = true; check_equal(turbowasm_wasi_preview1_advance(&p.wasi), TURBOWASM_OK);
        drive(&p, 1, 0); join(&thread); check_worker(&w, TURBOWASM_OK, 0);
        check_equal(p1_load(&p.guests[0], 128), 4u);
        check_equal(p1_load(&p.guests[0], 512), UINT32_C(0x676e6f70));
        check_equal(p.reentrant_advance, TURBOWASM_INVALID_ARGUMENT);
        check_equal(p.reentrant_destroy, TURBOWASM_INVALID_ARGUMENT);
        teardown(&p, 1);
    }
    it("acknowledges cancellation after a pinned owner scan before releasing guest state") {
        threaded_probe p; setup(&p, 1, 1);
        threaded_work w = {.p = &p, .operation = P1_RECV, .interruptible = true};
        cmeta_thread_t thread, controller; start(&w, &thread); drive(&p, 0, 3);
        p.block_scan = true; p.ready = true;
        check_equal(cmeta_thread_create(&controller, cancel_during_scan, &p), SALTS_OK);
        check_equal(turbowasm_wasi_preview1_advance(&p.wasi), TURBOWASM_OK);
        join(&controller); drive(&p, 1, 0); join(&thread);
        check_worker(&w, TURBOWASM_INTERRUPTED, 0); check_false(p.premature_return);
        check_equal(p.reads, 1u); /* Readiness never admits a second receive after cancellation. */
        check_equal(p1_load(&p.guests[0], 128), UINT32_C(0x12345678));
        check_equal(p.finishes, 1u); teardown(&p, 1);
    }
    it("shutdown wakes all parked workers and preserves cleanup and close admission") {
        threaded_probe p; setup(&p, WORKERS, WORKERS);
        threaded_work work[WORKERS]; cmeta_thread_t threads[WORKERS];
        for (unsigned i = 0; i < WORKERS; ++i) {
            work[i] = (threaded_work){.p = &p, .index = i, .operation = P1_RECV}; start(&work[i], &threads[i]);
        }
        drive(&p, 0, 3 * WORKERS);
        check_equal(turbowasm_wasi_preview1_shutdown_request(&p.wasi), TURBOWASM_OK);
        drive(&p, WORKERS, 0);
        for (unsigned i = 0; i < WORKERS; ++i) { join(&threads[i]); check_worker(&work[i], TURBOWASM_OK, TURBOWASM_WASI_ERRNO_INTR); }
        bool done = false; check_equal(turbowasm_wasi_preview1_shutdown_poll(&p.wasi, &done), TURBOWASM_OK); check_true(done);
        check_equal(turbowasm_wasi_preview1_destroy_checked(&p.wasi), TURBOWASM_INVALID_ARGUMENT);
        work[0].operation = P1_CLOSE; start(&work[0], &threads[0]); drive(&p, WORKERS + 1, 0); join(&threads[0]);
        check_worker(&work[0], TURBOWASM_OK, 0); check_false(p.open[0]); teardown(&p, WORKERS);
    }
    it("preserves synchronous owner AGAIN and resumable owner waits") {
        threaded_probe p; setup(&p, 1, 1);
        uint32_t args[] = {4, 0, 1, 0, 128, 132};
        check_equal(p1_call(&p.guests[0], P1_RECV, args, 6), TURBOWASM_WASI_ERRNO_AGAIN);
        turbowasm_execution execution = {0}; p1_start(&p.guests[0], &execution, P1_RECV, args, 6);
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
        p.ready = true; check_equal(turbowasm_wasi_preview1_advance(&p.wasi), TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK);
        check_equal(p1_execution_result(&execution), 0u); turbowasm_execution_destroy(&execution); teardown(&p, 1);
    }
    it("keeps a parked lease distinct from a closed and rebound descriptor") {
        threaded_probe p; setup(&p, 1, 1);
        threaded_work w = {.p = &p, .operation = P1_RECV}; cmeta_thread_t thread;
        start(&w, &thread); drive(&p, 0, 3);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 4), 0u);
        turbowasm_wasi_fs_file file = {1, 1}; turbowasm_wasi_fs_descriptor descriptor;
        check_equal(turbowasm_wasi_fs_bind_socket_move(&p.fs, 4, &p.ops, &file,
            TURBOWASM_WASI_FILETYPE_SOCKET_STREAM, 0, UINT64_MAX, 0, &descriptor), TURBOWASM_OK);
        p.open[1] = true; p.ready = true;
        check_equal(turbowasm_wasi_preview1_advance(&p.wasi), TURBOWASM_OK);
        drive(&p, 1, 0); join(&thread); check_worker(&w, TURBOWASM_OK, TURBOWASM_WASI_ERRNO_BADF);
        check_true(p.open[1]); check_equal(p.reads, 1u);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 4), 0u); teardown(&p, 1);
    }
    it("rechecks contracted rights after worker readiness without consuming input") {
        threaded_probe p; setup(&p, 2, 1);
        threaded_work w = {.p = &p, .operation = P1_RECV}; cmeta_thread_t thread;
        start(&w, &thread); drive(&p, 0, 3);
        turbowasm_value args[] = {p1_i32(4), p1_i64(0), p1_i64(0)};
        check_equal(p1_invoke(&p.guests[1], P1_WRAPPER_BASE + P1_RIGHTS, args, 3).as.i32, 0);
        p.ready = true; check_equal(turbowasm_wasi_preview1_advance(&p.wasi), TURBOWASM_OK);
        drive(&p, 1, 0); join(&thread); check_worker(&w, TURBOWASM_OK, TURBOWASM_WASI_ERRNO_NOTCAPABLE);
        check_equal(p.reads, 1u); teardown(&p, 2);
    }
    it("bounds worker wait admission and returns AGAIN without losing the first waiter") {
        threaded_probe p; setup(&p, 2, 1);
        threaded_work a = {.p = &p, .operation = P1_RECV}, b = {.p = &p, .index = 1, .operation = P1_RECV};
        cmeta_thread_t ta, tb; start(&a, &ta); drive(&p, 0, 3);
        start(&b, &tb); drive(&p, 1, 0); join(&tb); check_worker(&b, TURBOWASM_OK, TURBOWASM_WASI_ERRNO_AGAIN);
        p.ready = true; check_equal(turbowasm_wasi_preview1_advance(&p.wasi), TURBOWASM_OK);
        drive(&p, 2, 0); join(&ta); check_worker(&a, TURBOWASM_OK, 0); teardown(&p, 2);
    }
    it("reserves payload budget across workers and releases it after terminal cleanup") {
        threaded_probe p; setup_with_budget(&p, 2, 2, 4);
        threaded_work a = {.p = &p, .operation = P1_RECV}, b = {.p = &p, .index = 1, .operation = P1_RECV};
        cmeta_thread_t ta, tb; start(&a, &ta); drive(&p, 0, 3);
        start(&b, &tb); drive(&p, 1, 0); join(&tb); check_worker(&b, TURBOWASM_OK, TURBOWASM_WASI_ERRNO_NOMEM);
        check_equal(p.reads, 1u);
        p.ready = true; check_equal(turbowasm_wasi_preview1_advance(&p.wasi), TURBOWASM_OK);
        drive(&p, 2, 0); join(&ta); check_worker(&a, TURBOWASM_OK, 0);
        start(&b, &tb); drive(&p, 3, 0); join(&tb); check_worker(&b, TURBOWASM_OK, 0); teardown(&p, 2);
    }
    it("registers worker poll clocks and computes owner deadlines") {
        threaded_probe p; setup(&p, 1, 1);
        p1_subscription(&p.guests[0], 0, 42, TURBOWASM_WASI_EVENT_CLOCK, TURBOWASM_WASI_CLOCKID_MONOTONIC);
        p1_store(&p.guests[0], 1048, 500); /* relative deadline: 100 + 500 */
        threaded_work w = {.p = &p, .operation = P1_POLL}; cmeta_thread_t thread; start(&w, &thread);
        drive(&p, 0, 1);
        uint64_t delay = 0; check_equal(turbowasm_wasi_preview1_next_timeout(&p.wasi, &delay), TURBOWASM_OK);
        check_equal(delay, UINT64_C(500));
        check_equal(turbowasm_wasi_preview1_shutdown_request(&p.wasi), TURBOWASM_OK);
        drive(&p, 1, 0); join(&thread); check_worker(&w, TURBOWASM_OK, TURBOWASM_WASI_ERRNO_INTR); teardown(&p, 1);
    }
    it("counts ordinary file callbacks during shutdown without transferring them to the owner") {
        threaded_probe p; setup(&p, 0, 1);
        turbowasm_wasi_fs_descriptor descriptor;
        check_equal(turbowasm_wasi_fs_bind_descriptor(&p.fs, 4,
            (turbowasm_wasi_fs_file){WORKERS, 1}, false, NULL, &descriptor), TURBOWASM_OK);
        p1_guest_init(&p.guests[0], &p.wasi); p1_iovec(&p.guests[0], 4);
        threaded_work w = {.p = &p, .operation = P1_READ}; cmeta_thread_t thread; start(&w, &thread);
        cmeta_mutex_lock(&p.mutex);
        while (!p.scan_entered) cmeta_cond_wait(&p.changed, &p.mutex);
        cmeta_mutex_unlock(&p.mutex);
        check_equal(turbowasm_wasi_preview1_shutdown_request(&p.wasi), TURBOWASM_OK);
        bool complete = true;
        check_equal(turbowasm_wasi_preview1_shutdown_poll(&p.wasi, &complete), TURBOWASM_OK); check_false(complete);
        check_equal(turbowasm_wasi_preview1_destroy_checked(&p.wasi), TURBOWASM_INVALID_ARGUMENT);
        cmeta_mutex_lock(&p.mutex); p.release_scan = true;
        cmeta_cond_broadcast(&p.changed); cmeta_mutex_unlock(&p.mutex);
        drive(&p, 1, 0); join(&thread); check_worker(&w, TURBOWASM_OK, 0);
        check_equal(turbowasm_wasi_preview1_shutdown_poll(&p.wasi, &complete), TURBOWASM_OK); check_true(complete);
        p1_guest_destroy(&p.guests[0]); check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 4), 0u); teardown(&p, 0);
    }
    it("rejects invalid threaded configuration while preserving existing ownership") {
        threaded_probe p; setup(&p, 0, 1);
        turbowasm_wasi_preview1 other = {0}; turbowasm_wasi_preview1_config_v2 v = config(&p, 1);
        turbowasm_wasi_preview1_threaded_config t = {1, 1000000, wake_owner, &p};
        check_equal(turbowasm_wasi_preview1_init_threaded(&other, &v, &t), TURBOWASM_INVALID_ARGUMENT);
        check(other.impl == NULL);
        t.interrupt_interval_ns = 0;
        check_equal(turbowasm_wasi_preview1_init_threaded(&other, &v, &t), TURBOWASM_INVALID_ARGUMENT);
        t.interrupt_interval_ns = UINT64_C(1000000001);
        check_equal(turbowasm_wasi_preview1_init_threaded(&other, &v, &t), TURBOWASM_INVALID_ARGUMENT);
        t.interrupt_interval_ns = 1; t.request_capacity = SIZE_MAX;
        check_equal(turbowasm_wasi_preview1_init_threaded(&other, &v, &t), TURBOWASM_INVALID_ARGUMENT);
        check(other.impl == NULL); teardown(&p, 0);
    }
}
