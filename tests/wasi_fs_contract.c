#include "wasi_fs_contract.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    CONTRACT_OFLAGS_CREAT = 1u
};

typedef struct contract_probe {
    turbowasm_wasi_fs_provider inner;
    uint32_t close_calls;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t seek_calls;
    uint32_t tell_calls;
    uint32_t stat_calls;
    uint32_t path_open_calls;
    uint32_t path_stat_calls;
    uint32_t path_create_calls;
    uint32_t path_remove_calls;
    uint32_t path_unlink_calls;
    uint32_t readdir_calls;
    uint32_t force_path_stat_error;
    uint8_t last_path[64];
    size_t last_path_length;
} contract_probe;

static void contract_record_path(
    contract_probe *probe,
    const uint8_t *path,
    size_t path_length) {
    assert(probe != NULL);
    assert(path_length <= sizeof(probe->last_path));
    assert(path_length == 0u || path != NULL);
    probe->last_path_length = path_length;
    if (path_length != 0u)
        memcpy(probe->last_path, path, path_length);
}

static uint32_t probe_close(
    void *context,
    turbowasm_wasi_fs_file file) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->close_calls;
    return probe->inner.close(probe->inner.context, file);
}

static uint32_t probe_read(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->read_calls;
    return probe->inner.read(
        probe->inner.context, file, buffers, buffer_count, out_read);
}

static uint32_t probe_write(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->write_calls;
    return probe->inner.write(
        probe->inner.context, file, buffers, buffer_count, out_written);
}

static uint32_t probe_seek(
    void *context,
    turbowasm_wasi_fs_file file,
    int64_t offset,
    uint8_t whence,
    uint64_t *out_offset) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->seek_calls;
    return probe->inner.seek(
        probe->inner.context, file, offset, whence, out_offset);
}

static uint32_t probe_tell(
    void *context,
    turbowasm_wasi_fs_file file,
    uint64_t *out_offset) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->tell_calls;
    return probe->inner.tell(
        probe->inner.context, file, out_offset);
}

static uint32_t probe_stat(
    void *context,
    turbowasm_wasi_fs_file file,
    turbowasm_wasi_fs_stat *out_stat) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->stat_calls;
    return probe->inner.stat(
        probe->inner.context, file, out_stat);
}

static uint32_t probe_path_open(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t dirflags,
    const uint8_t *path,
    size_t path_length,
    uint32_t oflags,
    uint64_t rights_base,
    uint64_t rights_inheriting,
    uint32_t fdflags,
    turbowasm_wasi_fs_file *out_file) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->path_open_calls;
    contract_record_path(probe, path, path_length);
    return probe->inner.path_open(
        probe->inner.context,
        directory,
        dirflags,
        path,
        path_length,
        oflags,
        rights_base,
        rights_inheriting,
        fdflags,
        out_file);
}

static uint32_t probe_path_stat(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint32_t lookup_flags,
    const uint8_t *path,
    size_t path_length,
    turbowasm_wasi_fs_stat *out_stat) {
    contract_probe *probe = (contract_probe *)context;
    uint32_t forced;

    ++probe->path_stat_calls;
    contract_record_path(probe, path, path_length);
    forced = probe->force_path_stat_error;
    probe->force_path_stat_error = 0u;
    if (forced != 0u)
        return forced;
    return probe->inner.path_stat(
        probe->inner.context,
        directory,
        lookup_flags,
        path,
        path_length,
        out_stat);
}

static uint32_t probe_path_create_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->path_create_calls;
    contract_record_path(probe, path, path_length);
    return probe->inner.path_create_directory(
        probe->inner.context, directory, path, path_length);
}

static uint32_t probe_path_remove_directory(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->path_remove_calls;
    contract_record_path(probe, path, path_length);
    return probe->inner.path_remove_directory(
        probe->inner.context, directory, path, path_length);
}

static uint32_t probe_path_unlink_file(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->path_unlink_calls;
    contract_record_path(probe, path, path_length);
    return probe->inner.path_unlink_file(
        probe->inner.context, directory, path, path_length);
}

static uint32_t probe_readdir(
    void *context,
    turbowasm_wasi_fs_file directory,
    uint64_t cookie,
    turbowasm_wasi_fs_dirent *out_entry,
    bool *out_has_entry) {
    contract_probe *probe = (contract_probe *)context;
    ++probe->readdir_calls;
    return probe->inner.readdir(
        probe->inner.context,
        directory,
        cookie,
        out_entry,
        out_has_entry);
}

static void contract_setup_probe(
    const turbowasm_wasi_fs_contract_fixture *fixture,
    contract_probe *probe,
    turbowasm_wasi_fs_provider *out_provider,
    turbowasm_wasi_fs_file *out_root) {
    turbowasm_status status;

    assert(fixture != NULL);
    assert(fixture->setup != NULL);
    assert(fixture->teardown != NULL);
    assert(probe != NULL);
    assert(out_provider != NULL);
    assert(out_root != NULL);

    *probe = (contract_probe){0};
    status = fixture->setup(
        fixture->context, &probe->inner, out_root);
    assert(status == TURBOWASM_OK);

    assert(probe->inner.close != NULL);
    assert(probe->inner.read != NULL);
    assert(probe->inner.write != NULL);
    assert(probe->inner.seek != NULL);
    assert(probe->inner.tell != NULL);
    assert(probe->inner.stat != NULL);
    assert(probe->inner.path_open != NULL);
    assert(probe->inner.path_stat != NULL);
    assert(probe->inner.path_create_directory != NULL);
    assert(probe->inner.path_remove_directory != NULL);
    assert(probe->inner.path_unlink_file != NULL);
    assert(probe->inner.readdir != NULL);

    *out_provider = probe->inner;
    out_provider->context = probe;
    out_provider->close = probe_close;
    out_provider->read = probe_read;
    out_provider->write = probe_write;
    out_provider->seek = probe_seek;
    out_provider->tell = probe_tell;
    out_provider->stat = probe_stat;
    out_provider->path_open = probe_path_open;
    out_provider->path_stat = probe_path_stat;
    out_provider->path_create_directory = probe_path_create_directory;
    out_provider->path_remove_directory = probe_path_remove_directory;
    out_provider->path_unlink_file = probe_path_unlink_file;
    out_provider->readdir = probe_readdir;
}

static uint64_t contract_root_rights(void) {
    return
        TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY |
        TURBOWASM_WASI_RIGHT_PATH_OPEN |
        TURBOWASM_WASI_RIGHT_FD_READDIR |
        TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET |
        TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY |
        TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE;
}

static uint64_t contract_file_rights(void) {
    return
        TURBOWASM_WASI_RIGHT_FD_READ |
        TURBOWASM_WASI_RIGHT_FD_WRITE |
        TURBOWASM_WASI_RIGHT_FD_SEEK |
        TURBOWASM_WASI_RIGHT_FD_TELL |
        TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET;
}

static void contract_run_surface(
    const turbowasm_wasi_fs_contract_fixture *fixture) {
    contract_probe probe;
    turbowasm_wasi_fs_provider provider = {0};
    turbowasm_wasi_fs_file root_file = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_descriptor root_descriptor = {0};
    turbowasm_wasi_fs_descriptor_info info = {0};
    turbowasm_wasi_fs_stat stat = {0};
    const uint8_t dir_path[] = {'d','i','r'};
    const uint8_t file_path[] = {'f','i','l','e'};
    const uint8_t embedded_path[] = {'x',0u,'y'};
    const uint8_t payload[] = {'a','b','c'};
    uint8_t readback[sizeof(payload)] = {0};
    turbowasm_wasi_const_buffer write_buffer = {
        payload, sizeof(payload)
    };
    turbowasm_wasi_buffer read_buffer = {
        readback, sizeof(readback)
    };
    uint32_t file_fd = 0u;
    uint32_t transferred = 0u;
    uint64_t offset = 0u;
    uint64_t cookie = 0u;
    bool saw_dir = false;
    bool saw_file = false;
    char guest_root[] = "/sandbox";
    size_t iterations;

    contract_setup_probe(
        fixture, &probe, &provider, &root_file);
    config.descriptor_capacity = 4u;
    config.provider = provider;

    assert(turbowasm_wasi_fs_init(
        &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        &filesystem,
        3u,
        root_file,
        true,
        guest_root,
        contract_root_rights(),
        contract_file_rights(),
        &root_descriptor) == TURBOWASM_OK);

    guest_root[1] = 'X';
    assert(turbowasm_wasi_fs_descriptor_info_get(
        &filesystem, 3u, &info));
    assert(info.preopen);
    assert(strcmp(info.guest_path, "/sandbox") == 0);

    assert(turbowasm_wasi_fs_path_create_directory(
        &filesystem, 3u,
        dir_path, sizeof(dir_path)) ==
        TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.path_create_calls == 1u);
    assert(probe.last_path_length == sizeof(dir_path));
    assert(memcmp(
        probe.last_path, dir_path, sizeof(dir_path)) == 0);

    assert(turbowasm_wasi_fs_path_open(
        &filesystem,
        3u,
        0u,
        file_path,
        sizeof(file_path),
        CONTRACT_OFLAGS_CREAT,
        contract_file_rights(),
        0u,
        0u,
        &file_fd) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(file_fd == 4u);
    assert(probe.path_open_calls == 1u);
    assert(probe.last_path_length == sizeof(file_path));
    assert(memcmp(
        probe.last_path, file_path, sizeof(file_path)) == 0);

    assert(turbowasm_wasi_fs_fd_write(
        &filesystem,
        file_fd,
        &write_buffer,
        1u,
        &transferred) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(transferred == sizeof(payload));
    assert(probe.write_calls == 1u);

    assert(turbowasm_wasi_fs_fd_seek(
        &filesystem,
        file_fd,
        0,
        TURBOWASM_WASI_WHENCE_SET,
        &offset) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(offset == 0u);
    assert(probe.seek_calls == 1u);

    assert(turbowasm_wasi_fs_fd_read(
        &filesystem,
        file_fd,
        &read_buffer,
        1u,
        &transferred) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(transferred == sizeof(payload));
    assert(memcmp(readback, payload, sizeof(payload)) == 0);
    assert(probe.read_calls == 1u);

    assert(turbowasm_wasi_fs_fd_tell(
        &filesystem,
        file_fd,
        &offset) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(offset == sizeof(payload));
    assert(probe.tell_calls == 1u);

    assert(turbowasm_wasi_fs_fd_stat(
        &filesystem,
        file_fd,
        &stat) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(stat.file_type == TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
    assert(stat.size == sizeof(payload));
    assert(probe.stat_calls == 1u);

    stat = (turbowasm_wasi_fs_stat){0};
    assert(turbowasm_wasi_fs_path_stat(
        &filesystem,
        3u,
        0u,
        file_path,
        sizeof(file_path),
        &stat) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(stat.file_type == TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
    assert(stat.size == sizeof(payload));
    assert(probe.path_stat_calls == 1u);

    {
        uint32_t before = probe.path_stat_calls;
        (void)turbowasm_wasi_fs_path_stat(
            &filesystem,
            3u,
            0u,
            embedded_path,
            sizeof(embedded_path),
            &stat);
        assert(probe.path_stat_calls == before + 1u);
        assert(probe.last_path_length == sizeof(embedded_path));
        assert(memcmp(
            probe.last_path,
            embedded_path,
            sizeof(embedded_path)) == 0);
    }

    for (iterations = 0u; iterations < 16u; ++iterations) {
        turbowasm_wasi_fs_dirent entry = {0};
        bool has_entry = false;

        assert(turbowasm_wasi_fs_fd_readdir(
            &filesystem,
            3u,
            cookie,
            &entry,
            &has_entry) == TURBOWASM_WASI_ERRNO_SUCCESS);
        if (!has_entry)
            break;

        if (entry.name_length == sizeof(dir_path) &&
            memcmp(entry.name, dir_path, sizeof(dir_path)) == 0)
            saw_dir = true;
        if (entry.name_length == sizeof(file_path) &&
            memcmp(entry.name, file_path, sizeof(file_path)) == 0)
            saw_file = true;
        cookie = entry.next_cookie;
    }
    assert(iterations < 16u);
    assert(saw_dir);
    assert(saw_file);
    assert(probe.readdir_calls >= 3u);

    assert(turbowasm_wasi_fs_close_fd(
        &filesystem, file_fd) == TURBOWASM_WASI_ERRNO_SUCCESS);

    assert(turbowasm_wasi_fs_path_unlink_file(
        &filesystem,
        3u,
        file_path,
        sizeof(file_path)) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.path_unlink_calls == 1u);

    assert(turbowasm_wasi_fs_path_remove_directory(
        &filesystem,
        3u,
        dir_path,
        sizeof(dir_path)) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.path_remove_calls == 1u);

    assert(turbowasm_wasi_fs_close_fd(
        &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
        &filesystem) == TURBOWASM_OK);
    fixture->teardown(fixture->context);
}

static void contract_run_capacity_and_generation(
    const turbowasm_wasi_fs_contract_fixture *fixture) {
    contract_probe probe;
    turbowasm_wasi_fs_provider provider = {0};
    turbowasm_wasi_fs_file root_file = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_descriptor root_descriptor = {0};
    turbowasm_wasi_fs_descriptor_info old_info = {0};
    turbowasm_wasi_fs_descriptor_info new_info = {0};
    const uint8_t one_path[] = {'o','n','e'};
    const uint8_t overflow_path[] = {'o','v','e','r'};
    const uint8_t two_path[] = {'t','w','o'};
    uint32_t file_fd = 0u;
    uint32_t close_before;
    uint32_t open_before;

    contract_setup_probe(
        fixture, &probe, &provider, &root_file);
    config.descriptor_capacity = 2u;
    config.provider = provider;

    assert(turbowasm_wasi_fs_init(
        &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        &filesystem,
        3u,
        root_file,
        true,
        "/",
        TURBOWASM_WASI_RIGHT_PATH_OPEN,
        TURBOWASM_WASI_RIGHT_FD_WRITE,
        &root_descriptor) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_path_open(
        &filesystem, 3u, 0u,
        one_path, sizeof(one_path),
        CONTRACT_OFLAGS_CREAT,
        TURBOWASM_WASI_RIGHT_FD_WRITE,
        0u, 0u,
        &file_fd) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(file_fd == 4u);
    assert(turbowasm_wasi_fs_descriptor_info_get(
        &filesystem, file_fd, &old_info));

    close_before = probe.close_calls;
    open_before = probe.path_open_calls;
    assert(turbowasm_wasi_fs_path_open(
        &filesystem, 3u, 0u,
        overflow_path, sizeof(overflow_path),
        CONTRACT_OFLAGS_CREAT,
        TURBOWASM_WASI_RIGHT_FD_WRITE,
        0u, 0u,
        &file_fd) == TURBOWASM_WASI_ERRNO_MFILE);
    assert(probe.close_calls == close_before);
    assert(probe.path_open_calls == open_before);

    assert(turbowasm_wasi_fs_close_descriptor(
        &filesystem,
        old_info.descriptor) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(probe.close_calls == close_before + 1u);

    assert(turbowasm_wasi_fs_path_open(
        &filesystem, 3u, 0u,
        two_path, sizeof(two_path),
        CONTRACT_OFLAGS_CREAT,
        TURBOWASM_WASI_RIGHT_FD_WRITE,
        0u, 0u,
        &file_fd) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(file_fd == 4u);
    assert(turbowasm_wasi_fs_descriptor_info_get(
        &filesystem, file_fd, &new_info));
    assert(new_info.descriptor.slot == old_info.descriptor.slot);
    assert(new_info.descriptor.generation !=
           old_info.descriptor.generation);

    close_before = probe.close_calls;
    assert(turbowasm_wasi_fs_close_descriptor(
        &filesystem,
        old_info.descriptor) == TURBOWASM_WASI_ERRNO_BADF);
    assert(probe.close_calls == close_before);

    assert(turbowasm_wasi_fs_close_fd(
        &filesystem, file_fd) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_close_fd(
        &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
        &filesystem) == TURBOWASM_OK);
    fixture->teardown(fixture->context);
}

static void contract_run_rights_and_errors(
    const turbowasm_wasi_fs_contract_fixture *fixture) {
    contract_probe probe;
    turbowasm_wasi_fs_provider provider = {0};
    turbowasm_wasi_fs_file root_file = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_descriptor root_descriptor = {0};
    turbowasm_wasi_fs_stat stat = {0};
    turbowasm_wasi_fs_dirent entry = {0};
    const uint8_t path[] = {'x'};
    uint32_t file_fd = 0u;
    bool has_entry = false;

    contract_setup_probe(
        fixture, &probe, &provider, &root_file);
    config.descriptor_capacity = 1u;
    config.provider = provider;
    assert(turbowasm_wasi_fs_init(
        &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        &filesystem,
        3u,
        root_file,
        true,
        "/",
        TURBOWASM_WASI_RIGHT_PATH_OPEN,
        TURBOWASM_WASI_RIGHT_FD_WRITE,
        &root_descriptor) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_path_create_directory(
        &filesystem, 3u,
        path, sizeof(path)) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(probe.path_create_calls == 0u);

    assert(turbowasm_wasi_fs_fd_readdir(
        &filesystem, 3u, 0u,
        &entry, &has_entry) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(probe.readdir_calls == 0u);

    assert(turbowasm_wasi_fs_path_open(
        &filesystem, 3u, 0u,
        path, sizeof(path),
        CONTRACT_OFLAGS_CREAT,
        TURBOWASM_WASI_RIGHT_FD_READ,
        0u, 0u,
        &file_fd) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(probe.path_open_calls == 0u);

    assert(turbowasm_wasi_fs_close_fd(
        &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
        &filesystem) == TURBOWASM_OK);
    fixture->teardown(fixture->context);

    contract_setup_probe(
        fixture, &probe, &provider, &root_file);
    config = (turbowasm_wasi_fs_config){0};
    config.descriptor_capacity = 1u;
    config.provider = provider;
    assert(turbowasm_wasi_fs_init(
        &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        &filesystem,
        3u,
        root_file,
        true,
        "/",
        TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET,
        0u,
        &root_descriptor) == TURBOWASM_OK);

    probe.force_path_stat_error = TURBOWASM_WASI_ERRNO_IO;
    assert(turbowasm_wasi_fs_path_stat(
        &filesystem, 3u, 0u,
        path, sizeof(path),
        &stat) == TURBOWASM_WASI_ERRNO_IO);
    assert(probe.path_stat_calls == 1u);

    assert(turbowasm_wasi_fs_close_fd(
        &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
        &filesystem) == TURBOWASM_OK);
    fixture->teardown(fixture->context);

    contract_setup_probe(
        fixture, &probe, &provider, &root_file);
    provider.path_stat = NULL;
    config = (turbowasm_wasi_fs_config){0};
    config.descriptor_capacity = 1u;
    config.provider = provider;
    assert(turbowasm_wasi_fs_init(
        &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
        &filesystem,
        3u,
        root_file,
        true,
        "/",
        TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET,
        0u,
        &root_descriptor) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_path_stat(
        &filesystem, 3u, 0u,
        path, sizeof(path),
        &stat) == TURBOWASM_WASI_ERRNO_NOSYS);
    assert(probe.path_stat_calls == 0u);

    assert(turbowasm_wasi_fs_close_fd(
        &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
        &filesystem) == TURBOWASM_OK);
    fixture->teardown(fixture->context);
}

int turbowasm_wasi_fs_contract_run(
    const turbowasm_wasi_fs_contract_fixture *fixture) {
    assert(fixture != NULL);
    assert(fixture->setup != NULL);
    assert(fixture->teardown != NULL);

    contract_run_surface(fixture);
    contract_run_capacity_and_generation(fixture);
    contract_run_rights_and_errors(fixture);
    return 0;
}
