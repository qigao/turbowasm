#include <turbowasm/wasi_fs.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct fake_fs {
    uint32_t close_calls;
    uint32_t read_calls;
    uint32_t write_calls;
    turbowasm_wasi_fs_file last_file;
    bool fail_close;
} fake_fs;

static uint32_t fake_close(
    void *context,
    turbowasm_wasi_fs_file file) {
    fake_fs *fake = (fake_fs *)context;
    assert(fake != NULL);
    ++fake->close_calls;
    fake->last_file = file;
    return fake->fail_close
        ? TURBOWASM_WASI_ERRNO_IO
        : TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_read(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_read) {
    fake_fs *fake = (fake_fs *)context;
    size_t index;
    uint32_t total = 0u;

    assert(fake != NULL);
    assert(out_read != NULL);
    ++fake->read_calls;
    fake->last_file = file;

    for (index = 0u; index < buffer_count; ++index) {
        size_t cursor;
        for (cursor = 0u; cursor < buffers[index].size; ++cursor)
            buffers[index].data[cursor] =
                (uint8_t)('A' + (int)cursor);
        total += (uint32_t)buffers[index].size;
    }
    *out_read = total;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_write(
    void *context,
    turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers,
    size_t buffer_count,
    uint32_t *out_written) {
    fake_fs *fake = (fake_fs *)context;
    size_t index;
    uint32_t total = 0u;

    assert(fake != NULL);
    assert(out_written != NULL);
    ++fake->write_calls;
    fake->last_file = file;

    for (index = 0u; index < buffer_count; ++index)
        total += (uint32_t)buffers[index].size;
    *out_written = total;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_wasi_fs_file file_id(
    uint64_t object,
    uint32_t generation) {
    turbowasm_wasi_fs_file file;
    file.object = object;
    file.generation = generation;
    return file;
}

static void test_bounded_descriptor_lifecycle(void) {
    fake_fs fake = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi_fs_descriptor root = {0};
    turbowasm_wasi_fs_descriptor file = {0};
    turbowasm_wasi_fs_descriptor replacement = {0};
    turbowasm_wasi_fs_descriptor ignored = {0};
    turbowasm_wasi_fs_descriptor_info info = {0};
    char root_path[] = "/sandbox";
    uint8_t read_a[2] = {0};
    uint8_t read_b[3] = {0};
    const uint8_t write_a[] = {'h','i'};
    const uint8_t write_b[] = {'x','y','z'};
    turbowasm_wasi_buffer reads[2] = {
        {read_a, sizeof(read_a)},
        {read_b, sizeof(read_b)}
    };
    turbowasm_wasi_const_buffer writes[2] = {
        {write_a, sizeof(write_a)},
        {write_b, sizeof(write_b)}
    };
    uint32_t transferred = 0u;

    config.descriptor_capacity = 2u;
    config.provider.context = &fake;
    config.provider.close = fake_close;
    config.provider.read = fake_read;
    config.provider.write = fake_write;

    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);

    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem,
               3u,
               file_id(100u, 1u),
               true,
               root_path,
               &root) == TURBOWASM_OK);
    root_path[1] = 'X';

    assert(turbowasm_wasi_fs_descriptor_info_get(
               &filesystem, 3u, &info));
    assert(info.preopen);
    assert(strcmp(info.guest_path, "/sandbox") == 0);
    assert(info.file.object == 100u);
    assert(info.descriptor.slot == root.slot);
    assert(info.descriptor.generation == root.generation);

    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem,
               4u,
               file_id(200u, 1u),
               false,
               NULL,
               &file) == TURBOWASM_OK);

    /* Fixed table is full; no hidden growth. */
    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem,
               5u,
               file_id(300u, 1u),
               false,
               NULL,
               &ignored) == TURBOWASM_OUT_OF_MEMORY);
    assert(ignored.generation == 0u);

    assert(turbowasm_wasi_fs_fd_write(
               &filesystem, 4u,
               writes, 2u,
               &transferred) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(transferred == 5u);
    assert(fake.write_calls == 1u);
    assert(fake.last_file.object == 200u);

    transferred = 0u;
    assert(turbowasm_wasi_fs_fd_read(
               &filesystem, 4u,
               reads, 2u,
               &transferred) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(transferred == 5u);
    assert(fake.read_calls == 1u);
    assert(read_a[0] == 'A' && read_a[1] == 'B');
    assert(read_b[0] == 'A' && read_b[2] == 'C');

    /* Destruction is fail-closed until descriptors are explicitly closed. */
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_INVALID_ARGUMENT);

    /* Provider close failure retains descriptor ownership. */
    fake.fail_close = true;
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, file) == TURBOWASM_WASI_ERRNO_IO);
    assert(turbowasm_wasi_fs_descriptor_info_get(
               &filesystem, 4u, &info));
    fake.fail_close = false;

    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, file) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(fake.close_calls == 2u);
    assert(!turbowasm_wasi_fs_descriptor_info_get(
        &filesystem, 4u, &info));

    /* Reusing the slot increments table generation. */
    assert(turbowasm_wasi_fs_bind_descriptor(
               &filesystem,
               4u,
               file_id(201u, 2u),
               false,
               NULL,
               &replacement) == TURBOWASM_OK);
    assert(replacement.slot == file.slot);
    assert(replacement.generation != file.generation);

    /* Stale retained descriptor cannot close the reused slot. */
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, file) == TURBOWASM_WASI_ERRNO_BADF);
    assert(fake.close_calls == 2u);

    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 4u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(fake.close_calls == 3u);
    assert(fake.last_file.object == 201u);

    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(fake.close_calls == 4u);
    assert(fake.last_file.object == 100u);

    assert(turbowasm_wasi_fs_close_fd(
               &filesystem, 3u) == TURBOWASM_WASI_ERRNO_BADF);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
    assert(filesystem.impl == NULL);
}

static void test_config_validation(void) {
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = {0};
    fake_fs fake = {0};

    config.descriptor_capacity = 1u;
    config.provider.context = &fake;
    config.provider.close = fake_close;
    config.provider.read = fake_read;
    /* write missing */
    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_INVALID_ARGUMENT);

    config.provider.write = fake_write;
    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
}

int main(void) {
    test_bounded_descriptor_lifecycle();
    test_config_validation();
    return 0;
}
