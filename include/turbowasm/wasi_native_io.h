#ifndef TURBOWASM_WASI_NATIVE_IO_H
#define TURBOWASM_WASI_NATIVE_IO_H

#include <turbowasm/native_io.h>
#include <turbowasm/wasi.h>

#include <salts/native_io.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional Preview1 fd_read/fd_write provider backed by TurboWasm::NativeIO.
 *
 * The provider borrows a live NativeIO bridge. Descriptor policy remains with
 * the host through resolve_fd: TurboWasm never exposes or owns OS handles.
 */
typedef struct turbowasm_wasi_native_io {
    void *impl;
} turbowasm_wasi_native_io;

/*
 * Resolve one Preview1 fd to a live NativeIO endpoint and scalar operation
 * kind. Return a Preview1 errno. On SUCCESS:
 *
 *   read  -> PIPE_READ or STREAM_RECV
 *   write -> PIPE_WRITE or STREAM_SEND
 *
 * Datagram fd semantics are deliberately excluded here because one scalar
 * operation per WASI call cannot preserve datagram vectored-message semantics.
 */
typedef uint32_t (*turbowasm_wasi_native_io_resolve_fd_fn)(
    void *context,
    uint32_t fd,
    bool write,
    native_io_endpoint *out_endpoint,
    native_io_operation_kind *out_kind);

typedef struct turbowasm_wasi_native_io_config {
    turbowasm_native_io_bridge *bridge;
    turbowasm_wasi_native_io_resolve_fd_fn resolve_fd;
    void *resolve_context;
} turbowasm_wasi_native_io_config;

turbowasm_status turbowasm_wasi_native_io_init(
    turbowasm_wasi_native_io *provider,
    const turbowasm_wasi_native_io_config *config);

void turbowasm_wasi_native_io_destroy(
    turbowasm_wasi_native_io *provider);

/*
 * Install async fd callbacks into a Preview1 config.
 *
 * Existing fd providers are never overwritten. enable_read/enable_write select
 * the directions to install. At least one direction must be enabled.
 */
turbowasm_status turbowasm_wasi_native_io_apply(
    turbowasm_wasi_native_io *provider,
    turbowasm_wasi_preview1_config *wasi_config,
    bool enable_read,
    bool enable_write);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI_NATIVE_IO_H */
