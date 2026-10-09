#ifndef TURBOWASM_WASI_PROVIDER_PRIVATE_H
#define TURBOWASM_WASI_PROVIDER_PRIVATE_H
#include "wasi_fs_private.h"

typedef enum tw_wasi_provider_operation {
    TW_PROVIDER_CLOSE, TW_PROVIDER_READ, TW_PROVIDER_WRITE,
    TW_PROVIDER_SEEK, TW_PROVIDER_TELL, TW_PROVIDER_STAT,
    TW_PROVIDER_PATH_OPEN, TW_PROVIDER_PATH_STAT, TW_PROVIDER_PATH_MUTATE,
    TW_PROVIDER_READDIR, TW_PROVIDER_RENAME, TW_PROVIDER_FLAGS,
    TW_PROVIDER_RETAIN, TW_PROVIDER_CLEANUP, TW_PROVIDER_READY,
    TW_PROVIDER_ACCEPT, TW_PROVIDER_RECV, TW_PROVIDER_SEND, TW_PROVIDER_SHUTDOWN
} tw_wasi_provider_operation;

/* Typed borrowed argument frame, valid through owner acknowledgement. Native
 * provider context/identity stay unchanged, including rename and accept. */
typedef struct tw_wasi_provider_request {
    tw_wasi_provider_operation operation;
    const tw_wasi_fd_lease *lease;
    union {
        struct { const turbowasm_wasi_buffer *buffers; size_t count; uint32_t *out; } read;
        struct { const turbowasm_wasi_const_buffer *buffers; size_t count; uint32_t *out; } write;
        struct { int64_t offset; uint8_t whence; uint64_t *out; } seek;
        uint64_t *tell;
        turbowasm_wasi_fs_stat *stat;
        struct { uint32_t dirflags; const uint8_t *path; size_t length;
            uint32_t oflags; uint64_t base, inheriting; uint32_t flags;
            turbowasm_wasi_fs_file *out; } open;
        struct { uint32_t flags; const uint8_t *path; size_t length;
            turbowasm_wasi_fs_stat *out; } path_stat;
        struct { turbowasm_wasi_fs_path_mutation_fn fn; const uint8_t *path; size_t length; } mutate;
        struct { uint64_t cookie; turbowasm_wasi_fs_dirent *out; bool *has_entry; } readdir;
        struct { const uint8_t *source; size_t source_length; turbowasm_wasi_fs_file target_file;
            const uint8_t *target; size_t target_length; } rename;
        uint16_t flags;
        struct { uint8_t direction; turbowasm_wasi_readiness *out; } ready;
        turbowasm_wasi_fs_file *accept;
        struct { const turbowasm_wasi_buffer *buffers; size_t count; uint16_t flags;
            bool nonblock; uint32_t *out; uint16_t *out_flags; } recv;
        uint8_t shutdown;
    } as;
} tw_wasi_provider_request;

uint32_t tw_wasi_provider_call(const tw_wasi_fd_lease *, tw_wasi_provider_request *);
#endif
