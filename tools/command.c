#include "command.h"
#include <turbowasm/wasi_host_fs.h>
#include <turbowasm/wasi_sockets.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { COMMAND_DESCRIPTORS = 64, COMMAND_MODULE_LIMIT = 16 * 1024 * 1024 };
typedef struct command_files {
    turbowasm_wasi_fs_provider disk;
    FILE *streams[3];
} command_files;

static int stream_index(turbowasm_wasi_fs_file f) {
    return f.generation == 1 && f.object >= UINT64_MAX - 2
        ? (int)(UINT64_MAX - f.object) : -1;
}
static uint32_t command_close(void *context, turbowasm_wasi_fs_file f) {
    command_files *p = context;
    if (stream_index(f) >= 0) return 0;
    return p->disk.close ? p->disk.close(p->disk.context, f) : TURBOWASM_WASI_ERRNO_BADF;
}
static uint32_t command_read(void *context, turbowasm_wasi_fs_file f,
    const turbowasm_wasi_buffer *buffers, size_t count, uint32_t *out) {
    command_files *p = context;
    int i = stream_index(f);
    if (i < 0) return p->disk.read ? p->disk.read(p->disk.context, f, buffers, count, out) : TURBOWASM_WASI_ERRNO_BADF;
    *out = 0;
    if (i != 0) return TURBOWASM_WASI_ERRNO_BADF;
    for (size_t n = 0; n < count; ++n) {
        size_t got = fread(buffers[n].data, 1, buffers[n].size, p->streams[i]);
        *out += (uint32_t)got;
        if (got < buffers[n].size)
            return ferror(p->streams[i]) && !*out ? TURBOWASM_WASI_ERRNO_IO : 0;
    }
    return 0;
}
static uint32_t command_write(void *context, turbowasm_wasi_fs_file f,
    const turbowasm_wasi_const_buffer *buffers, size_t count, uint32_t *out) {
    command_files *p = context;
    int i = stream_index(f);
    if (i < 0) return p->disk.write ? p->disk.write(p->disk.context, f, buffers, count, out) : TURBOWASM_WASI_ERRNO_BADF;
    *out = 0;
    if (i == 0) return TURBOWASM_WASI_ERRNO_BADF;
    for (size_t n = 0; n < count; ++n) {
        size_t put = fwrite(buffers[n].data, 1, buffers[n].size, p->streams[i]);
        *out += (uint32_t)put;
        if (put < buffers[n].size) return *out ? 0 : TURBOWASM_WASI_ERRNO_IO;
    }
    return fflush(p->streams[i]) ? TURBOWASM_WASI_ERRNO_IO : 0;
}
static uint32_t command_stat(void *context, turbowasm_wasi_fs_file f, turbowasm_wasi_fs_stat *out) {
    command_files *p = context;
    if (stream_index(f) >= 0) {
        *out = (turbowasm_wasi_fs_stat){0};
        out->file_type = TURBOWASM_WASI_FILETYPE_CHARACTER_DEVICE;
        return 0;
    }
    return p->disk.stat ? p->disk.stat(p->disk.context, f, out) : TURBOWASM_WASI_ERRNO_BADF;
}
static uint32_t command_seek(void *context, turbowasm_wasi_fs_file f, int64_t offset, uint8_t whence, uint64_t *out) {
    command_files *p = context;
    if (stream_index(f) >= 0) return TURBOWASM_WASI_ERRNO_NOTSUP;
    return p->disk.seek ? p->disk.seek(p->disk.context, f, offset, whence, out) : TURBOWASM_WASI_ERRNO_BADF;
}
static uint32_t command_tell(void *context, turbowasm_wasi_fs_file f, uint64_t *out) {
    return command_seek(context, f, 0, TURBOWASM_WASI_WHENCE_CUR, out);
}
static uint32_t command_open(void *context, turbowasm_wasi_fs_file dir, uint32_t lookup,
    const uint8_t *path, size_t length, uint32_t flags, uint64_t base, uint64_t inheriting,
    uint32_t fdflags, turbowasm_wasi_fs_file *out) {
    command_files *p = context;
    return p->disk.path_open ? p->disk.path_open(p->disk.context, dir, lookup, path, length,
        flags, base, inheriting, fdflags, out) : TURBOWASM_WASI_ERRNO_BADF;
}
static uint32_t command_path_stat(void *context, turbowasm_wasi_fs_file dir, uint32_t lookup,
    const uint8_t *path, size_t length, turbowasm_wasi_fs_stat *out) {
    command_files *p = context;
    return p->disk.path_stat ? p->disk.path_stat(p->disk.context, dir, lookup, path, length, out) : TURBOWASM_WASI_ERRNO_BADF;
}
#define COMMAND_MUTATION(name) \
static uint32_t command_##name(void *context, turbowasm_wasi_fs_file dir, const uint8_t *path, size_t length) { \
    command_files *p = context; \
    return p->disk.name ? p->disk.name(p->disk.context, dir, path, length) : TURBOWASM_WASI_ERRNO_BADF; \
}
COMMAND_MUTATION(path_create_directory)
COMMAND_MUTATION(path_remove_directory)
COMMAND_MUTATION(path_unlink_file)
#undef COMMAND_MUTATION
static uint32_t command_rename(void *context, turbowasm_wasi_fs_file a, const uint8_t *ap, size_t an,
    turbowasm_wasi_fs_file b, const uint8_t *bp, size_t bn) {
    command_files *p = context;
    return p->disk.path_rename ? p->disk.path_rename(p->disk.context, a, ap, an, b, bp, bn) : TURBOWASM_WASI_ERRNO_NOTSUP;
}
static uint32_t command_flags(void *context, turbowasm_wasi_fs_file f, uint16_t flags) {
    command_files *p = context;
    if (stream_index(f) >= 0) return flags ? TURBOWASM_WASI_ERRNO_NOTSUP : 0;
    return p->disk.set_flags ? p->disk.set_flags(p->disk.context, f, flags) : TURBOWASM_WASI_ERRNO_NOTSUP;
}
static uint32_t command_clock(void *context, uint32_t id, uint64_t precision, uint64_t *out) {
    (void)context; (void)precision;
    struct timespec now;
    if (id != TURBOWASM_WASI_CLOCKID_REALTIME) return TURBOWASM_WASI_ERRNO_NOTSUP;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC || now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (UINT64_MAX - (uint64_t)now.tv_nsec) / UINT64_C(1000000000))
        return TURBOWASM_WASI_ERRNO_IO;
    *out = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return 0;
}
static void command_exit(void *context, turbowasm_instance *instance, uint32_t code) {
    (void)instance;
    tw_command_result *r = context;
    r->exited = true;
    r->exit_code = code;
}

tw_command_result tw_command_run(const tw_command_options *o) {
    tw_command_result r = { .status = TURBOWASM_INVALID_ARGUMENT, .phase = "options" };
    if (!o || !o->module_path || !o->streams[0] || !o->streams[1] || !o->streams[2] ||
        !o->memory_bytes || !o->fuel) return r;
    FILE *input = NULL;
    uint8_t *bytes = NULL;
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_wasi_fs fs = {0};
    turbowasm_wasi_host_fs host = {0};
    turbowasm_wasi_preview1 wasi = {0};
    command_files files = {0};
    memcpy(files.streams, o->streams, sizeof(files.streams));
    turbowasm_wasi_fs_file root = {0};
    r.phase = "read module";
    input = fopen(o->module_path, "rb");
    if (!input || fseek(input, 0, SEEK_END)) goto done;
    long length = ftell(input);
    if (length <= 0 || length > COMMAND_MODULE_LIMIT || fseek(input, 0, SEEK_SET)) goto done;
    bytes = malloc((size_t)length);
    if (!bytes) { r.status = TURBOWASM_OUT_OF_MEMORY; goto done; }
    if (fread(bytes, 1, (size_t)length, input) != (size_t)length) goto done;
    fclose(input); input = NULL;
    turbowasm_runtime_config runtime = {0};
    runtime.limits.max_module_bytes = COMMAND_MODULE_LIMIT;
    runtime.limits.max_allocation_bytes = 64u * 1024u * 1024u;
    runtime.limits.max_linear_memory_bytes = o->memory_bytes;
    runtime.limits.max_table_elements = 65536;
    r.phase = "load module";
    r.status = turbowasm_module_load_borrowed_with_config(&module, bytes, (size_t)length, &runtime);
    if (r.status != TURBOWASM_OK) goto done;
    turbowasm_module_summary summary;
    if (!turbowasm_module_summary_get(&module, &summary) || summary.has_start) {
        r.status = TURBOWASM_UNSUPPORTED; r.phase = "command must export _start without a start section"; goto done;
    }
    uint32_t entry = UINT32_MAX;
    for (size_t i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(&module, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == 6 &&
            memcmp(e->name.bytes, "_start", 6) == 0) entry = e->item_index;
    }
    if (entry == UINT32_MAX) { r.status = TURBOWASM_INVALID_ARGUMENT; r.phase = "missing _start"; goto done; }
    r.phase = "filesystem";
    if (o->directory) {
        turbowasm_wasi_host_fs_config hc = {0};
        hc.host_root = o->directory; hc.file_capacity = COMMAND_DESCRIPTORS; hc.path_capacity = 4096;
        r.status = turbowasm_wasi_host_fs_init(&host, &hc);
        if (r.status != TURBOWASM_OK) goto done;
        if (!turbowasm_wasi_host_fs_provider(&host, &files.disk, &root)) {
            r.status = TURBOWASM_INVALID_ARGUMENT; goto done;
        }
    }
    turbowasm_wasi_fs_config fc = {0};
    fc.descriptor_capacity = COMMAND_DESCRIPTORS;
    fc.provider = (turbowasm_wasi_fs_provider){
        .context = &files, .close = command_close, .read = command_read, .write = command_write,
        .stat = command_stat, .seek = command_seek, .tell = command_tell,
        .path_open = command_open, .path_stat = command_path_stat,
        .path_create_directory = command_path_create_directory,
        .path_remove_directory = command_path_remove_directory, .path_unlink_file = command_path_unlink_file,
        .path_rename = command_rename, .set_flags = command_flags };
    r.status = turbowasm_wasi_fs_init(&fs, &fc);
    if (r.status != TURBOWASM_OK) goto done;
    turbowasm_wasi_fs_descriptor descriptor;
    for (uint32_t i = 0; i < 3; ++i) {
        uint64_t rights = (i == 0 ? TURBOWASM_WASI_RIGHT_FD_READ : TURBOWASM_WASI_RIGHT_FD_WRITE) |
            TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET | TURBOWASM_WASI_RIGHT_FD_FDSTAT_SET_FLAGS;
        r.status = turbowasm_wasi_fs_bind_descriptor_with_rights(&fs, i,
            (turbowasm_wasi_fs_file){UINT64_MAX - i, 1}, false, NULL, rights, 0, &descriptor);
        if (r.status != TURBOWASM_OK) goto done;
    }
    if (o->directory) {
        r.status = turbowasm_wasi_fs_bind_descriptor_with_rights(&fs, 3, root, true, ".",
            UINT64_MAX, UINT64_MAX, &descriptor);
        if (r.status != TURBOWASM_OK) goto done;
    }
    r.phase = "WASI imports";
    turbowasm_wasi_preview1_config_v2 wc;
    turbowasm_wasi_preview1_config_v2_init(&wc);
    wc.base.allow_args = true; wc.base.args = o->args; wc.base.arg_count = o->arg_count;
    wc.base.allow_environ = true; wc.base.environment = o->environment; wc.base.environment_count = o->environment_count;
    wc.base.allow_clock = true; wc.base.clock_time = command_clock;
    wc.base.allow_fd_read = true; wc.base.allow_fd_write = true;
    wc.base.allow_filesystem = true; wc.base.filesystem = &fs;
    wc.base.allow_proc_exit = true; wc.base.proc_exit = command_exit; wc.base.proc_exit_context = &r;
    r.status = turbowasm_wasi_preview1_init_v2(&wasi, &wc);
    if (r.status != TURBOWASM_OK) goto done;
    r.status = turbowasm_linker_init(&linker);
    if (r.status != TURBOWASM_OK) goto done;
    r.status = turbowasm_wasi_preview1_define(&wasi, &linker);
    if (r.status != TURBOWASM_OK) goto done;
    r.phase = "instantiate";
    r.status = turbowasm_instance_create_linked(&instance, &module, &linker);
    if (r.status != TURBOWASM_OK) goto done;
    r.phase = "execute";
    turbowasm_execution_options execution = { .fuel = o->fuel, .has_fuel_limit = true };
    size_t count = 0;
    r.status = turbowasm_instance_invoke_with_options(&instance, entry, NULL, 0, NULL, 0, &count, &r.trap, &execution);
    if (r.exited && r.status == TURBOWASM_INTERRUPTED) r.status = TURBOWASM_OK;
done:
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_wasi_preview1_destroy(&wasi);
    /* bind_next uses descriptors 3..capacity+3; explicit bindings are 0..3. */
    for (uint32_t fd = 0; fd <= COMMAND_DESCRIPTORS + 3; ++fd)
        (void)turbowasm_wasi_fs_close_fd(&fs, fd);
    (void)turbowasm_wasi_fs_destroy(&fs);
    if (host.impl) {
        /* Also covers failure before the root descriptor was bound. */
        if (files.disk.close) (void)files.disk.close(files.disk.context, root);
        (void)turbowasm_wasi_host_fs_destroy(&host);
    }
    turbowasm_module_destroy(&module);
    if (input) fclose(input);
    free(bytes);
    return r;
}
