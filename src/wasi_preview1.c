#include <turbowasm/wasi.h>
#include <turbowasm/wasi_fs.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbowasm_wasi_string_list {
    char **items;
    uint32_t count;
    uint32_t bytes;
} turbowasm_wasi_string_list;

typedef struct turbowasm_wasi_preview1_impl {
    bool allow_args;
    bool allow_environ;
    bool allow_clock;
    bool allow_random;
    bool allow_fd_write;
    bool allow_fd_read;
    bool allow_proc_exit;
    bool allow_filesystem;
    turbowasm_wasi_string_list args;
    turbowasm_wasi_string_list environment;
    turbowasm_wasi_clock_time_fn clock_time;
    void *clock_context;
    turbowasm_wasi_random_fill_fn random_fill;
    void *random_context;
    turbowasm_wasi_fd_write_fn fd_write;
    turbowasm_wasi_fd_write_async_fn fd_write_async;
    void *fd_write_context;
    turbowasm_wasi_fd_read_fn fd_read;
    turbowasm_wasi_fd_read_async_fn fd_read_async;
    void *fd_read_context;
    turbowasm_wasi_proc_exit_fn proc_exit;
    void *proc_exit_context;
    turbowasm_wasi_fs *filesystem;
} turbowasm_wasi_preview1_impl;

static const uint8_t turbowasm_wasi_namespace_bytes[] =
    "wasi_snapshot_preview1";

static turbowasm_name turbowasm_wasi_name(
    const char *text) {
    turbowasm_name name = {0};
    if (text == NULL)
        return name;
    name.bytes = (const uint8_t *)text;
    name.size = (uint32_t)strlen(text);
    return name;
}

static turbowasm_name turbowasm_wasi_namespace(void) {
    turbowasm_name name;
    name.bytes = turbowasm_wasi_namespace_bytes;
    name.size = (uint32_t)(
        sizeof(turbowasm_wasi_namespace_bytes) - 1u);
    return name;
}

static void turbowasm_wasi_string_list_destroy(
    turbowasm_wasi_string_list *list) {
    uint32_t index;

    if (list == NULL)
        return;
    for (index = 0u; index < list->count; ++index)
        free(list->items[index]);
    free(list->items);
    *list = (turbowasm_wasi_string_list){0};
}

static turbowasm_status turbowasm_wasi_string_list_copy(
    turbowasm_wasi_string_list *out,
    const char *const *items,
    size_t count) {
    char **copies = NULL;
    uint64_t bytes = 0u;
    size_t index;

    if (out == NULL ||
        count > UINT32_MAX / 4u ||
        (count != 0u && items == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    *out = (turbowasm_wasi_string_list){0};
    if (count == 0u)
        return TURBOWASM_OK;

    if (count > SIZE_MAX / sizeof(*copies))
        return TURBOWASM_OUT_OF_MEMORY;
    copies = (char **)calloc(count, sizeof(*copies));
    if (copies == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    for (index = 0u; index < count; ++index) {
        size_t length;

        if (items[index] == NULL) {
            while (index != 0u)
                free(copies[--index]);
            free(copies);
            return TURBOWASM_INVALID_ARGUMENT;
        }

        length = strlen(items[index]);
        if (length == SIZE_MAX ||
            bytes + (uint64_t)length + 1u > UINT32_MAX) {
            while (index != 0u)
                free(copies[--index]);
            free(copies);
            return TURBOWASM_OUT_OF_MEMORY;
        }

        copies[index] = (char *)malloc(length + 1u);
        if (copies[index] == NULL) {
            while (index != 0u)
                free(copies[--index]);
            free(copies);
            return TURBOWASM_OUT_OF_MEMORY;
        }
        memcpy(copies[index], items[index], length + 1u);
        bytes += (uint64_t)length + 1u;
    }

    out->items = copies;
    out->count = (uint32_t)count;
    out->bytes = (uint32_t)bytes;
    return TURBOWASM_OK;
}

static void turbowasm_wasi_store_u32(
    uint8_t *destination,
    uint32_t value) {
    destination[0] = (uint8_t)(value & UINT32_C(0xff));
    destination[1] = (uint8_t)((value >> 8u) & UINT32_C(0xff));
    destination[2] = (uint8_t)((value >> 16u) & UINT32_C(0xff));
    destination[3] = (uint8_t)((value >> 24u) & UINT32_C(0xff));
}

static uint32_t turbowasm_wasi_load_u32(
    const uint8_t *source) {
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) |
           ((uint32_t)source[3] << 24u);
}

static void turbowasm_wasi_store_u64(
    uint8_t *destination,
    uint64_t value) {
    uint32_t index;

    for (index = 0u; index < 8u; ++index)
        destination[index] =
            (uint8_t)((value >> (index * 8u)) & UINT64_C(0xff));
}

static turbowasm_status turbowasm_wasi_return_errno(
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    uint32_t wasi_errno) {
    if (results == NULL || result_capacity < 1u ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = (int32_t)wasi_errno;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static uint32_t turbowasm_wasi_memory_span(
    turbowasm_host_call *call,
    uint32_t address,
    size_t length,
    turbowasm_host_memory_span *out) {
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status =
        turbowasm_host_call_memory_span(
            call, 0u, address, length, out, &trap);

    if (status == TURBOWASM_OK)
        return TURBOWASM_WASI_ERRNO_SUCCESS;

    /*
     * Preview1 surfaces invalid guest output ranges as errno FAULT. Runtime
     * remains the sole bounds checker; the adapter only translates the already
     * validated failure into the WASI ABI.
     */
    return TURBOWASM_WASI_ERRNO_FAULT;
}

static turbowasm_status turbowasm_wasi_sizes_get(
    const turbowasm_wasi_string_list *list,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_host_memory_span count_span = {0};
    turbowasm_host_memory_span bytes_span = {0};
    uint32_t error;

    if (list == NULL || call == NULL ||
        arguments == NULL || argument_count != 2u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_memory_span(
        call, (uint32_t)arguments[0].as.i32,
        4u, &count_span);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_memory_span(
            call, (uint32_t)arguments[1].as.i32,
            4u, &bytes_span);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        turbowasm_wasi_store_u32(
            count_span.data, list->count);
        turbowasm_wasi_store_u32(
            bytes_span.data, list->bytes);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count,
        trap, error);
}

static turbowasm_status turbowasm_wasi_list_get(
    const turbowasm_wasi_string_list *list,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_host_memory_span table = {0};
    turbowasm_host_memory_span buffer = {0};
    uint32_t table_address;
    uint32_t buffer_address;
    uint32_t error;
    uint32_t cursor = 0u;
    uint32_t index;
    size_t table_size;

    if (list == NULL || call == NULL ||
        arguments == NULL || argument_count != 2u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    table_address = (uint32_t)arguments[0].as.i32;
    buffer_address = (uint32_t)arguments[1].as.i32;
    table_size = (size_t)list->count * 4u;

    error = turbowasm_wasi_memory_span(
        call, table_address, table_size, &table);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_memory_span(
            call, buffer_address, list->bytes, &buffer);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        for (index = 0u; index < list->count; ++index) {
            size_t length = strlen(list->items[index]) + 1u;
            uint32_t pointer;

            if (cursor > UINT32_MAX - buffer_address) {
                error = TURBOWASM_WASI_ERRNO_FAULT;
                break;
            }

            pointer = buffer_address + cursor;
            turbowasm_wasi_store_u32(
                table.data + (size_t)index * 4u,
                pointer);
            memcpy(buffer.data + cursor,
                   list->items[index], length);
            cursor += (uint32_t)length;
        }
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count,
        trap, error);
}

static turbowasm_status turbowasm_wasi_args_sizes_get(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;

    if (impl == NULL || !impl->allow_args)
        return TURBOWASM_UNSUPPORTED;
    return turbowasm_wasi_sizes_get(
        &impl->args, call, arguments, argument_count,
        results, result_capacity, result_count, trap);
}

static turbowasm_status turbowasm_wasi_args_get(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;

    if (impl == NULL || !impl->allow_args)
        return TURBOWASM_UNSUPPORTED;
    return turbowasm_wasi_list_get(
        &impl->args, call, arguments, argument_count,
        results, result_capacity, result_count, trap);
}

static turbowasm_status turbowasm_wasi_environ_sizes_get(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;

    if (impl == NULL || !impl->allow_environ)
        return TURBOWASM_UNSUPPORTED;
    return turbowasm_wasi_sizes_get(
        &impl->environment, call, arguments, argument_count,
        results, result_capacity, result_count, trap);
}

static turbowasm_status turbowasm_wasi_environ_get(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;

    if (impl == NULL || !impl->allow_environ)
        return TURBOWASM_UNSUPPORTED;
    return turbowasm_wasi_list_get(
        &impl->environment, call, arguments, argument_count,
        results, result_capacity, result_count, trap);
}

static turbowasm_status turbowasm_wasi_clock_time_get(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_host_memory_span output = {0};
    uint64_t timestamp = 0u;
    uint32_t error;

    if (impl == NULL || !impl->allow_clock ||
        impl->clock_time == NULL ||
        call == NULL || arguments == NULL ||
        argument_count != 3u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I64 ||
        arguments[2].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_memory_span(
        call, (uint32_t)arguments[2].as.i32,
        8u, &output);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = impl->clock_time(
            impl->clock_context,
            (uint32_t)arguments[0].as.i32,
            (uint64_t)arguments[1].as.i64,
            &timestamp);
        if (error == TURBOWASM_WASI_ERRNO_SUCCESS)
            turbowasm_wasi_store_u64(output.data, timestamp);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_random_get(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_host_memory_span buffer = {0};
    uint32_t error;

    if (impl == NULL || !impl->allow_random ||
        impl->random_fill == NULL ||
        call == NULL || arguments == NULL ||
        argument_count != 2u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_memory_span(
        call,
        (uint32_t)arguments[0].as.i32,
        (uint32_t)arguments[1].as.i32,
        &buffer);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = impl->random_fill(
            impl->random_context,
            buffer.data,
            buffer.size);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static uint32_t turbowasm_wasi_collect_iovecs(
    turbowasm_host_call *call,
    uint32_t table_address,
    uint32_t iov_count,
    bool writable,
    turbowasm_wasi_const_buffer *const_buffers,
    turbowasm_wasi_buffer *mutable_buffers,
    uint64_t *out_capacity) {
    turbowasm_host_memory_span table = {0};
    uint64_t capacity = 0u;
    uint32_t index;
    uint32_t error;

    if (call == NULL || out_capacity == NULL ||
        (iov_count != 0u &&
         ((writable && mutable_buffers == NULL) ||
          (!writable && const_buffers == NULL))))
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_capacity = 0u;
    if (iov_count > TURBOWASM_WASI_IOV_MAX)
        return TURBOWASM_WASI_ERRNO_INVAL;

    error = turbowasm_wasi_memory_span(
        call, table_address, (size_t)iov_count * 8u, &table);
    if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
        return error;

    for (index = 0u; index < iov_count; ++index) {
        const uint8_t *entry =
            table.data + (size_t)index * 8u;
        uint32_t address = turbowasm_wasi_load_u32(entry);
        uint32_t length = turbowasm_wasi_load_u32(entry + 4u);
        turbowasm_host_memory_span span = {0};

        error = turbowasm_wasi_memory_span(
            call, address, length, &span);
        if (error != TURBOWASM_WASI_ERRNO_SUCCESS)
            return error;

        capacity += length;
        if (writable) {
            mutable_buffers[index].data = span.data;
            mutable_buffers[index].size = span.size;
        } else {
            const_buffers[index].data = span.data;
            const_buffers[index].size = span.size;
        }
    }

    *out_capacity = capacity;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_status turbowasm_wasi_fd_write(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_wasi_const_buffer buffers[TURBOWASM_WASI_IOV_MAX] = {{0}};
    turbowasm_host_memory_span written_span = {0};
    uint64_t capacity = 0u;
    uint32_t written = 0u;
    uint32_t error;

    if (impl == NULL || !impl->allow_fd_write ||
        (impl->fd_write == NULL &&
         impl->fd_write_async == NULL) ||
        call == NULL || arguments == NULL ||
        argument_count != 4u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32 ||
        arguments[2].kind != TURBOWASM_VALUE_I32 ||
        arguments[3].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_memory_span(
        call, (uint32_t)arguments[3].as.i32,
        4u, &written_span);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_collect_iovecs(
            call,
            (uint32_t)arguments[1].as.i32,
            (uint32_t)arguments[2].as.i32,
            false,
            buffers,
            NULL,
            &capacity);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        if (impl->fd_write_async != NULL) {
            error = impl->fd_write_async(
                impl->fd_write_context,
                call,
                (uint32_t)arguments[0].as.i32,
                buffers,
                (uint32_t)arguments[2].as.i32,
                &written);
        } else {
            error = impl->fd_write(
                impl->fd_write_context,
                (uint32_t)arguments[0].as.i32,
                buffers,
                (uint32_t)arguments[2].as.i32,
                &written);
        }
        if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
            if ((uint64_t)written > capacity)
                error = TURBOWASM_WASI_ERRNO_IO;
            else
                turbowasm_wasi_store_u32(
                    written_span.data, written);
        }
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_fd_read(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_wasi_buffer buffers[TURBOWASM_WASI_IOV_MAX] = {{0}};
    turbowasm_host_memory_span read_span = {0};
    uint64_t capacity = 0u;
    uint32_t read_count = 0u;
    uint32_t error;

    if (impl == NULL || !impl->allow_fd_read ||
        (impl->fd_read == NULL &&
         impl->fd_read_async == NULL) ||
        call == NULL || arguments == NULL ||
        argument_count != 4u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32 ||
        arguments[2].kind != TURBOWASM_VALUE_I32 ||
        arguments[3].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_memory_span(
        call, (uint32_t)arguments[3].as.i32,
        4u, &read_span);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_collect_iovecs(
            call,
            (uint32_t)arguments[1].as.i32,
            (uint32_t)arguments[2].as.i32,
            true,
            NULL,
            buffers,
            &capacity);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        if (impl->fd_read_async != NULL) {
            error = impl->fd_read_async(
                impl->fd_read_context,
                call,
                (uint32_t)arguments[0].as.i32,
                buffers,
                (uint32_t)arguments[2].as.i32,
                &read_count);
        } else {
            error = impl->fd_read(
                impl->fd_read_context,
                (uint32_t)arguments[0].as.i32,
                buffers,
                (uint32_t)arguments[2].as.i32,
                &read_count);
        }
        if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
            if ((uint64_t)read_count > capacity)
                error = TURBOWASM_WASI_ERRNO_IO;
            else
                turbowasm_wasi_store_u32(
                    read_span.data, read_count);
        }
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static uint32_t turbowasm_wasi_fs_descriptor_errno(
    turbowasm_wasi_fs *filesystem,
    uint32_t fd,
    turbowasm_wasi_fs_descriptor_info *out_info) {
    if (filesystem == NULL || out_info == NULL)
        return TURBOWASM_WASI_ERRNO_INVAL;
    if (!turbowasm_wasi_fs_descriptor_info_get(
            filesystem, fd, out_info))
        return TURBOWASM_WASI_ERRNO_BADF;
    if (!out_info->preopen)
        return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_status turbowasm_wasi_fd_seek(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_host_memory_span output = {0};
    uint64_t new_offset = 0u;
    uint32_t error;

    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL || call == NULL ||
        arguments == NULL || argument_count != 4u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I64 ||
        arguments[2].kind != TURBOWASM_VALUE_I32 ||
        arguments[3].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    if (arguments[2].as.i32 < TURBOWASM_WASI_WHENCE_SET ||
        arguments[2].as.i32 > TURBOWASM_WASI_WHENCE_END) {
        error = TURBOWASM_WASI_ERRNO_INVAL;
    } else {
        error = turbowasm_wasi_memory_span(
            call,
            (uint32_t)arguments[3].as.i32,
            8u,
            &output);
    }
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_fs_fd_seek(
            impl->filesystem,
            (uint32_t)arguments[0].as.i32,
            arguments[1].as.i64,
            (uint8_t)arguments[2].as.i32,
            &new_offset);
        if (error == TURBOWASM_WASI_ERRNO_SUCCESS)
            turbowasm_wasi_store_u64(
                output.data, new_offset);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_fd_tell(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_host_memory_span output = {0};
    uint64_t offset = 0u;
    uint32_t error;

    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL || call == NULL ||
        arguments == NULL || argument_count != 2u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_memory_span(
        call,
        (uint32_t)arguments[1].as.i32,
        8u,
        &output);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_fs_fd_tell(
            impl->filesystem,
            (uint32_t)arguments[0].as.i32,
            &offset);
        if (error == TURBOWASM_WASI_ERRNO_SUCCESS)
            turbowasm_wasi_store_u64(
                output.data, offset);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_fd_filestat_get(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_host_memory_span output = {0};
    turbowasm_wasi_fs_stat stat = {0};
    uint32_t error;

    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL || call == NULL ||
        arguments == NULL || argument_count != 2u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_memory_span(
        call,
        (uint32_t)arguments[1].as.i32,
        64u,
        &output);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_fs_fd_stat(
            impl->filesystem,
            (uint32_t)arguments[0].as.i32,
            &stat);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        memset(output.data, 0, 64u);
        turbowasm_wasi_store_u64(output.data + 0u, stat.device);
        turbowasm_wasi_store_u64(output.data + 8u, stat.inode);
        output.data[16u] = stat.file_type;
        turbowasm_wasi_store_u64(
            output.data + 24u, stat.link_count);
        turbowasm_wasi_store_u64(
            output.data + 32u, stat.size);
        turbowasm_wasi_store_u64(
            output.data + 40u, stat.accessed_ns);
        turbowasm_wasi_store_u64(
            output.data + 48u, stat.modified_ns);
        turbowasm_wasi_store_u64(
            output.data + 56u, stat.changed_ns);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_fd_readdir(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_host_memory_span buffer = {0};
    turbowasm_host_memory_span bufused = {0};
    uint64_t cookie;
    size_t used = 0u;
    uint32_t error;

    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL || call == NULL ||
        arguments == NULL || argument_count != 5u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32 ||
        arguments[2].kind != TURBOWASM_VALUE_I32 ||
        arguments[3].kind != TURBOWASM_VALUE_I64 ||
        arguments[4].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_memory_span(
        call,
        (uint32_t)arguments[1].as.i32,
        (uint32_t)arguments[2].as.i32,
        &buffer);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_memory_span(
            call,
            (uint32_t)arguments[4].as.i32,
            4u,
            &bufused);
    }

    cookie = (uint64_t)arguments[3].as.i64;
    while (error == TURBOWASM_WASI_ERRNO_SUCCESS &&
           used < buffer.size) {
        turbowasm_wasi_fs_dirent entry = {0};
        bool has_entry = false;
        uint8_t record[24u + TURBOWASM_WASI_FS_DIRENT_NAME_MAX];
        size_t record_size;
        size_t remaining;
        size_t copy_size;

        error = turbowasm_wasi_fs_fd_readdir(
            impl->filesystem,
            (uint32_t)arguments[0].as.i32,
            cookie,
            &entry,
            &has_entry);
        if (error != TURBOWASM_WASI_ERRNO_SUCCESS ||
            !has_entry)
            break;

        memset(record, 0, 24u);
        turbowasm_wasi_store_u64(record + 0u, entry.next_cookie);
        turbowasm_wasi_store_u64(record + 8u, entry.inode);
        turbowasm_wasi_store_u32(record + 16u, entry.name_length);
        record[20u] = entry.file_type;
        if (entry.name_length != 0u) {
            memcpy(record + 24u, entry.name, entry.name_length);
        }

        record_size = 24u + (size_t)entry.name_length;
        remaining = buffer.size - used;
        copy_size = record_size < remaining ? record_size : remaining;
        memcpy(buffer.data + used, record, copy_size);
        used += copy_size;
        if (copy_size != record_size)
            break;
        cookie = entry.next_cookie;
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        turbowasm_wasi_store_u32(bufused.data, (uint32_t)used);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_fd_close(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    uint32_t error;

    (void)call;
    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL ||
        arguments == NULL || argument_count != 1u ||
        arguments[0].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_fs_close_fd(
        impl->filesystem,
        (uint32_t)arguments[0].as.i32);
    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_fd_prestat_get(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_wasi_fs_descriptor_info info = {0};
    turbowasm_host_memory_span output = {0};
    uint32_t error;
    size_t name_length = 0u;

    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL || call == NULL ||
        arguments == NULL || argument_count != 2u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_fs_descriptor_errno(
        impl->filesystem,
        (uint32_t)arguments[0].as.i32,
        &info);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        name_length = strlen(info.guest_path);
        if (name_length > UINT32_MAX) {
            error = TURBOWASM_WASI_ERRNO_INVAL;
        } else {
            error = turbowasm_wasi_memory_span(
                call,
                (uint32_t)arguments[1].as.i32,
                8u,
                &output);
        }
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        memset(output.data, 0, 8u);
        output.data[0] = 0u;
        turbowasm_wasi_store_u32(
            output.data + 4u,
            (uint32_t)name_length);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_fd_prestat_dir_name(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_wasi_fs_descriptor_info info = {0};
    turbowasm_host_memory_span output = {0};
    uint32_t path_length;
    uint32_t error;
    size_t name_length = 0u;

    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL || call == NULL ||
        arguments == NULL || argument_count != 3u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32 ||
        arguments[2].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_fs_descriptor_errno(
        impl->filesystem,
        (uint32_t)arguments[0].as.i32,
        &info);
    path_length = (uint32_t)arguments[2].as.i32;

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        name_length = strlen(info.guest_path);
        if (name_length > path_length) {
            error = TURBOWASM_WASI_ERRNO_NAMETOOLONG;
        } else {
            error = turbowasm_wasi_memory_span(
                call,
                (uint32_t)arguments[1].as.i32,
                path_length,
                &output);
        }
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS &&
        name_length != 0u) {
        memcpy(output.data, info.guest_path, name_length);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

typedef uint32_t (*turbowasm_wasi_path_mutation_call_fn)(
    turbowasm_wasi_fs *filesystem,
    uint32_t directory_fd,
    const uint8_t *path,
    size_t path_length);

static turbowasm_status turbowasm_wasi_path_mutation(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    turbowasm_wasi_path_mutation_call_fn mutation) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_host_memory_span path = {0};
    uint32_t path_length;
    uint32_t error;

    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL || call == NULL ||
        arguments == NULL || argument_count != 3u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32 ||
        arguments[2].kind != TURBOWASM_VALUE_I32 ||
        mutation == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    path_length = (uint32_t)arguments[2].as.i32;
    error = turbowasm_wasi_memory_span(
        call,
        (uint32_t)arguments[1].as.i32,
        path_length,
        &path);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = mutation(
            impl->filesystem,
            (uint32_t)arguments[0].as.i32,
            path.data,
            path.size);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_path_create_directory(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    return turbowasm_wasi_path_mutation(
        context, call, arguments, argument_count,
        results, result_capacity, result_count, trap,
        turbowasm_wasi_fs_path_create_directory);
}

static turbowasm_status turbowasm_wasi_path_remove_directory(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    return turbowasm_wasi_path_mutation(
        context, call, arguments, argument_count,
        results, result_capacity, result_count, trap,
        turbowasm_wasi_fs_path_remove_directory);
}

static turbowasm_status turbowasm_wasi_path_unlink_file(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    return turbowasm_wasi_path_mutation(
        context, call, arguments, argument_count,
        results, result_capacity, result_count, trap,
        turbowasm_wasi_fs_path_unlink_file);
}

static turbowasm_status turbowasm_wasi_path_filestat_get(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_host_memory_span path = {0};
    turbowasm_host_memory_span output = {0};
    turbowasm_wasi_fs_stat stat = {0};
    uint32_t path_length;
    uint32_t error;

    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL || call == NULL ||
        arguments == NULL || argument_count != 5u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32 ||
        arguments[2].kind != TURBOWASM_VALUE_I32 ||
        arguments[3].kind != TURBOWASM_VALUE_I32 ||
        arguments[4].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    path_length = (uint32_t)arguments[3].as.i32;
    error = turbowasm_wasi_memory_span(
        call,
        (uint32_t)arguments[2].as.i32,
        path_length,
        &path);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_memory_span(
            call,
            (uint32_t)arguments[4].as.i32,
            64u,
            &output);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_fs_path_stat(
            impl->filesystem,
            (uint32_t)arguments[0].as.i32,
            (uint32_t)arguments[1].as.i32,
            path.data,
            path.size,
            &stat);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        memset(output.data, 0, 64u);
        turbowasm_wasi_store_u64(output.data + 0u, stat.device);
        turbowasm_wasi_store_u64(output.data + 8u, stat.inode);
        output.data[16u] = stat.file_type;
        turbowasm_wasi_store_u64(
            output.data + 24u, stat.link_count);
        turbowasm_wasi_store_u64(
            output.data + 32u, stat.size);
        turbowasm_wasi_store_u64(
            output.data + 40u, stat.accessed_ns);
        turbowasm_wasi_store_u64(
            output.data + 48u, stat.modified_ns);
        turbowasm_wasi_store_u64(
            output.data + 56u, stat.changed_ns);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_path_open(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_host_memory_span path = {0};
    turbowasm_host_memory_span opened_fd = {0};
    uint32_t path_length;
    uint32_t error;
    uint32_t guest_fd = 0u;

    if (impl == NULL || !impl->allow_filesystem ||
        impl->filesystem == NULL || call == NULL ||
        arguments == NULL || argument_count != 9u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32 ||
        arguments[2].kind != TURBOWASM_VALUE_I32 ||
        arguments[3].kind != TURBOWASM_VALUE_I32 ||
        arguments[4].kind != TURBOWASM_VALUE_I32 ||
        arguments[5].kind != TURBOWASM_VALUE_I64 ||
        arguments[6].kind != TURBOWASM_VALUE_I64 ||
        arguments[7].kind != TURBOWASM_VALUE_I32 ||
        arguments[8].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    path_length = (uint32_t)arguments[3].as.i32;
    error = turbowasm_wasi_memory_span(
        call,
        (uint32_t)arguments[2].as.i32,
        path_length,
        &path);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_memory_span(
            call,
            (uint32_t)arguments[8].as.i32,
            4u,
            &opened_fd);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_fs_path_open(
            impl->filesystem,
            (uint32_t)arguments[0].as.i32,
            (uint32_t)arguments[1].as.i32,
            path.data,
            path.size,
            (uint32_t)arguments[4].as.i32,
            (uint64_t)arguments[5].as.i64,
            (uint64_t)arguments[6].as.i64,
            (uint32_t)arguments[7].as.i32,
            &guest_fd);
        if (error == TURBOWASM_WASI_ERRNO_SUCCESS)
            turbowasm_wasi_store_u32(
                opened_fd.data, guest_fd);
    }

    return turbowasm_wasi_return_errno(
        results, result_capacity, result_count, trap, error);
}

static turbowasm_status turbowasm_wasi_proc_exit(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl =
        (turbowasm_wasi_preview1_impl *)context;
    turbowasm_instance *caller;

    (void)results;
    (void)result_capacity;

    if (impl == NULL || !impl->allow_proc_exit ||
        impl->proc_exit == NULL || call == NULL ||
        arguments == NULL || argument_count != 1u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    caller = turbowasm_host_call_instance(call);
    if (caller == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    impl->proc_exit(
        impl->proc_exit_context,
        caller,
        (uint32_t)arguments[0].as.i32);

    *result_count = 0u;
    *trap = TURBOWASM_TRAP_NONE;

    /*
     * Preview1 proc_exit never returns to guest code. INTERRUPTED is the
     * existing backend-neutral policy status used to unwind the current Wasm
     * invocation; the callback owns process/thread-group termination policy.
     */
    return TURBOWASM_INTERRUPTED;
}

turbowasm_status turbowasm_wasi_preview1_init(
    turbowasm_wasi_preview1 *wasi,
    const turbowasm_wasi_preview1_config *config) {
    turbowasm_wasi_preview1_impl *impl;
    turbowasm_status status;

    if (wasi == NULL || wasi->impl != NULL || config == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_wasi_preview1_impl *)calloc(
        1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    if ((config->allow_clock && config->clock_time == NULL) ||
        (config->allow_random && config->random_fill == NULL) ||
        (config->allow_fd_write &&
         ((config->fd_write == NULL) ==
          (config->fd_write_async == NULL))) ||
        (config->allow_fd_read &&
         ((config->fd_read == NULL) ==
          (config->fd_read_async == NULL))) ||
        (config->allow_proc_exit && config->proc_exit == NULL) ||
        (config->allow_filesystem && config->filesystem == NULL)) {
        free(impl);
        return TURBOWASM_INVALID_ARGUMENT;
    }

    impl->allow_args = config->allow_args;
    impl->allow_environ = config->allow_environ;
    impl->allow_clock = config->allow_clock;
    impl->allow_random = config->allow_random;
    impl->allow_fd_write = config->allow_fd_write;
    impl->allow_fd_read = config->allow_fd_read;
    impl->allow_proc_exit = config->allow_proc_exit;
    impl->allow_filesystem = config->allow_filesystem;
    impl->clock_time = config->clock_time;
    impl->clock_context = config->clock_context;
    impl->random_fill = config->random_fill;
    impl->random_context = config->random_context;
    impl->fd_write = config->fd_write;
    impl->fd_write_async = config->fd_write_async;
    impl->fd_write_context = config->fd_write_context;
    impl->fd_read = config->fd_read;
    impl->fd_read_async = config->fd_read_async;
    impl->fd_read_context = config->fd_read_context;
    impl->proc_exit = config->proc_exit;
    impl->proc_exit_context = config->proc_exit_context;
    impl->filesystem = config->filesystem;

    status = turbowasm_wasi_string_list_copy(
        &impl->args, config->args, config->arg_count);
    if (status != TURBOWASM_OK)
        goto fail;

    status = turbowasm_wasi_string_list_copy(
        &impl->environment,
        config->environment,
        config->environment_count);
    if (status != TURBOWASM_OK)
        goto fail;

    wasi->impl = impl;
    return TURBOWASM_OK;

fail:
    turbowasm_wasi_string_list_destroy(&impl->environment);
    turbowasm_wasi_string_list_destroy(&impl->args);
    free(impl);
    return status;
}

void turbowasm_wasi_preview1_destroy(
    turbowasm_wasi_preview1 *wasi) {
    turbowasm_wasi_preview1_impl *impl;

    if (wasi == NULL || wasi->impl == NULL)
        return;

    impl = (turbowasm_wasi_preview1_impl *)wasi->impl;
    turbowasm_wasi_string_list_destroy(&impl->environment);
    turbowasm_wasi_string_list_destroy(&impl->args);
    free(impl);
    wasi->impl = NULL;
}

static turbowasm_status turbowasm_wasi_define_function(
    turbowasm_linker *linker,
    turbowasm_name name,
    turbowasm_host_function_fn function,
    void *context) {
    static const turbowasm_value_kind params[] = {
        TURBOWASM_VALUE_I32,
        TURBOWASM_VALUE_I32
    };
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type type = {
        params, 2u, results, 1u
    };

    return turbowasm_linker_define_host_function(
        linker,
        turbowasm_wasi_namespace(),
        name,
        &type,
        function,
        context);
}

turbowasm_status turbowasm_wasi_preview1_define(
    turbowasm_wasi_preview1 *wasi,
    turbowasm_linker *linker) {
    turbowasm_wasi_preview1_impl *impl;
    turbowasm_status status;

    if (wasi == NULL || wasi->impl == NULL ||
        linker == NULL || linker->impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_wasi_preview1_impl *)wasi->impl;

    if (impl->allow_args) {
        status = turbowasm_wasi_define_function(
            linker,
            turbowasm_wasi_name("args_sizes_get"),
            turbowasm_wasi_args_sizes_get,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_wasi_define_function(
            linker,
            turbowasm_wasi_name("args_get"),
            turbowasm_wasi_args_get,
            impl);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (impl->allow_environ) {
        status = turbowasm_wasi_define_function(
            linker,
            turbowasm_wasi_name("environ_sizes_get"),
            turbowasm_wasi_environ_sizes_get,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_wasi_define_function(
            linker,
            turbowasm_wasi_name("environ_get"),
            turbowasm_wasi_environ_get,
            impl);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (impl->allow_clock) {
        static const turbowasm_value_kind clock_params[] = {
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I64,
            TURBOWASM_VALUE_I32
        };
        static const turbowasm_value_kind result_type[] = {
            TURBOWASM_VALUE_I32
        };
        const turbowasm_host_function_type clock_type = {
            clock_params, 3u, result_type, 1u
        };

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("clock_time_get"),
            &clock_type,
            turbowasm_wasi_clock_time_get,
            impl);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (impl->allow_random) {
        status = turbowasm_wasi_define_function(
            linker,
            turbowasm_wasi_name("random_get"),
            turbowasm_wasi_random_get,
            impl);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (impl->allow_fd_write || impl->allow_fd_read) {
        static const turbowasm_value_kind fd_params[] = {
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I32
        };
        static const turbowasm_value_kind result_type[] = {
            TURBOWASM_VALUE_I32
        };
        const turbowasm_host_function_type fd_type = {
            fd_params, 4u, result_type, 1u
        };

        if (impl->allow_fd_write) {
            status = turbowasm_linker_define_host_function(
                linker,
                turbowasm_wasi_namespace(),
                turbowasm_wasi_name("fd_write"),
                &fd_type,
                turbowasm_wasi_fd_write,
                impl);
            if (status != TURBOWASM_OK)
                return status;
        }

        if (impl->allow_fd_read) {
            status = turbowasm_linker_define_host_function(
                linker,
                turbowasm_wasi_namespace(),
                turbowasm_wasi_name("fd_read"),
                &fd_type,
                turbowasm_wasi_fd_read,
                impl);
            if (status != TURBOWASM_OK)
                return status;
        }
    }

    if (impl->allow_proc_exit) {
        static const turbowasm_value_kind proc_exit_params[] = {
            TURBOWASM_VALUE_I32
        };
        const turbowasm_host_function_type proc_exit_type = {
            proc_exit_params, 1u, NULL, 0u
        };

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("proc_exit"),
            &proc_exit_type,
            turbowasm_wasi_proc_exit,
            impl);
        if (status != TURBOWASM_OK)
            return status;
    }

    if (impl->allow_filesystem) {
        static const turbowasm_value_kind one_i32[] = {
            TURBOWASM_VALUE_I32
        };
        static const turbowasm_value_kind two_i32[] = {
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I32
        };
        static const turbowasm_value_kind three_i32[] = {
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I32
        };
        static const turbowasm_value_kind result_type[] = {
            TURBOWASM_VALUE_I32
        };
        const turbowasm_host_function_type close_type = {
            one_i32, 1u, result_type, 1u
        };
        const turbowasm_host_function_type prestat_type = {
            two_i32, 2u, result_type, 1u
        };
        const turbowasm_host_function_type dirname_type = {
            three_i32, 3u, result_type, 1u
        };
        static const turbowasm_value_kind seek_params[] = {
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I64,
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I32
        };
        const turbowasm_host_function_type seek_type = {
            seek_params, 4u, result_type, 1u
        };
        static const turbowasm_value_kind readdir_params[] = {
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I32,
            TURBOWASM_VALUE_I64,
            TURBOWASM_VALUE_I32
        };
        const turbowasm_host_function_type readdir_type = {
            readdir_params, 5u, result_type, 1u
        };

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("fd_close"),
            &close_type,
            turbowasm_wasi_fd_close,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("fd_prestat_get"),
            &prestat_type,
            turbowasm_wasi_fd_prestat_get,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("fd_prestat_dir_name"),
            &dirname_type,
            turbowasm_wasi_fd_prestat_dir_name,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("path_create_directory"),
            &dirname_type,
            turbowasm_wasi_path_create_directory,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("path_remove_directory"),
            &dirname_type,
            turbowasm_wasi_path_remove_directory,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("path_unlink_file"),
            &dirname_type,
            turbowasm_wasi_path_unlink_file,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("fd_seek"),
            &seek_type,
            turbowasm_wasi_fd_seek,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("fd_readdir"),
            &readdir_type,
            turbowasm_wasi_fd_readdir,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("fd_tell"),
            &prestat_type,
            turbowasm_wasi_fd_tell,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        status = turbowasm_linker_define_host_function(
            linker,
            turbowasm_wasi_namespace(),
            turbowasm_wasi_name("fd_filestat_get"),
            &prestat_type,
            turbowasm_wasi_fd_filestat_get,
            impl);
        if (status != TURBOWASM_OK)
            return status;

        {
            static const turbowasm_value_kind path_filestat_get_params[] = {
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I32
            };
            const turbowasm_host_function_type path_filestat_get_type = {
                path_filestat_get_params, 5u, result_type, 1u
            };

            status = turbowasm_linker_define_host_function(
                linker,
                turbowasm_wasi_namespace(),
                turbowasm_wasi_name("path_filestat_get"),
                &path_filestat_get_type,
                turbowasm_wasi_path_filestat_get,
                impl);
            if (status != TURBOWASM_OK)
                return status;
        }

        {
            static const turbowasm_value_kind path_open_params[] = {
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I64,
                TURBOWASM_VALUE_I64,
                TURBOWASM_VALUE_I32,
                TURBOWASM_VALUE_I32
            };
            const turbowasm_host_function_type path_open_type = {
                path_open_params, 9u, result_type, 1u
            };

            status = turbowasm_linker_define_host_function(
                linker,
                turbowasm_wasi_namespace(),
                turbowasm_wasi_name("path_open"),
                &path_open_type,
                turbowasm_wasi_path_open,
                impl);
            if (status != TURBOWASM_OK)
                return status;
        }
    }

    return TURBOWASM_OK;
}
