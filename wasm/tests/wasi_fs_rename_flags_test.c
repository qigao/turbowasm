#include "tinytest.h"
#include "wasi_fs_private.h"
#include <string.h>

static turbowasm_wasi_fs fs;
static unsigned calls;
static uint32_t provider_error;
static uint16_t applied_flags;
static uint32_t close_file(void *ctx, turbowasm_wasi_fs_file f) { (void)ctx; (void)f; return 0; }
static uint32_t read_file(void *ctx, turbowasm_wasi_fs_file f,
    const turbowasm_wasi_buffer *b, size_t n, uint32_t *out) {
    (void)ctx; (void)f; (void)b; (void)n; *out = 0; return 0;
}
static uint32_t write_file(void *ctx, turbowasm_wasi_fs_file f,
    const turbowasm_wasi_const_buffer *b, size_t n, uint32_t *out) {
    (void)ctx; (void)f; (void)b; (void)n; *out = 0; return 0;
}
static uint32_t rename_file(void *ctx, turbowasm_wasi_fs_file a, const uint8_t *ap, size_t an,
    turbowasm_wasi_fs_file b, const uint8_t *bp, size_t bn) {
    (void)ctx; (void)a; (void)b;
    ++calls;
    if (an != 3 || bn != 3 || memcmp(ap, "old", 3) || memcmp(bp, "new", 3))
        return TURBOWASM_WASI_ERRNO_INVAL;
    return provider_error;
}
static uint32_t set_flags(void *ctx, turbowasm_wasi_fs_file f, uint16_t flags) {
    (void)ctx; (void)f; ++calls;
    if (provider_error) return provider_error;
    applied_flags = flags; return 0;
}
static void bind(uint32_t fd, uint64_t rights) {
    turbowasm_wasi_fs_descriptor descriptor;
    check_equal(turbowasm_wasi_fs_bind_descriptor_with_rights(&fs, fd,
        (turbowasm_wasi_fs_file){fd + 1, 1}, false, NULL, rights, 0, &descriptor), TURBOWASM_OK);
}
static uint32_t rename_paths(uint32_t to) {
    return turbowasm_wasi_fs_path_rename(&fs, 3, (const uint8_t *)"old", 3,
        to, (const uint8_t *)"new", 3);
}

spec("filesystem rename admission and flag commits") {
    before_each() {
        fs = (turbowasm_wasi_fs){0}; calls = 0; provider_error = 0; applied_flags = 0;
        turbowasm_wasi_fs_config config = {0};
        config.descriptor_capacity = 4;
        config.provider.close = close_file; config.provider.read = read_file; config.provider.write = write_file;
        config.provider.path_rename = rename_file; config.provider.set_flags = set_flags;
        check_equal(turbowasm_wasi_fs_init(&fs, &config), TURBOWASM_OK);
    }
    after_each() {
        for (uint32_t fd = 3; fd < 7; ++fd) (void)turbowasm_wasi_fs_close_fd(&fs, fd);
        check_equal(turbowasm_wasi_fs_destroy(&fs), TURBOWASM_OK);
    }
    it("requires source and target rights before invoking the provider") {
        bind(3, TURBOWASM_WASI_RIGHT_PATH_RENAME_SOURCE);
        bind(4, 0);
        check_equal(rename_paths(4), TURBOWASM_WASI_ERRNO_NOTCAPABLE);
        check_equal(rename_paths(5), TURBOWASM_WASI_ERRNO_BADF);
        check_equal(calls, 0u);
        check_equal(turbowasm_wasi_fs_close_fd(&fs, 4), 0u);
        bind(4, TURBOWASM_WASI_RIGHT_PATH_RENAME_TARGET);
        check_equal(rename_paths(4), 0u); check_equal(calls, 1u);
        provider_error = TURBOWASM_WASI_ERRNO_XDEV;
        check_equal(rename_paths(4), TURBOWASM_WASI_ERRNO_XDEV);
        check_equal(tw_wasi_fd_set_rights(&fs, 3, 0, 0), 0u);
        check_equal(rename_paths(4), TURBOWASM_WASI_ERRNO_NOTCAPABLE);
        check_equal(calls, 2u);
    }
    it("commits flags only after provider success and preserves them on error") {
        bind(3, TURBOWASM_WASI_RIGHT_FD_FDSTAT_SET_FLAGS);
        check_equal(tw_wasi_fd_set_flags(&fs, 3, 1), 0u);
        check_equal(applied_flags, 1u);
        tw_wasi_fd_lease lease = {0};
        check_equal(tw_wasi_fd_acquire(&fs, 3, 0, &lease), 0u);
        check_equal(lease.flags, 1u);
        tw_wasi_fd_release(&fs, &lease);
        provider_error = TURBOWASM_WASI_ERRNO_NOTSUP;
        check_equal(tw_wasi_fd_set_flags(&fs, 3, 0), TURBOWASM_WASI_ERRNO_NOTSUP);
        check_equal(tw_wasi_fd_acquire(&fs, 3, 0, &lease), 0u);
        check_equal(lease.flags, 1u); check_equal(applied_flags, 1u);
        tw_wasi_fd_release(&fs, &lease);
        provider_error = 0;
        check_equal(tw_wasi_fd_set_flags(&fs, 3, 0), 0u);
        check_equal(applied_flags, 0u);
    }
}
