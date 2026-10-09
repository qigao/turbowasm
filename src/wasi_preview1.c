#include <turbowasm/wasi.h>
#include "wasi_fs_private.h"

#include "wasi_preview1_adapter_plan.h"

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
    struct tw_p1_async *async;
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


#define TURBOWASM_WASI_META_PARAM32(name_) \
    { sizeof(cmeta_param_desc), #name_, &cmeta_type_uint32, CMETA_PARAM_IN }
#define TURBOWASM_WASI_META_PARAM64(name_) \
    { sizeof(cmeta_param_desc), #name_, &cmeta_type_uint64, CMETA_PARAM_IN }
#define TURBOWASM_WASI_META_FUNCTION(name_, return_, params_, effects_) \
    { sizeof(cmeta_function_desc), name_, return_, params_, \
      sizeof(params_) / sizeof((params_)[0]), effects_, CMETA_PROP_NONE }

static const cmeta_param_desc turbowasm_wasi_meta_args_sizes_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(argc),
    TURBOWASM_WASI_META_PARAM32(argv_buf_size)
};
static const cmeta_param_desc turbowasm_wasi_meta_args_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(argv),
    TURBOWASM_WASI_META_PARAM32(argv_buf)
};
static const cmeta_param_desc turbowasm_wasi_meta_environ_sizes_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(environ_count),
    TURBOWASM_WASI_META_PARAM32(environ_buf_size)
};
static const cmeta_param_desc turbowasm_wasi_meta_environ_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(environ),
    TURBOWASM_WASI_META_PARAM32(environ_buf)
};
static const cmeta_param_desc turbowasm_wasi_meta_clock_time_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(clock_id),
    TURBOWASM_WASI_META_PARAM64(precision),
    TURBOWASM_WASI_META_PARAM32(timestamp)
};
static const cmeta_param_desc turbowasm_wasi_meta_random_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(buffer),
    TURBOWASM_WASI_META_PARAM32(buffer_length)
};
static const cmeta_param_desc turbowasm_wasi_meta_fd_write_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(iovs),
    TURBOWASM_WASI_META_PARAM32(iovs_length),
    TURBOWASM_WASI_META_PARAM32(written)
};
static const cmeta_param_desc turbowasm_wasi_meta_fd_read_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(iovs),
    TURBOWASM_WASI_META_PARAM32(iovs_length),
    TURBOWASM_WASI_META_PARAM32(read_count)
};
static const cmeta_param_desc turbowasm_wasi_meta_proc_exit_params[] = {
    TURBOWASM_WASI_META_PARAM32(exit_code)
};
static const cmeta_param_desc turbowasm_wasi_meta_fd_close_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd)
};
static const cmeta_param_desc turbowasm_wasi_meta_fd_prestat_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(prestat)
};
static const cmeta_param_desc turbowasm_wasi_meta_fd_prestat_dir_name_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(path),
    TURBOWASM_WASI_META_PARAM32(path_length)
};
static const cmeta_param_desc turbowasm_wasi_meta_path_create_directory_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(path),
    TURBOWASM_WASI_META_PARAM32(path_length)
};
static const cmeta_param_desc turbowasm_wasi_meta_path_remove_directory_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(path),
    TURBOWASM_WASI_META_PARAM32(path_length)
};
static const cmeta_param_desc turbowasm_wasi_meta_path_unlink_file_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(path),
    TURBOWASM_WASI_META_PARAM32(path_length)
};
static const cmeta_param_desc turbowasm_wasi_meta_fd_seek_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM64(offset),
    TURBOWASM_WASI_META_PARAM32(whence),
    TURBOWASM_WASI_META_PARAM32(new_offset)
};
static const cmeta_param_desc turbowasm_wasi_meta_fd_readdir_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(buffer),
    TURBOWASM_WASI_META_PARAM32(buffer_length),
    TURBOWASM_WASI_META_PARAM64(cookie),
    TURBOWASM_WASI_META_PARAM32(bufused)
};
static const cmeta_param_desc turbowasm_wasi_meta_fd_tell_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(offset)
};
static const cmeta_param_desc turbowasm_wasi_meta_fd_filestat_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(filestat)
};
static const cmeta_param_desc turbowasm_wasi_meta_path_filestat_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(lookup_flags),
    TURBOWASM_WASI_META_PARAM32(path),
    TURBOWASM_WASI_META_PARAM32(path_length),
    TURBOWASM_WASI_META_PARAM32(filestat)
};
static const cmeta_param_desc turbowasm_wasi_meta_path_open_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(dirflags),
    TURBOWASM_WASI_META_PARAM32(path),
    TURBOWASM_WASI_META_PARAM32(path_length),
    TURBOWASM_WASI_META_PARAM32(oflags),
    TURBOWASM_WASI_META_PARAM64(rights_base),
    TURBOWASM_WASI_META_PARAM64(rights_inheriting),
    TURBOWASM_WASI_META_PARAM32(fdflags),
    TURBOWASM_WASI_META_PARAM32(opened_fd)
};

static const cmeta_function_desc turbowasm_wasi_meta_args_sizes_get =
    TURBOWASM_WASI_META_FUNCTION(
        "args_sizes_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_args_sizes_get_params,
        CMETA_EFFECT_STATEFUL | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_args_get =
    TURBOWASM_WASI_META_FUNCTION(
        "args_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_args_get_params,
        CMETA_EFFECT_STATEFUL | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_environ_sizes_get =
    TURBOWASM_WASI_META_FUNCTION(
        "environ_sizes_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_environ_sizes_get_params,
        CMETA_EFFECT_STATEFUL | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_environ_get =
    TURBOWASM_WASI_META_FUNCTION(
        "environ_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_environ_get_params,
        CMETA_EFFECT_STATEFUL | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_clock_time_get =
    TURBOWASM_WASI_META_FUNCTION(
        "clock_time_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_clock_time_get_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_random_get =
    TURBOWASM_WASI_META_FUNCTION(
        "random_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_random_get_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_fd_write =
    TURBOWASM_WASI_META_FUNCTION(
        "fd_write", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_write_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_fd_read =
    TURBOWASM_WASI_META_FUNCTION(
        "fd_read", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_read_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_proc_exit = {
    sizeof(cmeta_function_desc), "proc_exit", &cmeta_type_void,
    turbowasm_wasi_meta_proc_exit_params,
    sizeof(turbowasm_wasi_meta_proc_exit_params) /
        sizeof(turbowasm_wasi_meta_proc_exit_params[0]),
    CMETA_EFFECT_STATEFUL | CMETA_EFFECT_MAY_FAIL,
    CMETA_PROP_NONE
};
static const cmeta_function_desc turbowasm_wasi_meta_fd_close =
    TURBOWASM_WASI_META_FUNCTION(
        "fd_close", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_close_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_fd_prestat_get =
    TURBOWASM_WASI_META_FUNCTION(
        "fd_prestat_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_prestat_get_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_fd_prestat_dir_name =
    TURBOWASM_WASI_META_FUNCTION(
        "fd_prestat_dir_name", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_prestat_dir_name_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_path_create_directory =
    TURBOWASM_WASI_META_FUNCTION(
        "path_create_directory", &cmeta_type_uint32,
        turbowasm_wasi_meta_path_create_directory_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_path_remove_directory =
    TURBOWASM_WASI_META_FUNCTION(
        "path_remove_directory", &cmeta_type_uint32,
        turbowasm_wasi_meta_path_remove_directory_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_path_unlink_file =
    TURBOWASM_WASI_META_FUNCTION(
        "path_unlink_file", &cmeta_type_uint32,
        turbowasm_wasi_meta_path_unlink_file_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_fd_seek =
    TURBOWASM_WASI_META_FUNCTION(
        "fd_seek", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_seek_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_fd_readdir =
    TURBOWASM_WASI_META_FUNCTION(
        "fd_readdir", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_readdir_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_fd_tell =
    TURBOWASM_WASI_META_FUNCTION(
        "fd_tell", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_tell_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_fd_filestat_get =
    TURBOWASM_WASI_META_FUNCTION(
        "fd_filestat_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_filestat_get_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_path_filestat_get =
    TURBOWASM_WASI_META_FUNCTION(
        "path_filestat_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_path_filestat_get_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_function_desc turbowasm_wasi_meta_path_open =
    TURBOWASM_WASI_META_FUNCTION(
        "path_open", &cmeta_type_uint32,
        turbowasm_wasi_meta_path_open_params,
        CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);

static const cmeta_param_desc turbowasm_wasi_meta_sock_accept_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(flags),
    TURBOWASM_WASI_META_PARAM32(accepted_fd)
};
static const cmeta_function_desc turbowasm_wasi_meta_sock_accept =
    TURBOWASM_WASI_META_FUNCTION("sock_accept", &cmeta_type_uint32,
        turbowasm_wasi_meta_sock_accept_params, CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_param_desc turbowasm_wasi_meta_sock_recv_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(ri_data),
    TURBOWASM_WASI_META_PARAM32(ri_data_len),
    TURBOWASM_WASI_META_PARAM32(ri_flags),
    TURBOWASM_WASI_META_PARAM32(ro_datalen),
    TURBOWASM_WASI_META_PARAM32(ro_flags)
};
static const cmeta_function_desc turbowasm_wasi_meta_sock_recv =
    TURBOWASM_WASI_META_FUNCTION("sock_recv", &cmeta_type_uint32,
        turbowasm_wasi_meta_sock_recv_params, CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_param_desc turbowasm_wasi_meta_sock_send_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(si_data),
    TURBOWASM_WASI_META_PARAM32(si_data_len),
    TURBOWASM_WASI_META_PARAM32(si_flags),
    TURBOWASM_WASI_META_PARAM32(so_datalen)
};
static const cmeta_function_desc turbowasm_wasi_meta_sock_send =
    TURBOWASM_WASI_META_FUNCTION("sock_send", &cmeta_type_uint32,
        turbowasm_wasi_meta_sock_send_params, CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_param_desc turbowasm_wasi_meta_sock_shutdown_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(how)
};
static const cmeta_function_desc turbowasm_wasi_meta_sock_shutdown =
    TURBOWASM_WASI_META_FUNCTION("sock_shutdown", &cmeta_type_uint32,
        turbowasm_wasi_meta_sock_shutdown_params, CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_param_desc turbowasm_wasi_meta_fd_fdstat_get_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(stat)
};
static const cmeta_function_desc turbowasm_wasi_meta_fd_fdstat_get =
    TURBOWASM_WASI_META_FUNCTION("fd_fdstat_get", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_fdstat_get_params, CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_param_desc turbowasm_wasi_meta_fd_fdstat_set_flags_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM32(flags)
};
static const cmeta_function_desc turbowasm_wasi_meta_fd_fdstat_set_flags =
    TURBOWASM_WASI_META_FUNCTION("fd_fdstat_set_flags", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_fdstat_set_flags_params, CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_param_desc turbowasm_wasi_meta_fd_fdstat_set_rights_params[] = {
    TURBOWASM_WASI_META_PARAM32(fd),
    TURBOWASM_WASI_META_PARAM64(rights_base),
    TURBOWASM_WASI_META_PARAM64(rights_inheriting)
};
static const cmeta_function_desc turbowasm_wasi_meta_fd_fdstat_set_rights =
    TURBOWASM_WASI_META_FUNCTION("fd_fdstat_set_rights", &cmeta_type_uint32,
        turbowasm_wasi_meta_fd_fdstat_set_rights_params, CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);
static const cmeta_param_desc turbowasm_wasi_meta_poll_oneoff_params[] = {
    TURBOWASM_WASI_META_PARAM32(subscriptions),
    TURBOWASM_WASI_META_PARAM32(events),
    TURBOWASM_WASI_META_PARAM32(count),
    TURBOWASM_WASI_META_PARAM32(nevents)
};
static const cmeta_function_desc turbowasm_wasi_meta_poll_oneoff =
    TURBOWASM_WASI_META_FUNCTION("poll_oneoff", &cmeta_type_uint32,
        turbowasm_wasi_meta_poll_oneoff_params, CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);

static const cmeta_param_desc turbowasm_wasi_meta_path_rename_params[] = {
    TURBOWASM_WASI_META_PARAM32(old_fd), TURBOWASM_WASI_META_PARAM32(old_path),
    TURBOWASM_WASI_META_PARAM32(old_length), TURBOWASM_WASI_META_PARAM32(new_fd),
    TURBOWASM_WASI_META_PARAM32(new_path), TURBOWASM_WASI_META_PARAM32(new_length)
};
static const cmeta_function_desc turbowasm_wasi_meta_path_rename =
    TURBOWASM_WASI_META_FUNCTION("path_rename", &cmeta_type_uint32,
        turbowasm_wasi_meta_path_rename_params, CMETA_EFFECT_IO | CMETA_EFFECT_MAY_FAIL);

static const cmeta_function_desc *const turbowasm_wasi_preview1_manifest[] = {
    &turbowasm_wasi_meta_args_sizes_get,
    &turbowasm_wasi_meta_args_get,
    &turbowasm_wasi_meta_environ_sizes_get,
    &turbowasm_wasi_meta_environ_get,
    &turbowasm_wasi_meta_clock_time_get,
    &turbowasm_wasi_meta_random_get,
    &turbowasm_wasi_meta_fd_write,
    &turbowasm_wasi_meta_fd_read,
    &turbowasm_wasi_meta_proc_exit,
    &turbowasm_wasi_meta_fd_close,
    &turbowasm_wasi_meta_fd_prestat_get,
    &turbowasm_wasi_meta_fd_prestat_dir_name,
    &turbowasm_wasi_meta_path_create_directory,
    &turbowasm_wasi_meta_path_remove_directory,
    &turbowasm_wasi_meta_path_unlink_file,
    &turbowasm_wasi_meta_fd_seek,
    &turbowasm_wasi_meta_fd_readdir,
    &turbowasm_wasi_meta_fd_tell,
    &turbowasm_wasi_meta_fd_filestat_get,
    &turbowasm_wasi_meta_path_filestat_get,
    &turbowasm_wasi_meta_path_open,
    &turbowasm_wasi_meta_sock_accept,
    &turbowasm_wasi_meta_sock_recv,
    &turbowasm_wasi_meta_sock_send,
    &turbowasm_wasi_meta_sock_shutdown,
    &turbowasm_wasi_meta_fd_fdstat_get,
    &turbowasm_wasi_meta_fd_fdstat_set_flags,
    &turbowasm_wasi_meta_fd_fdstat_set_rights,
    &turbowasm_wasi_meta_poll_oneoff,
    &turbowasm_wasi_meta_path_rename
};

#undef TURBOWASM_WASI_META_FUNCTION
#undef TURBOWASM_WASI_META_PARAM64
#undef TURBOWASM_WASI_META_PARAM32

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

static uint32_t turbowasm_wasi_memory_check(turbowasm_host_call *call, uint64_t address, uint64_t length) {
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    return turbowasm_host_call_memory_check64(call, 0, address, length, &trap) == TURBOWASM_OK ?
        TURBOWASM_WASI_ERRNO_SUCCESS : TURBOWASM_WASI_ERRNO_FAULT;
}
static uint32_t turbowasm_wasi_memory_write(turbowasm_host_call *call, uint64_t address,
    const void *source, size_t length) {
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    return turbowasm_host_call_memory_write64(call, 0, address, source, length, &trap) == TURBOWASM_OK ?
        TURBOWASM_WASI_ERRNO_SUCCESS : TURBOWASM_WASI_ERRNO_FAULT;
}
static uint32_t turbowasm_wasi_memory_read(turbowasm_host_call *call, uint64_t address,
    void *destination, size_t length) {
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    return turbowasm_host_call_memory_read64(call, 0, address, destination, length, &trap) == TURBOWASM_OK ?
        TURBOWASM_WASI_ERRNO_SUCCESS : TURBOWASM_WASI_ERRNO_FAULT;
}

static bool turbowasm_wasi_memory_shared(turbowasm_host_call *call) {
    turbowasm_memory_desc memory = {0};
    return turbowasm_module_memory_at(turbowasm_instance_module(
        turbowasm_host_call_instance(call)), 0, &memory) && memory.shared;
}

/* Each call owns its snapshot until the provider has returned. No Runtime
 * memory lock crosses the provider boundary, including a suspended callback. */
#define TURBOWASM_WASI_SHARED_IO_BYTES (1024u * 1024u)
typedef struct turbowasm_wasi_iov_snapshot {
    uint8_t entries[TURBOWASM_WASI_IOV_MAX * 8u];
    uint8_t *bytes;
} turbowasm_wasi_iov_snapshot;

static turbowasm_status turbowasm_wasi_sizes_get(
    const turbowasm_wasi_string_list *list,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    uint8_t output[4];
    uint32_t error;

    if (list == NULL || call == NULL ||
        arguments == NULL || argument_count != 2u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    error = turbowasm_wasi_memory_check(
        call, (uint32_t)arguments[0].as.i32,
        4u);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_memory_check(
            call, (uint32_t)arguments[1].as.i32,
            4u);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        turbowasm_wasi_store_u32(output, list->count);
        error = turbowasm_wasi_memory_write(call, (uint32_t)arguments[0].as.i32, output, 4u);
        if (!error) {
            turbowasm_wasi_store_u32(output, list->bytes);
            error = turbowasm_wasi_memory_write(call, (uint32_t)arguments[1].as.i32, output, 4u);
        }
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
    uint32_t table_address;
    uint32_t buffer_address;
    uint32_t error;
    uint32_t cursor = 0u;
    uint32_t index;
    uint64_t table_size;

    if (list == NULL || call == NULL ||
        arguments == NULL || argument_count != 2u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    table_address = (uint32_t)arguments[0].as.i32;
    buffer_address = (uint32_t)arguments[1].as.i32;
    table_size = (uint64_t)list->count * 4u;

    error = turbowasm_wasi_memory_check(call, table_address, table_size);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = turbowasm_wasi_memory_check(call, buffer_address, list->bytes);
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
            uint8_t entry[4];
            turbowasm_wasi_store_u32(entry, pointer);
            uint64_t entry_address = (uint64_t)table_address + (uint64_t)index * 4u;
            error = turbowasm_wasi_memory_write(call, entry_address, entry, 4u);
            if (!error) error = turbowasm_wasi_memory_write(call, pointer, list->items[index], length);
            if (error) break;
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
    uint8_t output[8];
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

    error = turbowasm_wasi_memory_check(
        call, (uint32_t)arguments[2].as.i32,
        sizeof(output));
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        error = impl->clock_time(
            impl->clock_context,
            (uint32_t)arguments[0].as.i32,
            (uint64_t)arguments[1].as.i64,
            &timestamp);
        if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
            turbowasm_wasi_store_u64(output, timestamp);
            error = turbowasm_wasi_memory_write(call, (uint32_t)arguments[2].as.i32, output, sizeof(output));
        }
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

static uint32_t turbowasm_wasi_snapshot_iovecs(
    turbowasm_host_call *call, uint32_t table_address, uint32_t count,
    bool writable, turbowasm_wasi_const_buffer *inputs,
    turbowasm_wasi_buffer *outputs, turbowasm_wasi_iov_snapshot *snapshot,
    uint64_t *out_capacity) {
    if (count > TURBOWASM_WASI_IOV_MAX) return TURBOWASM_WASI_ERRNO_INVAL;
    uint32_t error = turbowasm_wasi_memory_read(call, table_address, snapshot->entries, (size_t)count * 8u);
    if (error) return error;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = snapshot->entries + (size_t)i * 8u;
        uint32_t address = turbowasm_wasi_load_u32(entry);
        uint32_t length = turbowasm_wasi_load_u32(entry + 4u);
        error = turbowasm_wasi_memory_check(call, address, length);
        if (error) return error;
        total += length; /* At most 64 uint32_t lengths; cannot overflow. */
    }
    if (total > TURBOWASM_WASI_SHARED_IO_BYTES) return TURBOWASM_WASI_ERRNO_NOMEM;
    snapshot->bytes = malloc(total ? (size_t)total : 1u);
    if (!snapshot->bytes) return TURBOWASM_WASI_ERRNO_NOMEM;
    size_t offset = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = snapshot->entries + (size_t)i * 8u;
        uint32_t address = turbowasm_wasi_load_u32(entry);
        uint32_t length = turbowasm_wasi_load_u32(entry + 4u);
        if (writable) {
            outputs[i] = (turbowasm_wasi_buffer){snapshot->bytes + offset, length};
        } else {
            error = turbowasm_wasi_memory_read(call, address, snapshot->bytes + offset, length);
            if (error) return error;
            inputs[i] = (turbowasm_wasi_const_buffer){snapshot->bytes + offset, length};
        }
        offset += length;
    }
    *out_capacity = total;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t turbowasm_wasi_snapshot_readback(turbowasm_host_call *call,
    const turbowasm_wasi_iov_snapshot *snapshot, uint32_t count, uint32_t read_count) {
    size_t offset = 0;
    for (uint32_t i = 0; i < count && read_count; ++i) {
        const uint8_t *entry = snapshot->entries + (size_t)i * 8u;
        uint32_t address = turbowasm_wasi_load_u32(entry);
        uint32_t length = turbowasm_wasi_load_u32(entry + 4u);
        uint32_t copied = length < read_count ? length : read_count;
        uint32_t error = turbowasm_wasi_memory_write(call, address, snapshot->bytes + offset, copied);
        if (error) return error;
        offset += length;
        read_count -= copied;
    }
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

#include "wasi_preview1_sockets.inc"

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
    turbowasm_wasi_iov_snapshot snapshot = {0};
    uint8_t written_bytes[4];
    uint64_t capacity = 0u;
    uint32_t written = 0u;
    uint32_t error;
    bool table_fd = false;

    if (impl == NULL || !impl->allow_fd_write ||
        (impl->fd_write == NULL &&
         impl->fd_write_async == NULL && impl->async == NULL) ||
        call == NULL || arguments == NULL ||
        argument_count != 4u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32 ||
        arguments[2].kind != TURBOWASM_VALUE_I32 ||
        arguments[3].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    if (impl->async) {
        turbowasm_wasi_fs_descriptor_info info = {0};
        if (turbowasm_wasi_fs_descriptor_info_get(impl->filesystem, (uint32_t)arguments[0].as.i32, &info)) {
            table_fd = true;
            if (tw_wasi_fd_is_socket(impl->filesystem, (uint32_t)arguments[0].as.i32)) return tw_p1_socket_io(impl, call, arguments, false, false,
                results, result_capacity, result_count, trap);
        }
        if (!table_fd && !impl->fd_write && !impl->fd_write_async)
            return turbowasm_wasi_return_errno(results, result_capacity, result_count, trap, TURBOWASM_WASI_ERRNO_BADF);
    }

    bool shared = turbowasm_wasi_memory_shared(call);
    error = turbowasm_wasi_memory_check(
        call, (uint32_t)arguments[3].as.i32,
        4u);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        if (shared) error = turbowasm_wasi_snapshot_iovecs(call,
            (uint32_t)arguments[1].as.i32, (uint32_t)arguments[2].as.i32,
            false, buffers, NULL, &snapshot, &capacity);
        else error = turbowasm_wasi_collect_iovecs(
            call,
            (uint32_t)arguments[1].as.i32,
            (uint32_t)arguments[2].as.i32,
            false,
            buffers,
            NULL,
            &capacity);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        if (table_fd) {
            error=turbowasm_wasi_fs_fd_write(impl->filesystem, (uint32_t)arguments[0].as.i32,
                buffers, (uint32_t)arguments[2].as.i32, &written);
        } else if (impl->fd_write_async != NULL) {
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
            else {
                turbowasm_wasi_store_u32(written_bytes, written);
                error = turbowasm_wasi_memory_write(call, (uint32_t)arguments[3].as.i32, written_bytes, 4u);
            }
        }
    }
    free(snapshot.bytes);
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
    turbowasm_wasi_iov_snapshot snapshot = {0};
    uint8_t read_bytes[4];
    uint64_t capacity = 0u;
    uint32_t read_count = 0u;
    uint32_t error;
    bool table_fd = false;

    if (impl == NULL || !impl->allow_fd_read ||
        (impl->fd_read == NULL &&
         impl->fd_read_async == NULL && impl->async == NULL) ||
        call == NULL || arguments == NULL ||
        argument_count != 4u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I32 ||
        arguments[2].kind != TURBOWASM_VALUE_I32 ||
        arguments[3].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    if (impl->async) {
        turbowasm_wasi_fs_descriptor_info info = {0};
        if (turbowasm_wasi_fs_descriptor_info_get(impl->filesystem, (uint32_t)arguments[0].as.i32, &info)) {
            table_fd = true;
            if (tw_wasi_fd_is_socket(impl->filesystem, (uint32_t)arguments[0].as.i32)) return tw_p1_socket_io(impl, call, arguments, true, false,
                results, result_capacity, result_count, trap);
        }
        if (!table_fd && !impl->fd_read && !impl->fd_read_async)
            return turbowasm_wasi_return_errno(results, result_capacity, result_count, trap, TURBOWASM_WASI_ERRNO_BADF);
    }

    bool shared = turbowasm_wasi_memory_shared(call);
    error = turbowasm_wasi_memory_check(
        call, (uint32_t)arguments[3].as.i32,
        4u);
    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        if (shared) error = turbowasm_wasi_snapshot_iovecs(call,
            (uint32_t)arguments[1].as.i32, (uint32_t)arguments[2].as.i32,
            true, NULL, buffers, &snapshot, &capacity);
        else error = turbowasm_wasi_collect_iovecs(
            call,
            (uint32_t)arguments[1].as.i32,
            (uint32_t)arguments[2].as.i32,
            true,
            NULL,
            buffers,
            &capacity);
    }

    if (error == TURBOWASM_WASI_ERRNO_SUCCESS) {
        if (table_fd) {
            error=turbowasm_wasi_fs_fd_read(impl->filesystem, (uint32_t)arguments[0].as.i32,
                buffers, (uint32_t)arguments[2].as.i32, &read_count);
        } else if (impl->fd_read_async != NULL) {
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
            else {
                if (shared) error = turbowasm_wasi_snapshot_readback(call, &snapshot,
                    (uint32_t)arguments[2].as.i32, read_count);
                if (!error) {
                    turbowasm_wasi_store_u32(read_bytes, read_count);
                    error = turbowasm_wasi_memory_write(call, (uint32_t)arguments[3].as.i32, read_bytes, 4u);
                }
            }
        }
    }
    free(snapshot.bytes);
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
    if (impl == NULL || (!impl->allow_filesystem && !impl->async) ||
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

static turbowasm_status turbowasm_wasi_path_rename(
    void *context, turbowasm_host_call *call,
    const turbowasm_value *arguments, size_t argument_count,
    turbowasm_value *results, size_t result_capacity,
    size_t *result_count, turbowasm_trap *trap) {
    turbowasm_wasi_preview1_impl *impl = context;
    if (!impl || !impl->allow_filesystem || !impl->filesystem || !call ||
        !arguments || argument_count != 6) return TURBOWASM_INVALID_ARGUMENT;
    for (size_t i = 0; i < 6; ++i)
        if (arguments[i].kind != TURBOWASM_VALUE_I32) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_host_memory_span a = {0}, b = {0};
    uint32_t error = turbowasm_wasi_memory_span(call, (uint32_t)arguments[1].as.i32,
        (uint32_t)arguments[2].as.i32, &a);
    if (!error) error = turbowasm_wasi_memory_span(call, (uint32_t)arguments[4].as.i32,
        (uint32_t)arguments[5].as.i32, &b);
    if (!error) error = turbowasm_wasi_fs_path_rename(impl->filesystem,
        (uint32_t)arguments[0].as.i32, a.data, a.size,
        (uint32_t)arguments[3].as.i32, b.data, b.size);
    return turbowasm_wasi_return_errno(results, result_capacity, result_count, trap, error);
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

turbowasm_status turbowasm_wasi_preview1_destroy_checked(turbowasm_wasi_preview1 *wasi) {
    if (!wasi) return TURBOWASM_INVALID_ARGUMENT;
    if (!wasi->impl) return TURBOWASM_OK;
    turbowasm_wasi_preview1_impl *impl = wasi->impl;
    if (impl->async && (impl->async->active || impl->async->used)) return TURBOWASM_INVALID_ARGUMENT;
    if (impl->async) { free(impl->async->waits); free(impl->async); }
    turbowasm_wasi_string_list_destroy(&impl->environment);
    turbowasm_wasi_string_list_destroy(&impl->args);
    free(impl); wasi->impl = NULL; return TURBOWASM_OK;
}
void turbowasm_wasi_preview1_destroy(turbowasm_wasi_preview1 *wasi) {
    (void)turbowasm_wasi_preview1_destroy_checked(wasi);
}

void turbowasm_wasi_preview1_config_v2_init(turbowasm_wasi_preview1_config_v2 *c) {
    if (!c) return;
    *c = (turbowasm_wasi_preview1_config_v2){0};
    c->size=sizeof(*c); c->api_version=2;
    c->wait_capacity=64; c->subscription_capacity=64;
    c->io_bytes=65536; c->pending_bytes=4u*1024u*1024u;
}
turbowasm_status turbowasm_wasi_preview1_init_v2(turbowasm_wasi_preview1 *wasi,
    const turbowasm_wasi_preview1_config_v2 *c) {
    if (!c || c->size != sizeof(*c) || c->api_version != 2 || !c->base.filesystem ||
        !c->base.filesystem->impl || !c->wait_capacity || c->wait_capacity > SIZE_MAX/sizeof(tw_p1_wait) ||
        !c->subscription_capacity || c->subscription_capacity > UINT32_MAX/48u ||
        c->subscription_capacity > SIZE_MAX/sizeof(tw_p1_subscription) ||
        !c->io_bytes || c->io_bytes > UINT32_MAX || !c->pending_bytes) return TURBOWASM_INVALID_ARGUMENT;
    struct tw_p1_async *a=calloc(1, sizeof(*a));
    if (!a) return TURBOWASM_OUT_OF_MEMORY;
    a->waits=calloc(c->wait_capacity, sizeof(*a->waits));
    if (!a->waits) { free(a); return TURBOWASM_OUT_OF_MEMORY; }
    turbowasm_wasi_preview1_config base=c->base;
    /* Table dispatch is the default for v2; explicit legacy callbacks keep
     * serving fds which were not admitted to the common table. */
    if (base.allow_fd_read && !base.fd_read && !base.fd_read_async) {
        base.fd_read=turbowasm_wasi_fs_fd_read; base.fd_read_context=base.filesystem;
    }
    if (base.allow_fd_write && !base.fd_write && !base.fd_write_async) {
        base.fd_write=turbowasm_wasi_fs_fd_write; base.fd_write_context=base.filesystem;
    }
    turbowasm_status status=turbowasm_wasi_preview1_init(wasi, &base);
    if (status != TURBOWASM_OK) { free(a->waits); free(a); return status; }
    a->sockets=c->allow_sockets; a->poll=c->allow_poll; a->capacity=c->wait_capacity;
    a->subscriptions=c->subscription_capacity; a->io_bytes=c->io_bytes; a->budget=c->pending_bytes;
    ((turbowasm_wasi_preview1_impl *)wasi->impl)->async=a;
    return TURBOWASM_OK;
}

size_t turbowasm_wasi_preview1_function_count(void) {
    return sizeof(turbowasm_wasi_preview1_manifest) /
           sizeof(turbowasm_wasi_preview1_manifest[0]);
}

const cmeta_function_desc *turbowasm_wasi_preview1_function_at(
    size_t index) {
    if (index >= turbowasm_wasi_preview1_function_count())
        return NULL;
    return turbowasm_wasi_preview1_manifest[index];
}

const cmeta_function_desc *turbowasm_wasi_preview1_find_function(
    const char *name) {
    size_t index;

    if (name == NULL || name[0] == '\0')
        return NULL;
    for (index = 0u;
         index < turbowasm_wasi_preview1_function_count();
         ++index) {
        const cmeta_function_desc *function =
            turbowasm_wasi_preview1_manifest[index];
        if (function != NULL &&
            function->name != NULL &&
            strcmp(function->name, name) == 0)
            return function;
    }
    return NULL;
}

static const turbowasm_wasi_preview1_adapter_function_plan *
turbowasm_wasi_adapter_plan_for_metadata(
    const cmeta_function_desc *metadata) {
    size_t index;
    size_t count = turbowasm_wasi_preview1_function_count();

    if (metadata == NULL ||
        count != turbowasm_wasi_preview1_adapter_function_count)
        return NULL;

    for (index = 0u; index < count; ++index) {
        if (turbowasm_wasi_preview1_manifest[index] == metadata)
            return &turbowasm_wasi_preview1_adapter_functions[index];
    }
    return NULL;
}

static bool turbowasm_wasi_adapter_value_kind(
    turbowasm_wasi_preview1_adapter_carrier carrier,
    turbowasm_value_kind *out_kind) {
    if (out_kind == NULL)
        return false;

    switch (carrier) {
    case turbowasm_wasi_preview1_adapter_carrier_u32:
        *out_kind = TURBOWASM_VALUE_I32;
        return true;
    case turbowasm_wasi_preview1_adapter_carrier_u64:
        *out_kind = TURBOWASM_VALUE_I64;
        return true;
    case turbowasm_wasi_preview1_adapter_carrier_void:
        break;
    }
    return false;
}

static turbowasm_status turbowasm_wasi_define_cmeta_function(
    turbowasm_linker *linker,
    const cmeta_function_desc *metadata,
    turbowasm_host_function_fn function,
    void *context) {
    const turbowasm_wasi_preview1_adapter_function_plan *plan;
    turbowasm_value_kind params[9];
    turbowasm_value_kind result;
    turbowasm_host_function_type type = {0};
    size_t index;

    if (linker == NULL || metadata == NULL || function == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    plan = turbowasm_wasi_adapter_plan_for_metadata(metadata);
    if (plan == NULL ||
        plan->param_count > sizeof(params) / sizeof(params[0]))
        return TURBOWASM_INVALID_ARGUMENT;

    for (index = 0u; index < plan->param_count; ++index) {
        if (plan->params == NULL ||
            !turbowasm_wasi_adapter_value_kind(
                plan->params[index].carrier, &params[index]))
            return TURBOWASM_INVALID_ARGUMENT;
    }

    type.params = params;
    type.param_count = plan->param_count;
    if (plan->return_carrier !=
        turbowasm_wasi_preview1_adapter_carrier_void) {
        if (!turbowasm_wasi_adapter_value_kind(
                plan->return_carrier, &result))
            return TURBOWASM_INVALID_ARGUMENT;
        type.results = &result;
        type.result_count = 1u;
    }

    return turbowasm_linker_define_host_function(
        linker,
        turbowasm_wasi_namespace(),
        turbowasm_wasi_name(plan->function_name),
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

#define TURBOWASM_WASI_DEFINE(metadata_, function_) \
    do { \
        status = turbowasm_wasi_define_cmeta_function( \
            linker, (metadata_), (function_), impl); \
        if (status != TURBOWASM_OK) \
            return status; \
    } while (0)

    if (impl->allow_args) {
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_args_sizes_get,
            turbowasm_wasi_args_sizes_get);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_args_get,
            turbowasm_wasi_args_get);
    }

    if (impl->allow_environ) {
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_environ_sizes_get,
            turbowasm_wasi_environ_sizes_get);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_environ_get,
            turbowasm_wasi_environ_get);
    }

    if (impl->allow_clock) {
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_clock_time_get,
            turbowasm_wasi_clock_time_get);
    }

    if (impl->allow_random) {
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_random_get,
            turbowasm_wasi_random_get);
    }

    if (impl->allow_fd_write) {
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_fd_write,
            turbowasm_wasi_fd_write);
    }

    if (impl->allow_fd_read) {
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_fd_read,
            turbowasm_wasi_fd_read);
    }

    if (impl->allow_proc_exit) {
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_proc_exit,
            turbowasm_wasi_proc_exit);
    }

    if (impl->allow_filesystem || impl->async) {
        TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_fd_close, turbowasm_wasi_fd_close);
    }
    if (impl->async) {
        TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_fd_fdstat_get, tw_p1_fd_fdstat_get);
        TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_fd_fdstat_set_flags, tw_p1_fd_fdstat_set_flags);
        TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_fd_fdstat_set_rights, tw_p1_fd_fdstat_set_rights);
        if (impl->async->sockets) {
            TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_sock_accept, tw_p1_sock_accept);
            TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_sock_recv, tw_p1_sock_recv);
            TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_sock_send, tw_p1_sock_send);
            TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_sock_shutdown, tw_p1_sock_shutdown);
        }
        if (impl->async->poll) TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_poll_oneoff, tw_p1_poll_oneoff);
    }
    if (impl->allow_filesystem) {
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_fd_prestat_get,
            turbowasm_wasi_fd_prestat_get);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_fd_prestat_dir_name,
            turbowasm_wasi_fd_prestat_dir_name);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_path_create_directory,
            turbowasm_wasi_path_create_directory);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_path_remove_directory,
            turbowasm_wasi_path_remove_directory);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_path_unlink_file,
            turbowasm_wasi_path_unlink_file);
        TURBOWASM_WASI_DEFINE(&turbowasm_wasi_meta_path_rename, turbowasm_wasi_path_rename);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_fd_seek,
            turbowasm_wasi_fd_seek);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_fd_readdir,
            turbowasm_wasi_fd_readdir);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_fd_tell,
            turbowasm_wasi_fd_tell);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_fd_filestat_get,
            turbowasm_wasi_fd_filestat_get);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_path_filestat_get,
            turbowasm_wasi_path_filestat_get);
        TURBOWASM_WASI_DEFINE(
            &turbowasm_wasi_meta_path_open,
            turbowasm_wasi_path_open);
    }

#undef TURBOWASM_WASI_DEFINE

    return TURBOWASM_OK;
}
