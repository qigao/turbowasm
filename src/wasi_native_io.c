#include <turbowasm/wasi_native_io.h>

#include <salts/error_codes.h>

#include <stddef.h>
#include <stdlib.h>

typedef struct turbowasm_wasi_native_io_impl {
    turbowasm_native_io_bridge *bridge;
    turbowasm_wasi_native_io_resolve_fd_fn resolve_fd;
    void *resolve_context;
} turbowasm_wasi_native_io_impl;

static uint32_t turbowasm_wasi_native_io_status_errno(
    int status) {
    switch (status) {
        case SALTS_OK:
            return TURBOWASM_WASI_ERRNO_SUCCESS;
        case SALTS_ENOBUFS:
        case SALTS_EBUSY:
        case SALTS_EALREADY:
            return TURBOWASM_WASI_ERRNO_AGAIN;
        case SALTS_EINTR:
        case SALTS_ECANCELED:
            return TURBOWASM_WASI_ERRNO_INTR;
        case SALTS_EBADF:
        case SALTS_ENOENT:
            return TURBOWASM_WASI_ERRNO_BADF;
        case SALTS_EINVAL:
            return TURBOWASM_WASI_ERRNO_INVAL;
        case SALTS_ENOTSUP:
        case SALTS_ENOSYS:
            return TURBOWASM_WASI_ERRNO_NOSYS;
        default:
            return TURBOWASM_WASI_ERRNO_IO;
    }
}

static bool turbowasm_wasi_native_io_kind_valid(
    native_io_operation_kind kind,
    bool write) {
    if (write) {
        return kind == NATIVE_IO_OPERATION_PIPE_WRITE ||
               kind == NATIVE_IO_OPERATION_STREAM_SEND;
    }

    return kind == NATIVE_IO_OPERATION_PIPE_READ ||
           kind == NATIVE_IO_OPERATION_STREAM_RECV;
}

static uint32_t turbowasm_wasi_native_io_resolve(
    turbowasm_wasi_native_io_impl *impl,
    uint32_t fd,
    bool write,
    native_io_endpoint *out_endpoint,
    native_io_operation_kind *out_kind) {
    uint32_t error;

    if (impl == NULL || impl->resolve_fd == NULL ||
        out_endpoint == NULL || out_kind == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_endpoint = (native_io_endpoint){0};
    *out_kind = (native_io_operation_kind)0;

    error = impl->resolve_fd(
        impl->resolve_context,
        fd,
        write,
        out_endpoint,
        out_kind);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    if (!native_io_endpoint_valid(*out_endpoint) ||
        !turbowasm_wasi_native_io_kind_valid(
            *out_kind, write))
        return TURBOWASM_WASI_ERRNO_INVAL;

    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t turbowasm_wasi_native_io_completion_errno(
    const native_io_completion *completion,
    bool write,
    size_t capacity,
    uint32_t *out_transferred) {
    if (completion == NULL || out_transferred == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_transferred = 0u;

    switch (completion->kind) {
        case NATIVE_IO_COMPLETION_OK:
            if (completion->bytes > capacity ||
                completion->bytes > UINT32_MAX)
                return TURBOWASM_WASI_ERRNO_IO;
            *out_transferred = (uint32_t)completion->bytes;
            return TURBOWASM_WASI_ERRNO_SUCCESS;

        case NATIVE_IO_COMPLETION_EOF:
            return write
                ? TURBOWASM_WASI_ERRNO_IO
                : TURBOWASM_WASI_ERRNO_SUCCESS;

        case NATIVE_IO_COMPLETION_CANCELLED:
            return TURBOWASM_WASI_ERRNO_INTR;

        case NATIVE_IO_COMPLETION_FAILED:
            return completion->status == SALTS_OK
                ? TURBOWASM_WASI_ERRNO_IO
                : turbowasm_wasi_native_io_status_errno(
                    completion->status);

        default:
            return TURBOWASM_WASI_ERRNO_IO;
    }
}

static uint32_t turbowasm_wasi_native_io_read(
    void *context,
    turbowasm_host_call *call,
    uint32_t fd,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    turbowasm_wasi_native_io_impl *impl =
        (turbowasm_wasi_native_io_impl *)context;
    native_io_endpoint endpoint = {0};
    native_io_operation_kind kind =
        (native_io_operation_kind)0;
    native_io_operation operation = {0};
    native_io_completion completion = {0};
    size_t index;
    uint32_t error;
    int status;

    if (impl == NULL || call == NULL ||
        out_read == NULL ||
        (buffer_count != 0u && buffers == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_read = 0u;
    error = turbowasm_wasi_native_io_resolve(
        impl, fd, false, &endpoint, &kind);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    for (index = 0u; index < buffer_count; ++index) {
        if (buffers[index].size != 0u)
            break;
    }
    if (index == buffer_count)
        return TURBOWASM_WASI_ERRNO_SUCCESS;

    operation.kind = kind;
    operation.endpoint = endpoint;
    operation.buffer = buffers[index].data;
    operation.length = buffers[index].size;

    status = turbowasm_native_io_await(
        impl->bridge, call, &operation, &completion);
    if (status != SALTS_OK)
        return turbowasm_wasi_native_io_status_errno(status);

    /*
     * One scalar NativeIO operation per Preview1 call deliberately publishes a
     * legal short read instead of issuing a second operation that could block
     * after progress was already available.
     */
    return turbowasm_wasi_native_io_completion_errno(
        &completion, false, buffers[index].size, out_read);
}

static uint32_t turbowasm_wasi_native_io_write(
    void *context,
    turbowasm_host_call *call,
    uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    turbowasm_wasi_native_io_impl *impl =
        (turbowasm_wasi_native_io_impl *)context;
    native_io_endpoint endpoint = {0};
    native_io_operation_kind kind =
        (native_io_operation_kind)0;
    native_io_operation operation = {0};
    native_io_completion completion = {0};
    size_t index;
    uint32_t error;
    int status;

    if (impl == NULL || call == NULL ||
        out_written == NULL ||
        (buffer_count != 0u && buffers == NULL))
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_written = 0u;
    error = turbowasm_wasi_native_io_resolve(
        impl, fd, true, &endpoint, &kind);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    for (index = 0u; index < buffer_count; ++index) {
        if (buffers[index].size != 0u)
            break;
    }
    if (index == buffer_count)
        return TURBOWASM_WASI_ERRNO_SUCCESS;

    operation.kind = kind;
    operation.endpoint = endpoint;
    operation.buffer = (void *)buffers[index].data;
    operation.length = buffers[index].size;

    status = turbowasm_native_io_await(
        impl->bridge, call, &operation, &completion);
    if (status != SALTS_OK)
        return turbowasm_wasi_native_io_status_errno(status);

    /* As with read, returning after the first scalar operation is a legal
     * Preview1 short write and never introduces a second blocking point. */
    return turbowasm_wasi_native_io_completion_errno(
        &completion, true, buffers[index].size, out_written);
}

turbowasm_status turbowasm_wasi_native_io_init(
    turbowasm_wasi_native_io *provider,
    const turbowasm_wasi_native_io_config *config) {
    turbowasm_wasi_native_io_impl *impl;

    if (provider == NULL || provider->impl != NULL ||
        config == NULL || config->bridge == NULL ||
        config->bridge->impl == NULL ||
        config->resolve_fd == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_wasi_native_io_impl *)calloc(
        1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->bridge = config->bridge;
    impl->resolve_fd = config->resolve_fd;
    impl->resolve_context = config->resolve_context;
    provider->impl = impl;
    return TURBOWASM_OK;
}

void turbowasm_wasi_native_io_destroy(
    turbowasm_wasi_native_io *provider) {
    if (provider == NULL || provider->impl == NULL)
        return;

    free(provider->impl);
    provider->impl = NULL;
}

turbowasm_status turbowasm_wasi_native_io_apply(
    turbowasm_wasi_native_io *provider,
    turbowasm_wasi_preview1_config *wasi_config,
    bool enable_read,
    bool enable_write) {
    turbowasm_wasi_native_io_impl *impl;

    if (provider == NULL || provider->impl == NULL ||
        wasi_config == NULL ||
        (!enable_read && !enable_write))
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_wasi_native_io_impl *)provider->impl;

    if (enable_read &&
        (wasi_config->allow_fd_read ||
         wasi_config->fd_read != NULL ||
         wasi_config->fd_read_async != NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    if (enable_write &&
        (wasi_config->allow_fd_write ||
         wasi_config->fd_write != NULL ||
         wasi_config->fd_write_async != NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    if (enable_read) {
        wasi_config->allow_fd_read = true;
        wasi_config->fd_read_async =
            turbowasm_wasi_native_io_read;
        wasi_config->fd_read_context = impl;
    }

    if (enable_write) {
        wasi_config->allow_fd_write = true;
        wasi_config->fd_write_async =
            turbowasm_wasi_native_io_write;
        wasi_config->fd_write_context = impl;
    }

    return TURBOWASM_OK;
}
