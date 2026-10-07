#include <turbowasm/wasi_host_fs.h>

#include <cmeta_fs.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

enum {
    HOST_LOOKUPFLAGS_SYMLINK_FOLLOW = 1u
};

typedef struct host_security_paths {
    char root[1024];
    char moved[1024];
    char outside[1024];
} host_security_paths;

static int join_path(
    char *out,
    size_t out_size,
    const char *base,
    const char *child) {
    return cmeta_fs_path_join(out, out_size, base, child);
}

static int write_file(const char *path, const char *text) {
    cmeta_fs_buf_t buffer =
        cmeta_fs_buf_init((char *)text, strlen(text));
    return cmeta_fs_write_file(path, &buffer);
}

static void cleanup_dir(const char *root) {
    static const char *const names[] = {
        "inside", "escape", "child"
    };
    char path[1024];
    size_t index;

    if (root == NULL || root[0] == '\0')
        return;
    for (index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (join_path(path, sizeof(path), root, names[index]) == 0) {
            (void)cmeta_fs_unlink(path);
            (void)cmeta_fs_rmdir(path);
        }
    }
    (void)cmeta_fs_rmdir(root);
}

static void prepare_paths(host_security_paths *paths) {
    char temp[768];

    assert(paths != NULL);
    *paths = (host_security_paths){0};
    assert(cmeta_fs_get_tmpdir(temp, sizeof(temp)) == 0);
    assert(join_path(
        paths->root, sizeof(paths->root),
        temp, "turbowasm_wasi_host_security") == 0);
    assert(join_path(
        paths->moved, sizeof(paths->moved),
        temp, "turbowasm_wasi_host_security_moved") == 0);
    assert(join_path(
        paths->outside, sizeof(paths->outside),
        temp, "turbowasm_wasi_host_security_outside.txt") == 0);

    cleanup_dir(paths->root);
    cleanup_dir(paths->moved);
    (void)cmeta_fs_unlink(paths->outside);
}

static void test_escape_and_symlink(void) {
    host_security_paths paths;
    turbowasm_wasi_host_fs adapter = {0};
    turbowasm_wasi_host_fs_config config = {0};
    turbowasm_wasi_fs_provider provider = {0};
    turbowasm_wasi_fs_file root = {0};
    turbowasm_wasi_fs_file file = {0};
    turbowasm_wasi_fs_stat stat = {0};
    cmeta_fs_stat_t outside_stat = {0};
    const uint8_t parent_escape[] = {
        '.', '.', '/', 'o', 'u', 't'
    };
    const uint8_t absolute[] = {'/', 'o', 'u', 't'};
    const uint8_t embedded_nul[] = {'x', 0u, 'y'};
    const uint8_t link_path[] = {'e','s','c','a','p','e'};
    char link_host[1024];
    int symlink_result;

    prepare_paths(&paths);
    assert(cmeta_fs_mkdir(paths.root, 0755) == 0);
    assert(write_file(paths.outside, "outside") == 0);
    assert(join_path(
        link_host, sizeof(link_host), paths.root, "escape") == 0);
    symlink_result = cmeta_fs_symlink(
        paths.outside, link_host, 0);

    config.host_root = paths.root;
    config.file_capacity = 4u;
    config.path_capacity = 512u;
    assert(turbowasm_wasi_host_fs_init(
        &adapter, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_host_fs_provider(
        &adapter, &provider, &root));

    assert(provider.path_open(
        provider.context, root,
        0u,
        parent_escape, sizeof(parent_escape),
        0u,
        TURBOWASM_WASI_RIGHT_FD_READ,
        0u,
        0u,
        &file) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(file.object == 0u);

    assert(provider.path_open(
        provider.context, root,
        0u,
        absolute, sizeof(absolute),
        0u,
        TURBOWASM_WASI_RIGHT_FD_READ,
        0u,
        0u,
        &file) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    assert(file.object == 0u);

    assert(provider.path_stat(
        provider.context, root,
        0u,
        embedded_nul, sizeof(embedded_nul),
        &stat) == TURBOWASM_WASI_ERRNO_INVAL);

    if (symlink_result == 0) {
        assert(provider.path_open(
            provider.context, root,
            0u,
            link_path, sizeof(link_path),
            0u,
            TURBOWASM_WASI_RIGHT_FD_READ,
            0u,
            0u,
            &file) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);
        assert(file.object == 0u);

        assert(provider.path_stat(
            provider.context, root,
            0u,
            link_path, sizeof(link_path),
            &stat) == TURBOWASM_WASI_ERRNO_SUCCESS);
        assert(stat.file_type ==
               TURBOWASM_WASI_FILETYPE_SYMBOLIC_LINK);

        assert(provider.path_stat(
            provider.context, root,
            HOST_LOOKUPFLAGS_SYMLINK_FOLLOW,
            link_path, sizeof(link_path),
            &stat) == TURBOWASM_WASI_ERRNO_NOTCAPABLE);

        assert(provider.path_unlink_file(
            provider.context, root,
            link_path, sizeof(link_path)) ==
            TURBOWASM_WASI_ERRNO_SUCCESS);
        assert(cmeta_fs_stat(paths.outside, &outside_stat) == 0);
    }

    assert(provider.close(
        provider.context, root) ==
        TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_host_fs_destroy(
        &adapter) == TURBOWASM_OK);

    (void)cmeta_fs_unlink(link_host);
    assert(cmeta_fs_unlink(paths.outside) == 0);
    assert(cmeta_fs_rmdir(paths.root) == 0);
}

static void test_root_identity_survives_path_replacement(void) {
    host_security_paths paths;
    turbowasm_wasi_host_fs adapter = {0};
    turbowasm_wasi_host_fs_config config = {0};
    turbowasm_wasi_fs_provider provider = {0};
    turbowasm_wasi_fs_file root = {0};
    turbowasm_wasi_fs_file file = {0};
    turbowasm_wasi_buffer read_buffer;
    const uint8_t inside_path[] = {'i','n','s','i','d','e'};
    char original_path[1024];
    char replacement_path[1024];
    uint8_t bytes[16] = {0};
    uint32_t read = 0u;

    prepare_paths(&paths);
    assert(cmeta_fs_mkdir(paths.root, 0755) == 0);
    assert(join_path(
        original_path, sizeof(original_path),
        paths.root, "inside") == 0);
    assert(write_file(original_path, "original") == 0);

    config.host_root = paths.root;
    config.file_capacity = 4u;
    config.path_capacity = 512u;
    assert(turbowasm_wasi_host_fs_init(
        &adapter, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_host_fs_provider(
        &adapter, &provider, &root));

    assert(cmeta_fs_rename(paths.root, paths.moved) == 0);
    assert(cmeta_fs_mkdir(paths.root, 0755) == 0);
    assert(join_path(
        replacement_path, sizeof(replacement_path),
        paths.root, "inside") == 0);
    assert(write_file(replacement_path, "attacker") == 0);

    assert(provider.path_open(
        provider.context, root,
        0u,
        inside_path, sizeof(inside_path),
        0u,
        TURBOWASM_WASI_RIGHT_FD_READ,
        0u,
        0u,
        &file) == TURBOWASM_WASI_ERRNO_SUCCESS);

    read_buffer.data = bytes;
    read_buffer.size = 8u;
    assert(provider.read(
        provider.context, file,
        &read_buffer, 1u, &read) ==
        TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(read == 8u);
    assert(memcmp(bytes, "original", 8u) == 0);

    assert(provider.close(
        provider.context, file) ==
        TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(provider.close(
        provider.context, root) ==
        TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_host_fs_destroy(
        &adapter) == TURBOWASM_OK);

    assert(cmeta_fs_unlink(replacement_path) == 0);
    assert(cmeta_fs_rmdir(paths.root) == 0);
    assert(join_path(
        original_path, sizeof(original_path),
        paths.moved, "inside") == 0);
    assert(cmeta_fs_unlink(original_path) == 0);
    assert(cmeta_fs_rmdir(paths.moved) == 0);
}

int main(void) {
    test_escape_and_symlink();
    test_root_identity_survives_path_replacement();
    return 0;
}
