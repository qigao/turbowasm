#include "wasi_fs_contract.h"

#include <turbowasm/wasi_littlefs.h>

#include "lfs.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    TEST_BLOCK_SIZE = 256,
    TEST_BLOCK_COUNT = 64,
    TEST_READ_SIZE = 16,
    TEST_PROG_SIZE = 16,
    TEST_CACHE_SIZE = 64,
    TEST_LOOKAHEAD_SIZE = 16,
    TEST_FILE_CAPACITY = 8
};

typedef struct littlefs_test_backend {
    uint8_t storage[TEST_BLOCK_SIZE * TEST_BLOCK_COUNT];
    uint8_t read_buffer[TEST_CACHE_SIZE];
    uint8_t prog_buffer[TEST_CACHE_SIZE];
    uint8_t lookahead_buffer[TEST_LOOKAHEAD_SIZE];
    lfs_t filesystem;
    struct lfs_config config;
    turbowasm_wasi_littlefs adapter;
} littlefs_test_backend;

static int test_read(
    const struct lfs_config *config,
    lfs_block_t block,
    lfs_off_t offset,
    void *buffer,
    lfs_size_t size) {
    littlefs_test_backend *backend =
        (littlefs_test_backend *)config->context;
    size_t begin;

    if (backend == NULL || buffer == NULL ||
        block >= TEST_BLOCK_COUNT ||
        offset > TEST_BLOCK_SIZE ||
        size > TEST_BLOCK_SIZE - offset)
        return LFS_ERR_IO;

    begin = (size_t)block * TEST_BLOCK_SIZE + offset;
    memcpy(buffer, backend->storage + begin, size);
    return LFS_ERR_OK;
}

static int test_prog(
    const struct lfs_config *config,
    lfs_block_t block,
    lfs_off_t offset,
    const void *buffer,
    lfs_size_t size) {
    littlefs_test_backend *backend =
        (littlefs_test_backend *)config->context;
    const uint8_t *source = (const uint8_t *)buffer;
    size_t begin;
    size_t index;

    if (backend == NULL || buffer == NULL ||
        block >= TEST_BLOCK_COUNT ||
        offset > TEST_BLOCK_SIZE ||
        size > TEST_BLOCK_SIZE - offset)
        return LFS_ERR_IO;

    begin = (size_t)block * TEST_BLOCK_SIZE + offset;
    for (index = 0u; index < size; ++index)
        backend->storage[begin + index] &= source[index];
    return LFS_ERR_OK;
}

static int test_erase(
    const struct lfs_config *config,
    lfs_block_t block) {
    littlefs_test_backend *backend =
        (littlefs_test_backend *)config->context;

    if (backend == NULL || block >= TEST_BLOCK_COUNT)
        return LFS_ERR_IO;

    memset(
        backend->storage + (size_t)block * TEST_BLOCK_SIZE,
        0xff,
        TEST_BLOCK_SIZE);
    return LFS_ERR_OK;
}

static int test_sync(const struct lfs_config *config) {
    return config != NULL ? LFS_ERR_OK : LFS_ERR_IO;
}

static void test_configure(littlefs_test_backend *backend) {
    backend->config = (struct lfs_config){0};
    backend->config.context = backend;
    backend->config.read = test_read;
    backend->config.prog = test_prog;
    backend->config.erase = test_erase;
    backend->config.sync = test_sync;
    backend->config.read_size = TEST_READ_SIZE;
    backend->config.prog_size = TEST_PROG_SIZE;
    backend->config.block_size = TEST_BLOCK_SIZE;
    backend->config.block_count = TEST_BLOCK_COUNT;
    backend->config.block_cycles = 100;
    backend->config.cache_size = TEST_CACHE_SIZE;
    backend->config.lookahead_size = TEST_LOOKAHEAD_SIZE;
    backend->config.read_buffer = backend->read_buffer;
    backend->config.prog_buffer = backend->prog_buffer;
    backend->config.lookahead_buffer = backend->lookahead_buffer;
}

static turbowasm_status littlefs_setup(
    void *context,
    turbowasm_wasi_fs_provider *out_provider,
    turbowasm_wasi_fs_file *out_root) {
    littlefs_test_backend *backend =
        (littlefs_test_backend *)context;
    turbowasm_wasi_littlefs_config adapter_config = {0};
    int result;

    if (backend == NULL ||
        out_provider == NULL ||
        out_root == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(backend, 0, sizeof(*backend));
    memset(backend->storage, 0xff, sizeof(backend->storage));
    test_configure(backend);

    result = lfs_format(
        &backend->filesystem, &backend->config);
    if (result < 0)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(&backend->filesystem, 0, sizeof(backend->filesystem));
    result = lfs_mount(
        &backend->filesystem, &backend->config);
    if (result < 0)
        return TURBOWASM_INVALID_ARGUMENT;

    adapter_config.filesystem = &backend->filesystem;
    adapter_config.file_capacity = TEST_FILE_CAPACITY;
    if (turbowasm_wasi_littlefs_init(
            &backend->adapter,
            &adapter_config) != TURBOWASM_OK) {
        (void)lfs_unmount(&backend->filesystem);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    if (!turbowasm_wasi_littlefs_provider(
            &backend->adapter,
            out_provider,
            out_root)) {
        (void)lfs_unmount(&backend->filesystem);
        return TURBOWASM_INVALID_ARGUMENT;
    }

    return TURBOWASM_OK;
}

static void littlefs_teardown(void *context) {
    littlefs_test_backend *backend =
        (littlefs_test_backend *)context;

    assert(backend != NULL);
    assert(turbowasm_wasi_littlefs_destroy(
        &backend->adapter) == TURBOWASM_OK);
    assert(lfs_unmount(&backend->filesystem) == LFS_ERR_OK);
}

int main(void) {
    littlefs_test_backend backend = {0};
    const turbowasm_wasi_fs_contract_fixture fixture = {
        &backend,
        littlefs_setup,
        littlefs_teardown
    };

    return turbowasm_wasi_fs_contract_run(&fixture);
}
