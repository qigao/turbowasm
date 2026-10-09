#include "tinytest.h"
#include "wasi_fs_private.h"
#include <salts/thread.h>

enum operation { OP_NONE, OP_READ, OP_CLOSE, OP_OPEN, OP_FLAGS };
typedef struct probe {
    turbowasm_wasi_fs fs;
    cmeta_mutex_t mutex;
    cmeta_cond_t cond;
    enum operation blocked;
    bool entered, released;
    uint32_t error, calls[5];
    uint32_t reentrant_close;
    bool reentrant_info;
} probe;
typedef struct work {
    probe *p;
    enum operation operation;
    uint32_t error, output;
} work;

/* The controller observes callback entry before issuing competing operations.
 * No sleeps or scheduler timing assumptions are needed. */
static uint32_t effect(probe *p, enum operation op, uint64_t object) {
    cmeta_mutex_lock(&p->mutex);
    ++p->calls[op];
    if (p->blocked == op && (op == OP_OPEN || object == 4)) {
        p->entered = true;
        cmeta_cond_broadcast(&p->cond);
        while (!p->released) cmeta_cond_wait(&p->cond, &p->mutex);
    }
    uint32_t error = p->error;
    cmeta_mutex_unlock(&p->mutex);
    return error;
}
static uint32_t close_file(void *context, turbowasm_wasi_fs_file file) {
    return effect(context, OP_CLOSE, file.object);
}
static uint32_t read_file(void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers, size_t count, uint32_t *out) {
    probe *p = context;
    (void)buffers; (void)count;
    if (file.object == 4) {
        turbowasm_wasi_fs_descriptor_info info;
        p->reentrant_info = turbowasm_wasi_fs_descriptor_info_get(&p->fs, 4, &info);
        p->reentrant_close = turbowasm_wasi_fs_close_fd(&p->fs, 4);
    }
    *out = 0;
    return effect(p, OP_READ, file.object);
}
static uint32_t write_file(void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers, size_t count, uint32_t *out) {
    (void)context; (void)file; (void)buffers; (void)count;
    *out = 0; return 0;
}
static uint32_t open_file(void *context, turbowasm_wasi_fs_file dir, uint32_t dirflags,
    const uint8_t *path, size_t length, uint32_t oflags, uint64_t base,
    uint64_t inheriting, uint32_t flags, turbowasm_wasi_fs_file *out) {
    (void)dirflags; (void)path; (void)length; (void)oflags;
    (void)base; (void)inheriting; (void)flags;
    uint32_t error = effect(context, OP_OPEN, dir.object);
    if (!error) *out = (turbowasm_wasi_fs_file){5, 1};
    return error;
}
static uint32_t flags_file(void *context, turbowasm_wasi_fs_file file, uint16_t flags) {
    (void)flags;
    return effect(context, OP_FLAGS, file.object);
}
static uint32_t open_child(probe *p, uint32_t *out) {
    return turbowasm_wasi_fs_path_open(&p->fs, 3, 0, (const uint8_t *)"child", 5,
        0, TURBOWASM_WASI_RIGHT_FD_READ | TURBOWASM_WASI_RIGHT_FD_WRITE, 0, 0, out);
}
static void worker(void *context) {
    work *w = context;
    switch (w->operation) {
    case OP_READ: w->error = turbowasm_wasi_fs_fd_read(&w->p->fs, 4, NULL, 0, &w->output); break;
    case OP_CLOSE: w->error = turbowasm_wasi_fs_close_fd(&w->p->fs, 4); break;
    case OP_OPEN: w->error = open_child(w->p, &w->output); break;
    case OP_FLAGS: w->error = tw_wasi_fd_set_flags(&w->p->fs, 4, 1); break;
    default: break;
    }
}
static void start(probe *p, work *w, cmeta_thread_t *thread, enum operation op) {
    p->blocked = op; p->entered = p->released = false;
    *w = (work){.p = p, .operation = op};
    check_equal(cmeta_thread_create(thread, worker, w), SALTS_OK);
    cmeta_mutex_lock(&p->mutex);
    while (!p->entered) cmeta_cond_wait(&p->cond, &p->mutex);
    cmeta_mutex_unlock(&p->mutex);
}
static void finish(probe *p, cmeta_thread_t *thread) {
    cmeta_mutex_lock(&p->mutex);
    p->released = true;
    cmeta_cond_broadcast(&p->cond);
    cmeta_mutex_unlock(&p->mutex);
    check_equal(cmeta_thread_join(thread), SALTS_OK);
    cmeta_thread_destroy(thread);
}
static uint32_t call_count(probe *p, enum operation op) {
    cmeta_mutex_lock(&p->mutex);
    uint32_t count = p->calls[op];
    cmeta_mutex_unlock(&p->mutex);
    return count;
}
static void set_error(probe *p, uint32_t error) {
    cmeta_mutex_lock(&p->mutex); p->error = error; cmeta_mutex_unlock(&p->mutex);
}
static probe p;
static work w;
static cmeta_thread_t thread;
static turbowasm_wasi_fs_descriptor original;

spec("concurrent filesystem admission") {
    before_each() {
        p = (probe){0}; w = (work){0}; thread = NULL;
        cmeta_mutex_init(&p.mutex); cmeta_cond_init(&p.cond);
        check(p.mutex != NULL); check(p.cond != NULL);
        turbowasm_wasi_fs_config config = {.descriptor_capacity = 3,
            .provider = {.context = &p, .close = close_file, .read = read_file,
                .write = write_file, .path_open = open_file, .set_flags = flags_file}};
        check_equal(turbowasm_wasi_fs_init(&p.fs, &config), TURBOWASM_OK);
        turbowasm_wasi_fs_descriptor root;
        check_equal(turbowasm_wasi_fs_bind_descriptor(&p.fs, 3,
            (turbowasm_wasi_fs_file){3, 1}, true, "/root", &root), TURBOWASM_OK);
        check_equal(turbowasm_wasi_fs_bind_descriptor(&p.fs, 4,
            (turbowasm_wasi_fs_file){4, 1}, false, NULL, &original), TURBOWASM_OK);
    }
    after_each() {
        if (thread) finish(&p, &thread);
        set_error(&p, 0);
        for (uint32_t fd = 3; fd <= 5; ++fd) (void)turbowasm_wasi_fs_close_fd(&p.fs, fd);
        check_equal(turbowasm_wasi_fs_destroy(&p.fs), TURBOWASM_OK);
        cmeta_cond_destroy(&p.cond); cmeta_mutex_destroy(&p.mutex);
    }
    it("pins blocked reads, permits provider reentry and independent descriptors") {
        start(&p, &w, &thread, OP_READ);
        check(p.reentrant_info); check_equal(p.reentrant_close, TURBOWASM_WASI_ERRNO_BUSY);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 4), TURBOWASM_WASI_ERRNO_BUSY);
        check_equal(call_count(&p, OP_CLOSE), 0u);
        turbowasm_wasi_fs_descriptor replacement;
        check_equal(turbowasm_wasi_fs_bind_descriptor(&p.fs, 4,
            (turbowasm_wasi_fs_file){40, 1}, false, NULL, &replacement), TURBOWASM_INVALID_ARGUMENT);
        uint32_t count;
        check_equal(turbowasm_wasi_fs_fd_read(&p.fs, 3, NULL, 0, &count), 0u);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 3), 0u);
        finish(&p, &thread); check_equal(w.error, 0u);
        check_equal(turbowasm_wasi_fs_close_descriptor(&p.fs, original), 0u);
        check_equal(turbowasm_wasi_fs_bind_descriptor(&p.fs, 4,
            (turbowasm_wasi_fs_file){40, 1}, false, NULL, &replacement), TURBOWASM_OK);
        check_equal(turbowasm_wasi_fs_close_descriptor(&p.fs, original), TURBOWASM_WASI_ERRNO_BADF);
    }
    it("keeps a failed close retryable and rejects duplicate close and new I/O") {
        set_error(&p, TURBOWASM_WASI_ERRNO_IO);
        start(&p, &w, &thread, OP_CLOSE);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 4), TURBOWASM_WASI_ERRNO_BUSY);
        uint32_t count;
        check_equal(turbowasm_wasi_fs_fd_read(&p.fs, 4, NULL, 0, &count), TURBOWASM_WASI_ERRNO_BUSY);
        check_equal(tw_wasi_fd_set_rights(&p.fs, 4, 0, 0), TURBOWASM_WASI_ERRNO_BUSY);
        check_equal(call_count(&p, OP_CLOSE), 1u);
        finish(&p, &thread); check_equal(w.error, TURBOWASM_WASI_ERRNO_IO);
        turbowasm_wasi_fs_descriptor_info info;
        check(turbowasm_wasi_fs_retained_descriptor_info_get(&p.fs, original, &info));
        set_error(&p, 0);
        check_equal(turbowasm_wasi_fs_close_descriptor(&p.fs, original), 0u);
        check_equal(call_count(&p, OP_CLOSE), 2u);
    }
    it("reserves before open and intersects child rights at publication") {
        start(&p, &w, &thread, OP_OPEN);
        uint32_t fd = 99;
        check_equal(open_child(&p, &fd), TURBOWASM_WASI_ERRNO_MFILE);
        check_equal(fd, 0u); check_equal(call_count(&p, OP_OPEN), 1u);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 3), TURBOWASM_WASI_ERRNO_BUSY);
        check_equal(tw_wasi_fd_set_rights(&p.fs, 3, UINT64_MAX,
            TURBOWASM_WASI_RIGHT_FD_READ), 0u);
        finish(&p, &thread); check_equal(w.error, 0u); check_equal(w.output, 5u);
        turbowasm_wasi_fs_descriptor_info info;
        check(turbowasm_wasi_fs_descriptor_info_get(&p.fs, 5, &info));
        check_equal(info.rights_base, (uint64_t)TURBOWASM_WASI_RIGHT_FD_READ);
        check_equal(call_count(&p, OP_CLOSE), 0u);
    }
    it("releases failed open reservations without publishing or closing an identity") {
        set_error(&p, TURBOWASM_WASI_ERRNO_IO);
        start(&p, &w, &thread, OP_OPEN);
        finish(&p, &thread); check_equal(w.error, TURBOWASM_WASI_ERRNO_IO);
        check_equal(w.output, 0u); check_equal(call_count(&p, OP_CLOSE), 0u);
        set_error(&p, 0);
        uint32_t fd;
        check_equal(open_child(&p, &fd), 0u); check_equal(fd, 5u);
    }
    it("commits one flags transaction and leaves concurrent observers on the old flags") {
        start(&p, &w, &thread, OP_FLAGS);
        check_equal(tw_wasi_fd_set_flags(&p.fs, 4, 0), TURBOWASM_WASI_ERRNO_BUSY);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 4), TURBOWASM_WASI_ERRNO_BUSY);
        tw_wasi_fd_lease lease = {0};
        check_equal(tw_wasi_fd_acquire(&p.fs, 4, 0, &lease), 0u);
        check_equal(lease.flags, 0u); tw_wasi_fd_release(&p.fs, &lease);
        finish(&p, &thread); check_equal(w.error, 0u);
        check_equal(tw_wasi_fd_acquire(&p.fs, 4, 0, &lease), 0u);
        check_equal(lease.flags, 1u); tw_wasi_fd_release(&p.fs, &lease);
        check_equal(call_count(&p, OP_FLAGS), 1u);
    }
    it("keeps the preopen name alive until the protected host copy releases its pin") {
        turbowasm_wasi_fs_descriptor_info info;
        check_equal(tw_wasi_fd_preopen_pin(&p.fs, 3, &info), 0u);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 3), TURBOWASM_WASI_ERRNO_BUSY);
        check_equal(info.guest_path, "/root");
        tw_wasi_fd_preopen_unpin(&p.fs, info.descriptor);
        check_equal(turbowasm_wasi_fs_close_fd(&p.fs, 3), 0u);
    }
}
