#include "wasi_fs_contract.h"
#include "wasi02_fs_contract.h"

#include <turbowasm/wasi_host_fs.h>

#include <salts_fs.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <string.h>

enum {
    HOST_TEST_FILE_CAPACITY = 8,
    HOST_TEST_PATH_CAPACITY = 512
};

typedef struct host_test_backend {
    char root_path[1024];
    turbowasm_wasi_host_fs adapter;
} host_test_backend;

static int host_join(
    char *out,
    size_t out_size,
    const char *base,
    const char *child) {
    return salts_fs_path_join(out, out_size, base, child);
}

static void host_cleanup_root(const char *root_path) {
    static const char *const files[] = {
        "file", "one", "over", "two", "x"
    };
    char path[1024];
    size_t index;

    if (root_path == NULL || root_path[0] == '\0')
        return;
    for (index = 0u; index < sizeof(files) / sizeof(files[0]); ++index) {
        if (host_join(path, sizeof(path), root_path, files[index]) == 0)
            (void)salts_fs_unlink(path);
    }
    if (host_join(path, sizeof(path), root_path, "dir") == 0)
        (void)salts_fs_rmdir(path);
    (void)salts_fs_rmdir(root_path);
}

static turbowasm_status host_setup(
    void *context,
    turbowasm_wasi_fs_provider *out_provider,
    turbowasm_wasi_fs_file *out_root) {
    host_test_backend *backend = (host_test_backend *)context;
    turbowasm_wasi_host_fs_config config = {0};
    char temp[768];

    if (backend == NULL || out_provider == NULL || out_root == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *backend = (host_test_backend){0};
    if (salts_fs_get_tmpdir(temp, sizeof(temp)) != 0 ||
        host_join(
            backend->root_path,
            sizeof(backend->root_path),
            temp,
            "turbowasm_wasi_host_contract") != 0)
        return TURBOWASM_INVALID_ARGUMENT;

    host_cleanup_root(backend->root_path);
    if (salts_fs_mkdir(backend->root_path, 0755) != 0)
        return TURBOWASM_INVALID_ARGUMENT;

    config.host_root = backend->root_path;
    config.file_capacity = HOST_TEST_FILE_CAPACITY;
    config.path_capacity = HOST_TEST_PATH_CAPACITY;
    if (turbowasm_wasi_host_fs_init(
            &backend->adapter, &config) != TURBOWASM_OK) {
        host_cleanup_root(backend->root_path);
        return TURBOWASM_INVALID_ARGUMENT;
    }
    if (!turbowasm_wasi_host_fs_provider(
            &backend->adapter, out_provider, out_root)) {
        host_cleanup_root(backend->root_path);
        return TURBOWASM_INVALID_ARGUMENT;
    }
    return TURBOWASM_OK;
}

static void host_teardown(void *context) {
    host_test_backend *backend = (host_test_backend *)context;

    assert(backend != NULL);
    assert(turbowasm_wasi_host_fs_destroy(
        &backend->adapter) == TURBOWASM_OK);
    host_cleanup_root(backend->root_path);
}

int main(void) {
    host_test_backend backend = {0};
    const turbowasm_wasi_fs_contract_fixture fixture = {
        &backend,
        host_setup,
        host_teardown
    };

    {
        int result = turbowasm_wasi_fs_contract_run(&fixture);
        if (result != 0)
            return result;
    }
    return turbowasm_wasi02_fs_contract_run(&fixture);
}
