#include <turbowasm/wasi.h>

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
    turbowasm_wasi_string_list args;
    turbowasm_wasi_string_list environment;
    turbowasm_wasi_clock_time_fn clock_time;
    void *clock_context;
    turbowasm_wasi_random_fill_fn random_fill;
    void *random_context;
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
        (config->allow_random && config->random_fill == NULL)) {
        free(impl);
        return TURBOWASM_INVALID_ARGUMENT;
    }

    impl->allow_args = config->allow_args;
    impl->allow_environ = config->allow_environ;
    impl->allow_clock = config->allow_clock;
    impl->allow_random = config->allow_random;
    impl->clock_time = config->clock_time;
    impl->clock_context = config->clock_context;
    impl->random_fill = config->random_fill;
    impl->random_context = config->random_context;

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

    return TURBOWASM_OK;
}
