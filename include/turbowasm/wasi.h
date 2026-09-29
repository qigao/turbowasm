#ifndef TURBOWASM_WASI_H
#define TURBOWASM_WASI_H

#include <turbowasm/link.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TURBOWASM_WASI_ERRNO_SUCCESS = 0,
    TURBOWASM_WASI_ERRNO_AGAIN = 6,
    TURBOWASM_WASI_ERRNO_BADF = 8,
    TURBOWASM_WASI_ERRNO_FAULT = 21,
    TURBOWASM_WASI_ERRNO_INTR = 27,
    TURBOWASM_WASI_ERRNO_INVAL = 28,
    TURBOWASM_WASI_ERRNO_IO = 29,
    TURBOWASM_WASI_ERRNO_MFILE = 33,
    TURBOWASM_WASI_ERRNO_NAMETOOLONG = 37,
    TURBOWASM_WASI_ERRNO_NOSYS = 52,
    TURBOWASM_WASI_ERRNO_NOTCAPABLE = 76
};

enum {
    TURBOWASM_WASI_CLOCKID_REALTIME = 0,
    TURBOWASM_WASI_CLOCKID_MONOTONIC = 1
};

enum {
    TURBOWASM_WASI_IOV_MAX = 64
};

enum {
    TURBOWASM_WASI_RIGHT_FD_READ = UINT64_C(1) << 1,
    TURBOWASM_WASI_RIGHT_FD_SEEK = UINT64_C(1) << 2,
    TURBOWASM_WASI_RIGHT_FD_TELL = UINT64_C(1) << 5,
    TURBOWASM_WASI_RIGHT_FD_WRITE = UINT64_C(1) << 6,
    TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY = UINT64_C(1) << 9,
    TURBOWASM_WASI_RIGHT_PATH_OPEN = UINT64_C(1) << 13,
    TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET = UINT64_C(1) << 18,
    TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET = UINT64_C(1) << 21,
    TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY = UINT64_C(1) << 25,
    TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE = UINT64_C(1) << 26
};

enum {
    TURBOWASM_WASI_WHENCE_SET = 0,
    TURBOWASM_WASI_WHENCE_CUR = 1,
    TURBOWASM_WASI_WHENCE_END = 2
};

enum {
    TURBOWASM_WASI_FILETYPE_UNKNOWN = 0,
    TURBOWASM_WASI_FILETYPE_BLOCK_DEVICE = 1,
    TURBOWASM_WASI_FILETYPE_CHARACTER_DEVICE = 2,
    TURBOWASM_WASI_FILETYPE_DIRECTORY = 3,
    TURBOWASM_WASI_FILETYPE_REGULAR_FILE = 4,
    TURBOWASM_WASI_FILETYPE_SOCKET_DGRAM = 5,
    TURBOWASM_WASI_FILETYPE_SOCKET_STREAM = 6,
    TURBOWASM_WASI_FILETYPE_SYMBOLIC_LINK = 7
};

typedef struct turbowasm_wasi_const_buffer {
    const uint8_t *data;
    size_t size;
} turbowasm_wasi_const_buffer;

typedef struct turbowasm_wasi_buffer {
    uint8_t *data;
    size_t size;
} turbowasm_wasi_buffer;

typedef uint32_t (*turbowasm_wasi_clock_time_fn)(
    void *context,
    uint32_t clock_id,
    uint64_t precision_ns,
    uint64_t *out_timestamp_ns);

typedef uint32_t (*turbowasm_wasi_random_fill_fn)(
    void *context,
    uint8_t *buffer,
    size_t length);

typedef uint32_t (*turbowasm_wasi_fd_write_fn)(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written);

typedef uint32_t (*turbowasm_wasi_fd_read_fn)(
    void *context,
    uint32_t fd,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read);

/*
 * Async fd providers receive the live host-call context so optional adapters
 * can suspend/resume through TurboWasm's backend-neutral host-wait contract.
 * They return Preview1 errno values exactly like the synchronous providers.
 */
typedef uint32_t (*turbowasm_wasi_fd_write_async_fn)(
    void *context,
    turbowasm_host_call *call,
    uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written);

typedef uint32_t (*turbowasm_wasi_fd_read_async_fn)(
    void *context,
    turbowasm_host_call *call,
    uint32_t fd,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read);

/*
 * Preview1 proc_exit policy callback.
 *
 * The callback observes the caller instance and exit code before TurboWasm
 * terminates the current Wasm invocation with TURBOWASM_INTERRUPTED. This
 * keeps process/thread-group policy outside Runtime while preserving proc_exit
 * as a non-returning guest operation.
 */
typedef void (*turbowasm_wasi_proc_exit_fn)(
    void *context,
    turbowasm_instance *caller,
    uint32_t exit_code);

struct turbowasm_wasi_fs;

typedef struct turbowasm_wasi_preview1 {
    void *impl;
} turbowasm_wasi_preview1;

typedef struct turbowasm_wasi_preview1_config {
    bool allow_args;
    const char *const *args;
    size_t arg_count;

    bool allow_environ;
    const char *const *environment;
    size_t environment_count;

    bool allow_clock;
    turbowasm_wasi_clock_time_fn clock_time;
    void *clock_context;

    bool allow_random;
    turbowasm_wasi_random_fill_fn random_fill;
    void *random_context;

    bool allow_fd_write;
    turbowasm_wasi_fd_write_fn fd_write;
    void *fd_write_context;

    bool allow_fd_read;
    turbowasm_wasi_fd_read_fn fd_read;
    void *fd_read_context;

    bool allow_proc_exit;
    turbowasm_wasi_proc_exit_fn proc_exit;
    void *proc_exit_context;

    bool allow_filesystem;
    struct turbowasm_wasi_fs *filesystem;

    /*
     * Appended extension fields preserve the established positional layout of
     * earlier Preview1 config members. When allow_fd_* is true, configure
     * exactly one of the synchronous or async callback for that direction.
     */
    turbowasm_wasi_fd_write_async_fn fd_write_async;
    turbowasm_wasi_fd_read_async_fn fd_read_async;
} turbowasm_wasi_preview1_config;

/*
 * Initialize one Preview1 capability object.
 *
 * argv/environment strings are copied and owned by the object. Each environment
 * entry is passed to the guest verbatim (normally "KEY=VALUE").
 */
turbowasm_status turbowasm_wasi_preview1_init(
    turbowasm_wasi_preview1 *wasi,
    const turbowasm_wasi_preview1_config *config);

void turbowasm_wasi_preview1_destroy(
    turbowasm_wasi_preview1 *wasi);

/*
 * Register enabled capabilities in the canonical
 * "wasi_snapshot_preview1" linker namespace.
 *
 * The WASI object must outlive consumer instances whose host bindings refer to
 * it. The linker itself retains its normal destroy-after-instantiation rule.
 */
turbowasm_status turbowasm_wasi_preview1_define(
    turbowasm_wasi_preview1 *wasi,
    turbowasm_linker *linker);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_H */
