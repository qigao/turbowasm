#ifndef TURBOWASM_WASI_H
#define TURBOWASM_WASI_H

#include <turbowasm/link.h>
#include <cmeta/function.h>

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
    TURBOWASM_WASI_ERRNO_BUSY = 10,
    TURBOWASM_WASI_ERRNO_CONNABORTED = 13,
    TURBOWASM_WASI_ERRNO_CONNREFUSED = 14,
    TURBOWASM_WASI_ERRNO_CONNRESET = 15,
    TURBOWASM_WASI_ERRNO_EXIST = 20,
    TURBOWASM_WASI_ERRNO_FAULT = 21,
    TURBOWASM_WASI_ERRNO_FBIG = 22,
    TURBOWASM_WASI_ERRNO_INTR = 27,
    TURBOWASM_WASI_ERRNO_INVAL = 28,
    TURBOWASM_WASI_ERRNO_IO = 29,
    TURBOWASM_WASI_ERRNO_ISDIR = 31,
    TURBOWASM_WASI_ERRNO_MFILE = 33,
    TURBOWASM_WASI_ERRNO_MSGSIZE = 35,
    TURBOWASM_WASI_ERRNO_NAMETOOLONG = 37,
    TURBOWASM_WASI_ERRNO_NOENT = 44,
    TURBOWASM_WASI_ERRNO_NOMEM = 48,
    TURBOWASM_WASI_ERRNO_NOSPC = 51,
    TURBOWASM_WASI_ERRNO_NOSYS = 52,
    TURBOWASM_WASI_ERRNO_NOTDIR = 54,
    TURBOWASM_WASI_ERRNO_NOTEMPTY = 55,
    TURBOWASM_WASI_ERRNO_NOTCONN = 53,
    TURBOWASM_WASI_ERRNO_NOTSOCK = 57,
    TURBOWASM_WASI_ERRNO_NOTSUP = 58,
    TURBOWASM_WASI_ERRNO_PIPE = 64,
    TURBOWASM_WASI_ERRNO_TIMEDOUT = 73,
    TURBOWASM_WASI_ERRNO_XDEV = 75,
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
    TURBOWASM_WASI_RIGHT_FD_FDSTAT_SET_FLAGS = UINT64_C(1) << 3,
    TURBOWASM_WASI_RIGHT_FD_TELL = UINT64_C(1) << 5,
    TURBOWASM_WASI_RIGHT_FD_WRITE = UINT64_C(1) << 6,
    TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY = UINT64_C(1) << 9,
    TURBOWASM_WASI_RIGHT_PATH_OPEN = UINT64_C(1) << 13,
    TURBOWASM_WASI_RIGHT_FD_READDIR = UINT64_C(1) << 14,
    TURBOWASM_WASI_RIGHT_PATH_RENAME_SOURCE = UINT64_C(1) << 16,
    TURBOWASM_WASI_RIGHT_PATH_RENAME_TARGET = UINT64_C(1) << 17,
    TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET = UINT64_C(1) << 18,
    TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET = UINT64_C(1) << 21,
    TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY = UINT64_C(1) << 25,
    TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE = UINT64_C(1) << 26,
    TURBOWASM_WASI_RIGHT_POLL_FD_READWRITE = UINT64_C(1) << 27,
    TURBOWASM_WASI_RIGHT_SOCK_SHUTDOWN = UINT64_C(1) << 28,
    TURBOWASM_WASI_RIGHT_SOCK_ACCEPT = UINT64_C(1) << 29
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

/* Borrowed output valid only during this callback. On success fill all length
 * bytes. The adapter invokes the provider once per admitted random_get,
 * including zero length. Concurrent guests require a concurrent-safe provider.
 * Shared guest output is published only on success; unshared calls retain the
 * direct-buffer behavior, including any provider-written bytes on error. */
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
 * Shared-memory fd_read/fd_write snapshot vector descriptors and payloads;
 * their aggregate payload capacity is limited to 1 MiB per call. A larger
 * valid range returns NOMEM before invoking the provider. Successful reads
 * copy only the reported byte count back to the original guest ranges.
 * Unshared-memory vector calls retain their existing capacity behavior.
 * Shared path imports and random_get use the same 1 MiB per-call payload bound;
 * rename counts both paths together. All guest ranges are checked first: FAULT
 * takes precedence over NOMEM, before allocation or provider effects. Paths are
 * copied before callbacks; random output is copied only on success. Zero-length
 * random calls invoke the provider once without payload allocation. For example,
 * request multiple <= 1 MiB random_get calls for a larger shared output.
 * Unshared path/random calls keep their existing capacity and error behavior.
 * Fixed filesystem outputs (including v2 fd_fdstat_get) and preopen names also
 * use protected copies; failed provider calls leave those guest outputs
 * unchanged. Preview1 pins borrowed preopen names through the copy. Concurrent
 * filesystem close may return BUSY while a synchronous operation is admitted;
 * finish that operation before retrying close. Public descriptor-info getters
 * retain their separately documented borrowed-path lifetime contract.
 * Providers used by concurrently executing guests must support that concurrency.
 */
turbowasm_status turbowasm_wasi_preview1_init(
    turbowasm_wasi_preview1 *wasi,
    const turbowasm_wasi_preview1_config *config);

/* Legacy void destruction preserves the object if v2 waits are still active.
 * v2 callers use destroy_checked in wasi_sockets.h to observe that condition. */
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

/*
 * Canonical retained metadata for the supported wasi_snapshot_preview1
 * import surface. Descriptors are immutable process-lifetime storage and are
 * the same source used to derive linker carrier signatures.
 */
size_t turbowasm_wasi_preview1_function_count(void);

const cmeta_function_desc *turbowasm_wasi_preview1_function_at(
    size_t index);

const cmeta_function_desc *turbowasm_wasi_preview1_find_function(
    const char *name);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_H */
