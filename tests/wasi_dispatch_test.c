#include <tinytest.h>
#include "wasi_dispatch_private.h"
#include "wasi_provider_private.h"

typedef struct probe {
    tw_wasi_dispatch dispatch;
    turbowasm_wasi_fs fs;
    cmeta_mutex_t mutex;
    cmeta_cond_t changed;
    size_t wakes, done, effects, wrong_owner, regular_effects;
    bool block_wake, wake_released;
    const void *owner;
    turbowasm_status reentrant_advance, reentrant_destroy, reentrant_stop;
} probe;

static void wake(void *context) {
    probe *p = context;
    cmeta_mutex_lock(&p->mutex);
    ++p->wakes;
    cmeta_cond_broadcast(&p->changed);
    while (p->block_wake && !p->wake_released) cmeta_cond_wait(&p->changed, &p->mutex);
    cmeta_mutex_unlock(&p->mutex);
}
static void completed(probe *p) {
    cmeta_mutex_lock(&p->mutex);
    ++p->done; ++p->wakes;
    cmeta_cond_broadcast(&p->changed);
    cmeta_mutex_unlock(&p->mutex);
}
static void init(probe *p, size_t capacity) {
    *p = (probe){.owner = cmeta_thread_current_token()};
    cmeta_mutex_init(&p->mutex); cmeta_cond_init(&p->changed);
    check(p->mutex && p->changed);
    check_equal(tw_wasi_dispatch_init(&p->dispatch, capacity, wake, p), TURBOWASM_OK);
}
static void destroy(probe *p) {
    check_equal(tw_wasi_dispatch_destroy(&p->dispatch), TURBOWASM_OK);
    cmeta_cond_destroy(&p->changed); cmeta_mutex_destroy(&p->mutex);
}
static void wait_wake(probe *p, size_t target) {
    cmeta_mutex_lock(&p->mutex);
    while (p->wakes < target) cmeta_cond_wait(&p->changed, &p->mutex);
    cmeta_mutex_unlock(&p->mutex);
}
static void drive(probe *p, size_t target) {
    size_t seen = 0;
    cmeta_mutex_lock(&p->mutex);
    while (p->done < target) {
        while (p->done < target && p->wakes == seen) cmeta_cond_wait(&p->changed, &p->mutex);
        seen = p->wakes;
        cmeta_mutex_unlock(&p->mutex);
        check_equal(tw_wasi_dispatch_advance(&p->dispatch), TURBOWASM_OK);
        cmeta_mutex_lock(&p->mutex);
    }
    cmeta_mutex_unlock(&p->mutex);
}
static void join(cmeta_thread_t *thread) {
    check_equal(cmeta_thread_join(thread), SALTS_OK); cmeta_thread_destroy(thread);
}
static uint32_t effect(void *context) {
    probe *p = context;
    ++p->effects;
    if (p->owner != cmeta_thread_current_token()) ++p->wrong_owner;
    p->reentrant_advance = tw_wasi_dispatch_advance(&p->dispatch);
    p->reentrant_destroy = tw_wasi_dispatch_destroy(&p->dispatch);
    p->reentrant_stop = tw_wasi_dispatch_stop(&p->dispatch);
    return 17;
}
typedef struct work {
    probe *p;
    bool cleanup, mandatory;
    uint32_t error;
    unsigned repetitions, busy;
} work;
static void request(void *context) {
    work *w = context;
    unsigned count = w->repetitions ? w->repetitions : 1;
    for (unsigned i = 0; i < count; ++i) {
        do {
            w->error = tw_wasi_dispatch_call(&w->p->dispatch, effect, w->p, w->cleanup, w->mandatory);
            if (w->error == TURBOWASM_WASI_ERRNO_BUSY && w->repetitions) {
                ++w->busy;
                cmeta_thread_yield();
            } else break;
        } while (true);
        if (w->error != 17) break;
    }
    completed(w->p);
}
static void start(work *w, cmeta_thread_t *thread) {
    check_equal(cmeta_thread_create(thread, request, w), SALTS_OK);
}

static void bounded_shutdown(void) {
    probe p; init(&p, 1);
    work a = {.p = &p}, b = {.p = &p}, cleanup = {.p = &p, .cleanup = true, .mandatory = true};
    cmeta_thread_t ta, tb, tc;
    start(&a, &ta); wait_wake(&p, 1);
    check_equal(tw_wasi_dispatch_destroy(&p.dispatch), TURBOWASM_INVALID_ARGUMENT);
    start(&b, &tb); join(&tb);
    check_equal(b.error, TURBOWASM_WASI_ERRNO_BUSY);
    check_equal(p.effects, (size_t)0);
    start(&cleanup, &tc);
    cmeta_mutex_lock(&p.dispatch.mutex);
    while (!p.dispatch.cleanup_waiters) cmeta_cond_wait(&p.dispatch.changed, &p.dispatch.mutex);
    cmeta_mutex_unlock(&p.dispatch.mutex);
    check_equal(tw_wasi_dispatch_stop(&p.dispatch), TURBOWASM_OK);
    start(&b, &tb); join(&tb);
    check_equal(b.error, TURBOWASM_WASI_ERRNO_INTR);
    drive(&p, 4); join(&ta); join(&tc);
    check_equal(a.error, 17u); check_equal(cleanup.error, 17u);
    check_equal(p.effects, (size_t)2); check_equal(p.wrong_owner, (size_t)0);
    check_equal(p.reentrant_advance, TURBOWASM_INVALID_ARGUMENT);
    check_equal(p.reentrant_destroy, TURBOWASM_INVALID_ARGUMENT);
    check_equal(p.reentrant_stop, TURBOWASM_INVALID_ARGUMENT);
    destroy(&p);
}

static void acknowledgement_lifetime(void) {
    probe p; init(&p, 1); p.block_wake = true;
    work w = {.p = &p}; cmeta_thread_t thread; start(&w, &thread); wait_wake(&p, 1);
    check_equal(tw_wasi_dispatch_advance(&p.dispatch), TURBOWASM_OK);
    /* The provider is done, but the caller still owns its acknowledgement
     * path and wake context. Destruction must reject this intermediate state. */
    check_equal(tw_wasi_dispatch_destroy(&p.dispatch), TURBOWASM_INVALID_ARGUMENT);
    cmeta_mutex_lock(&p.mutex); p.wake_released = true;
    cmeta_cond_broadcast(&p.changed); cmeta_mutex_unlock(&p.mutex);
    join(&thread); check_equal(w.error, 17u); destroy(&p);
}

static uint32_t file_effect(probe *p, turbowasm_wasi_fs_file file) {
    if (file.object == 3) {
        ++p->regular_effects;
        if (p->owner == cmeta_thread_current_token()) ++p->wrong_owner;
    } else {
        ++p->effects;
        if (p->owner != cmeta_thread_current_token()) ++p->wrong_owner;
    }
    return 0;
}
static uint32_t close_file(void *c, turbowasm_wasi_fs_file f) { return file_effect(c, f); }
static uint32_t read_file(void *c, turbowasm_wasi_fs_file f,
    const turbowasm_wasi_buffer *b, size_t n, uint32_t *out) {
    (void)b; (void)n; *out = 0; return file_effect(c, f);
}
static uint32_t write_file(void *c, turbowasm_wasi_fs_file f,
    const turbowasm_wasi_const_buffer *b, size_t n, uint32_t *out) {
    (void)b; (void)n; *out = 0; return file_effect(c, f);
}
static uint32_t seek_file(void *c, turbowasm_wasi_fs_file f, int64_t off, uint8_t whence, uint64_t *out) {
    (void)off; (void)whence; *out = 0; return file_effect(c, f);
}
static uint32_t tell_file(void *c, turbowasm_wasi_fs_file f, uint64_t *out) {
    *out = 0; return file_effect(c, f);
}
static uint32_t stat_file(void *c, turbowasm_wasi_fs_file f, turbowasm_wasi_fs_stat *out) {
    *out = (turbowasm_wasi_fs_stat){.file_type = TURBOWASM_WASI_FILETYPE_SOCKET_STREAM};
    return file_effect(c, f);
}
static uint32_t open_file(void *c, turbowasm_wasi_fs_file f, uint32_t df,
    const uint8_t *path, size_t len, uint32_t of, uint64_t base, uint64_t inheriting,
    uint32_t flags, turbowasm_wasi_fs_file *out) {
    (void)df; (void)path; (void)len; (void)of; (void)base; (void)inheriting; (void)flags;
    *out = (turbowasm_wasi_fs_file){6, 1}; return file_effect(c, f);
}
static uint32_t path_stat(void *c, turbowasm_wasi_fs_file f, uint32_t flags,
    const uint8_t *path, size_t len, turbowasm_wasi_fs_stat *out) {
    (void)flags; (void)path; (void)len; return stat_file(c, f, out);
}
static uint32_t mutate(void *c, turbowasm_wasi_fs_file f, const uint8_t *path, size_t len) {
    (void)path; (void)len; return file_effect(c, f);
}
static uint32_t readdir_file(void *c, turbowasm_wasi_fs_file f, uint64_t cookie,
    turbowasm_wasi_fs_dirent *out, bool *entry) {
    (void)cookie; (void)out; *entry = false; return file_effect(c, f);
}
static uint32_t rename_file(void *c, turbowasm_wasi_fs_file f, const uint8_t *src, size_t sn,
    turbowasm_wasi_fs_file dst, const uint8_t *target, size_t tn) {
    (void)src; (void)sn; (void)dst; (void)target; (void)tn; return file_effect(c, f);
}
static uint32_t retain(void *c, turbowasm_wasi_fs_file f) { return file_effect(c, f); }
static void release(void *c, turbowasm_wasi_fs_file f) { (void)file_effect(c, f); }
static void finish(void *c, turbowasm_wasi_fs_file f, uint8_t dir) { (void)dir; (void)file_effect(c, f); }
static uint32_t ready(void *c, turbowasm_wasi_fs_file f, uint8_t dir, turbowasm_wasi_readiness *out) {
    (void)dir; *out = (turbowasm_wasi_readiness){.ready = true}; return file_effect(c, f);
}
static uint32_t accept_file(void *c, turbowasm_wasi_fs_file f, turbowasm_wasi_fs_file *out) {
    *out = (turbowasm_wasi_fs_file){7, 1}; return file_effect(c, f);
}
static uint32_t recv_file(void *c, turbowasm_wasi_fs_file f, const turbowasm_wasi_buffer *b,
    size_t n, uint16_t flags, bool nonblock, uint32_t *out, uint16_t *out_flags) {
    (void)flags; (void)nonblock; *out_flags = 0; return read_file(c, f, b, n, out);
}
static uint32_t shutdown_file(void *c, turbowasm_wasi_fs_file f, uint8_t how) {
    (void)how; return file_effect(c, f);
}
static turbowasm_wasi_descriptor_ops provider(probe *p) {
    return (turbowasm_wasi_descriptor_ops){.size = sizeof(turbowasm_wasi_descriptor_ops), .api_version = 1,
        .file = {.context = p, .close = close_file, .read = read_file, .write = write_file,
            .seek = seek_file, .tell = tell_file, .stat = stat_file, .path_open = open_file,
            .path_stat = path_stat, .path_create_directory = mutate, .path_remove_directory = mutate,
            .path_unlink_file = mutate, .readdir = readdir_file, .path_rename = rename_file},
        .retain = retain, .release = release, .finish = finish, .ready = ready,
        .accept = accept_file, .recv = recv_file, .send = write_file, .shutdown = shutdown_file};
}

typedef struct fs_work {
    probe *p; unsigned line;
    turbowasm_status wrong_progress, wrong_bind, wrong_stop, wrong_destroy;
} fs_work;
#define REQUIRE_OK(expr) do { if ((expr) != 0) { w->line = __LINE__; goto done; } } while (0)
static void fs_worker(void *context) {
    fs_work *w = context; probe *p = w->p;
    uint32_t n, child_fd; uint64_t offset; uint16_t flags;
    turbowasm_wasi_fs_stat stat; turbowasm_wasi_fs_dirent entry; bool has_entry;
    turbowasm_wasi_readiness readiness;
    turbowasm_wasi_fs_file child = {0}; tw_wasi_fd_lease lease = {0};
    w->wrong_progress = tw_wasi_dispatch_advance(&p->dispatch);
    w->wrong_stop = tw_wasi_dispatch_stop(&p->dispatch);
    w->wrong_destroy = tw_wasi_dispatch_destroy(&p->dispatch);
    turbowasm_wasi_descriptor_ops ops = provider(p); turbowasm_wasi_fs_descriptor descriptor;
    child = (turbowasm_wasi_fs_file){9, 1};
    w->wrong_bind = turbowasm_wasi_fs_bind_socket_move(&p->fs, 9, &ops, &child,
        TURBOWASM_WASI_FILETYPE_SOCKET_STREAM, 0, UINT64_MAX, UINT64_MAX, &descriptor);
    if (child.generation != 1) { w->line = __LINE__; goto done; }
    REQUIRE_OK(turbowasm_wasi_fs_fd_read(&p->fs, 3, NULL, 0, &n));
    REQUIRE_OK(turbowasm_wasi_fs_close_fd(&p->fs, 3));
    REQUIRE_OK(turbowasm_wasi_fs_fd_read(&p->fs, 4, NULL, 0, &n));
    REQUIRE_OK(turbowasm_wasi_fs_fd_write(&p->fs, 4, NULL, 0, &n));
    REQUIRE_OK(turbowasm_wasi_fs_fd_seek(&p->fs, 4, 0, 0, &offset));
    REQUIRE_OK(turbowasm_wasi_fs_fd_tell(&p->fs, 4, &offset));
    REQUIRE_OK(turbowasm_wasi_fs_fd_stat(&p->fs, 4, &stat));
    const uint8_t path[] = "p";
    REQUIRE_OK(turbowasm_wasi_fs_path_open(&p->fs, 4, 0, path, 1, 0, UINT64_MAX, 0, 0, &child_fd));
    REQUIRE_OK(turbowasm_wasi_fs_path_stat(&p->fs, 4, 0, path, 1, &stat));
    REQUIRE_OK(turbowasm_wasi_fs_path_create_directory(&p->fs, 4, path, 1));
    REQUIRE_OK(turbowasm_wasi_fs_path_remove_directory(&p->fs, 4, path, 1));
    REQUIRE_OK(turbowasm_wasi_fs_path_unlink_file(&p->fs, 4, path, 1));
    REQUIRE_OK(turbowasm_wasi_fs_fd_readdir(&p->fs, 4, 0, &entry, &has_entry));
    REQUIRE_OK(turbowasm_wasi_fs_path_rename(&p->fs, 4, path, 1, child_fd, path, 1));
    REQUIRE_OK(tw_wasi_fd_acquire(&p->fs, child_fd, 0, &lease)); /* stat + retain */
    tw_wasi_fd_release(&p->fs, &lease);
    REQUIRE_OK(turbowasm_wasi_fs_close_fd(&p->fs, child_fd));
    REQUIRE_OK(tw_wasi_fd_acquire(&p->fs, 4, 0, &lease));
    REQUIRE_OK(tw_wasi_fd_claim(&p->fs, &lease, 1));
    REQUIRE_OK(tw_wasi_fd_ready(&p->fs, &lease, TURBOWASM_WASI_EVENT_FD_READ, &readiness));
    REQUIRE_OK(tw_wasi_provider_call(&lease, &(tw_wasi_provider_request){.operation = TW_PROVIDER_ACCEPT, .as.accept = &child}));
    REQUIRE_OK(tw_wasi_provider_call(&lease, &(tw_wasi_provider_request){.operation = TW_PROVIDER_RECV,
        .as.recv = {NULL, 0, 0, false, &n, &flags}}));
    REQUIRE_OK(tw_wasi_provider_call(&lease, &(tw_wasi_provider_request){.operation = TW_PROVIDER_SEND,
        .as.write = {NULL, 0, &n}}));
    REQUIRE_OK(tw_wasi_provider_call(&lease, &(tw_wasi_provider_request){.operation = TW_PROVIDER_SHUTDOWN, .as.shutdown = 1}));
    tw_wasi_fd_lease child_lease = lease; child_lease.file = child;
    REQUIRE_OK(tw_wasi_provider_call(&child_lease, &(tw_wasi_provider_request){.operation = TW_PROVIDER_CLOSE}));
    REQUIRE_OK(turbowasm_wasi_fs_close_fd(&p->fs, 4));
    tw_wasi_fd_release(&p->fs, &lease); /* finish + release after close */
done:
    completed(p);
}
#undef REQUIRE_OK

static void filesystem_routing(void) {
    probe p; init(&p, 2);
    turbowasm_wasi_descriptor_ops ops = provider(&p);
    turbowasm_wasi_fs_config config = {.descriptor_capacity = 4, .provider = ops.file};
    check_equal(turbowasm_wasi_fs_init(&p.fs, &config), TURBOWASM_OK);
    turbowasm_wasi_fs_descriptor descriptor;
    check_equal(turbowasm_wasi_fs_bind_descriptor(&p.fs, 3, (turbowasm_wasi_fs_file){3, 1},
        false, NULL, &descriptor), TURBOWASM_OK);
    check_equal(tw_wasi_fs_attach_dispatch(&p.fs, &p.dispatch), TURBOWASM_OK);
    check_equal(tw_wasi_fs_attach_dispatch(&p.fs, &p.dispatch), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_wasi_fs_destroy(&p.fs), TURBOWASM_INVALID_ARGUMENT);
    turbowasm_wasi_fs_file file = {4, 1};
    check_equal(turbowasm_wasi_fs_bind_socket_move(&p.fs, 4, &ops, &file,
        TURBOWASM_WASI_FILETYPE_SOCKET_STREAM, 0, UINT64_MAX, UINT64_MAX, &descriptor), TURBOWASM_OK);
    check_equal(tw_wasi_fs_detach_dispatch(&p.fs, &p.dispatch), TURBOWASM_INVALID_ARGUMENT);
    fs_work w = {.p = &p}; cmeta_thread_t thread;
    check_equal(cmeta_thread_create(&thread, fs_worker, &w), SALTS_OK);
    /* The worker finishes both ordinary file callbacks before the first
     * queued socket callback, with no owner progress. */
    wait_wake(&p, 1); check_equal(p.regular_effects, (size_t)2);
    check_equal(p.effects, (size_t)0);
    drive(&p, 1); join(&thread);
    check_equal(w.line, 0u); check_equal(w.wrong_progress, TURBOWASM_INVALID_ARGUMENT);
    check_equal(w.wrong_bind, TURBOWASM_INVALID_ARGUMENT);
    check_equal(w.wrong_stop, TURBOWASM_INVALID_ARGUMENT);
    check_equal(w.wrong_destroy, TURBOWASM_INVALID_ARGUMENT);
    check_equal(p.wrong_owner, (size_t)0); check(p.effects >= 24);
    check_equal(tw_wasi_fs_detach_dispatch(&p.fs, &p.dispatch), TURBOWASM_OK);
    check_equal(turbowasm_wasi_fs_destroy(&p.fs), TURBOWASM_OK); destroy(&p);
}

spec("Bounded WASI owner dispatch") {
    it("rejects full admission before effects and drains mandatory cleanup after shutdown") { bounded_shutdown(); }
    it("retains caller lifetime after provider completion until acknowledgement") { acknowledgement_lifetime(); }
    it("routes socket lifecycle and file callbacks without serializing ordinary files") { filesystem_routing(); }
    it("wraps a bounded queue repeatedly and preserves callback errors") {
        probe p; init(&p, 2);
        for (size_t i = 0; i < 64; ++i) {
            work w = {.p = &p}; cmeta_thread_t thread; start(&w, &thread);
            drive(&p, i + 1); join(&thread); check_equal(w.error, 17u);
        }
        check_equal(p.effects, (size_t)64); check_equal(p.wrong_owner, (size_t)0); destroy(&p);
    }
    it("accepts concurrent producers through a small queue without replay or loss") {
        probe p; init(&p, 2);
        enum { WORKERS = 8, ROUNDS = 32 };
        work workers[WORKERS]; cmeta_thread_t threads[WORKERS];
        for (size_t i = 0; i < WORKERS; ++i) {
            workers[i] = (work){.p = &p, .repetitions = ROUNDS}; start(&workers[i], &threads[i]);
        }
        drive(&p, WORKERS);
        for (size_t i = 0; i < WORKERS; ++i) {
            join(&threads[i]); check_equal(workers[i].error, 17u);
        }
        check_equal(p.effects, (size_t)(WORKERS * ROUNDS));
        check_equal(p.wrong_owner, (size_t)0); destroy(&p);
    }
    it("rejects attachment with live sockets and keeps an empty attached table alive") {
        probe p; init(&p, 1);
        turbowasm_wasi_descriptor_ops ops = provider(&p);
        turbowasm_wasi_fs_config config = {.descriptor_capacity = 2, .provider = ops.file};
        check_equal(turbowasm_wasi_fs_init(&p.fs, &config), TURBOWASM_OK);
        turbowasm_wasi_fs_file file = {4, 1}; turbowasm_wasi_fs_descriptor descriptor;
        check_equal(turbowasm_wasi_fs_bind_socket_move(&p.fs, 4, &ops, &file,
            TURBOWASM_WASI_FILETYPE_SOCKET_STREAM, 0, UINT64_MAX, 0, &descriptor), TURBOWASM_OK);
        check_equal(tw_wasi_fs_attach_dispatch(&p.fs, &p.dispatch), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 4), 0u);
        check_equal(tw_wasi_fs_attach_dispatch(&p.fs, &p.dispatch), TURBOWASM_OK);
        check_equal(turbowasm_wasi_fs_destroy(&p.fs), TURBOWASM_INVALID_ARGUMENT);
        check_equal(tw_wasi_fs_detach_dispatch(&p.fs, &p.dispatch), TURBOWASM_OK);
        check_equal(turbowasm_wasi_fs_destroy(&p.fs), TURBOWASM_OK); destroy(&p);
    }
    it("executes local owner calls directly and rejects reentrant progress") {
        probe p; init(&p, 1);
        check_equal(tw_wasi_dispatch_call(&p.dispatch, effect, &p, false, false), 17u);
        check_equal(p.wakes, (size_t)0); check_equal(p.reentrant_advance, TURBOWASM_INVALID_ARGUMENT);
        check_equal(p.reentrant_destroy, TURBOWASM_INVALID_ARGUMENT);
        check_equal(p.reentrant_stop, TURBOWASM_INVALID_ARGUMENT); destroy(&p);
    }
    it("rejects invalid or overflowing initialization without changing the owner") {
        probe p = {0};
        check_equal(tw_wasi_dispatch_init(&p.dispatch, 0, wake, &p), TURBOWASM_INVALID_ARGUMENT);
        check_equal(tw_wasi_dispatch_init(&p.dispatch, SIZE_MAX, wake, &p), TURBOWASM_INVALID_ARGUMENT);
        check_equal(tw_wasi_dispatch_init(&p.dispatch, 1, NULL, &p), TURBOWASM_INVALID_ARGUMENT);
        check(p.dispatch.owner == NULL && p.dispatch.storage == NULL);
    }
}
