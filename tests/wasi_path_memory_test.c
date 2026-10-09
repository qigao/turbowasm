#include <tinytest.h>
#include <turbowasm/wasi_sockets.h>
#include <string.h>
#include "fixtures/wasi_path_memory.h"

enum { OPEN, STAT, MKDIR, RMDIR, UNLINK, RENAME, RANDOM, FDSTAT };
enum { PAYLOAD_LIMIT = 1024 * 1024, INITIAL_BYTES = 33 * 65536 };
static turbowasm_module owner_module, guest_module;
static turbowasm_instance owner, guest;
static turbowasm_linker linker;
static turbowasm_wasi_fs fs;
static turbowasm_wasi_preview1 wasi;
static struct {
    bool shared, grow, mutate, embedded_nul;
    uint32_t source, target, source_length, target_length, error, calls, closes, pages;
} probe;
static turbowasm_name name(const char *s) {
    return (turbowasm_name){(const uint8_t *)s, (uint32_t)strlen(s)};
}
static turbowasm_value i32(uint32_t n) {
    return (turbowasm_value){.kind = TURBOWASM_VALUE_I32, .as.i32 = (int32_t)n};
}
static turbowasm_value i64(uint64_t n) {
    return (turbowasm_value){.kind = TURBOWASM_VALUE_I64, .as.i64 = (int64_t)n};
}
static uint32_t invoke(turbowasm_instance *instance, unsigned fn,
    const turbowasm_value *args, size_t argc, bool returns_value) {
    turbowasm_value result = {0}; size_t count = 0;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    check_equal(turbowasm_instance_invoke(instance, fn, args, argc, &result, 1, &count, &trap), TURBOWASM_OK);
    check_equal(trap, TURBOWASM_TRAP_NONE);
    check_equal(count, returns_value ? (size_t)1 : (size_t)0);
    return (uint32_t)result.as.i32;
}
static void fill(uint32_t address, uint32_t length, uint8_t value) {
    turbowasm_value args[] = {i32(address), i32(value), i32(length)};
    invoke(&owner, 1, args, 3, false);
}
static uint8_t read_byte(uint32_t address) {
    turbowasm_value arg = i32(address);
    return (uint8_t)invoke(&owner, 2, &arg, 1, true);
}
static uint64_t read_integer(uint32_t address, unsigned bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint64_t)read_byte(address + i) << (8 * i);
    return value;
}
static void effect(void) {
    ++probe.calls;
    if (probe.mutate) {
        fill(probe.source, probe.source_length, 'm');
        if (probe.target_length) fill(probe.target, probe.target_length, 'n');
    }
    if (probe.grow) {
        turbowasm_value one = i32(1);
        check_equal(invoke(&owner, 0, &one, 1, true), probe.pages);
        ++probe.pages;
    }
}
static void check_path(const uint8_t *path, size_t length, bool target) {
    check_equal(length, (size_t)(target ? probe.target_length : probe.source_length));
    bool same = true;
    for (size_t i = 0; i < length; ++i) {
        uint8_t expected = probe.embedded_nul && i == 1 ? 0 : target ? 'z' : 'a';
        if (path[i] != expected) { same = false; break; }
    }
    check(same);
}
static void paths_effect(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    check_path(a, an, false);
    if (b) check_path(b, bn, true);
    effect();
    /* Shared copies must remain stable even when the original bytes change and
     * memory moves while a provider still uses the borrowed host arguments. */
    if (probe.shared) {
        check_path(a, an, false);
        if (b) check_path(b, bn, true);
    }
}
static uint32_t close_file(void *context, turbowasm_wasi_fs_file file) {
    (void)context; (void)file; ++probe.closes; return 0;
}
static uint32_t read_file(void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers, size_t count, uint32_t *out) {
    (void)context; (void)file; (void)buffers; (void)count; *out = 0; return TURBOWASM_WASI_ERRNO_NOSYS;
}
static uint32_t write_file(void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers, size_t count, uint32_t *out) {
    (void)context; (void)file; (void)buffers; (void)count; *out = 0; return TURBOWASM_WASI_ERRNO_NOSYS;
}
static uint32_t open_file(void *context, turbowasm_wasi_fs_file dir, uint32_t flags,
    const uint8_t *path, size_t length, uint32_t oflags, uint64_t base,
    uint64_t inheriting, uint32_t fdflags, turbowasm_wasi_fs_file *out) {
    (void)context;
    check_equal(dir.object, UINT64_C(3)); check_equal(flags | oflags | fdflags, 0u);
    check_equal(base, (uint64_t)TURBOWASM_WASI_RIGHT_FD_READ); check_equal(inheriting, UINT64_C(0));
    paths_effect(path, length, NULL, 0);
    if (!probe.error) *out = (turbowasm_wasi_fs_file){9, 1};
    return probe.error;
}
static void fill_stat(turbowasm_wasi_fs_stat *out) {
    *out = (turbowasm_wasi_fs_stat){.device = 11, .inode = 22,
        .file_type = TURBOWASM_WASI_FILETYPE_REGULAR_FILE, .link_count = 3,
        .size = 44, .accessed_ns = 100, .modified_ns = 200, .changed_ns = 300};
}
static uint32_t path_stat(void *context, turbowasm_wasi_fs_file dir, uint32_t flags,
    const uint8_t *path, size_t length, turbowasm_wasi_fs_stat *out) {
    (void)context; check_equal(dir.object, UINT64_C(3)); check_equal(flags, 0u);
    paths_effect(path, length, NULL, 0); fill_stat(out); return probe.error;
}
static uint32_t stat_file(void *context, turbowasm_wasi_fs_file file, turbowasm_wasi_fs_stat *out) {
    (void)context; check_equal(file.object, UINT64_C(4));
    effect(); fill_stat(out); return probe.error;
}
static uint32_t mutation(void *context, turbowasm_wasi_fs_file dir, const uint8_t *path, size_t length) {
    (void)context; check_equal(dir.object, UINT64_C(3));
    paths_effect(path, length, NULL, 0); return probe.error;
}
static uint32_t rename_file(void *context, turbowasm_wasi_fs_file from, const uint8_t *a, size_t an,
    turbowasm_wasi_fs_file to, const uint8_t *b, size_t bn) {
    (void)context; check_equal(from.object, UINT64_C(3)); check_equal(to.object, UINT64_C(3));
    paths_effect(a, an, b, bn); return probe.error;
}
static uint32_t random_fill(void *context, uint8_t *bytes, size_t length) {
    (void)context; check(bytes != NULL); check_equal(length, (size_t)probe.source_length);
    effect(); memset(bytes, 0x7e, length); return probe.error;
}
static void setup(bool shared) {
    probe.shared = shared; probe.pages = 33;
    const uint8_t *memory = shared ? wasi_path_memory_owner_shared : wasi_path_memory_owner_unshared;
    const uint8_t *program = shared ? wasi_path_memory_shared : wasi_path_memory_unshared;
    check_equal(turbowasm_module_load_borrowed(&owner_module, memory,
        shared ? sizeof(wasi_path_memory_owner_shared) : sizeof(wasi_path_memory_owner_unshared)), TURBOWASM_OK);
    check_equal(turbowasm_instance_create(&owner, &owner_module), TURBOWASM_OK);
    check_equal(turbowasm_module_load_borrowed(&guest_module, program,
        shared ? sizeof(wasi_path_memory_shared) : sizeof(wasi_path_memory_unshared)), TURBOWASM_OK);
    turbowasm_wasi_fs_config config = {.descriptor_capacity = 3,
        .provider = {.close = close_file, .read = read_file, .write = write_file, .stat = stat_file,
            .path_open = open_file, .path_stat = path_stat, .path_create_directory = mutation,
            .path_remove_directory = mutation, .path_unlink_file = mutation, .path_rename = rename_file}};
    check_equal(turbowasm_wasi_fs_init(&fs, &config), TURBOWASM_OK);
    turbowasm_wasi_fs_descriptor descriptor;
    check_equal(turbowasm_wasi_fs_bind_descriptor(&fs, 3, (turbowasm_wasi_fs_file){3, 1}, true, "/", &descriptor), TURBOWASM_OK);
    check_equal(turbowasm_wasi_fs_bind_descriptor(&fs, 4, (turbowasm_wasi_fs_file){4, 1}, false, NULL, &descriptor), TURBOWASM_OK);
    turbowasm_wasi_preview1_config_v2 v2;
    turbowasm_wasi_preview1_config_v2_init(&v2);
    v2.base.allow_filesystem = v2.base.allow_random = true;
    v2.base.filesystem = &fs; v2.base.random_fill = random_fill;
    check_equal(turbowasm_wasi_preview1_init_v2(&wasi, &v2), TURBOWASM_OK);
    check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
    check_equal(turbowasm_linker_define_instance(&linker, name("p"), &owner), TURBOWASM_OK);
    check_equal(turbowasm_wasi_preview1_define(&wasi, &linker), TURBOWASM_OK);
    check_equal(turbowasm_instance_create_linked(&guest, &guest_module, &linker), TURBOWASM_OK);
}
static uint32_t path_call(unsigned fn, uint32_t out) {
    turbowasm_value open_args[] = {i32(3), i32(0), i32(probe.source), i32(probe.source_length),
        i32(0), i64(TURBOWASM_WASI_RIGHT_FD_READ), i64(0), i32(0), i32(out)};
    turbowasm_value stat_args[] = {i32(3), i32(0), i32(probe.source), i32(probe.source_length), i32(out)};
    turbowasm_value other_args[] = {i32(3), i32(probe.source), i32(probe.source_length),
        i32(3), i32(probe.target), i32(probe.target_length)};
    return invoke(&guest, fn, fn == OPEN ? open_args : fn == STAT ? stat_args : other_args,
        fn == OPEN ? 9 : fn == STAT ? 5 : fn == RENAME ? 6 : 3, true);
}
static void prepare_paths(uint32_t length, uint32_t target_length) {
    probe.source = 128; probe.target = PAYLOAD_LIMIT + 256;
    probe.source_length = length; probe.target_length = target_length;
    fill(probe.source, length, 'a');
    fill(probe.target, target_length, 'z');
    if (probe.embedded_nul) {
        if (length > 1) fill(probe.source + 1, 1, 0);
        if (target_length > 1) fill(probe.target + 1, 1, 0);
    }
}
static void check_stat(uint32_t out) {
    check_equal(read_integer(out, 8), UINT64_C(11)); check_equal(read_integer(out + 8, 8), UINT64_C(22));
    check_equal(read_byte(out + 16), (uint8_t)TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
    for (unsigned i = 17; i < 24; ++i) check_equal(read_byte(out + i), 0u);
    check_equal(read_integer(out + 24, 8), UINT64_C(3)); check_equal(read_integer(out + 32, 8), UINT64_C(44));
    check_equal(read_integer(out + 40, 8), UINT64_C(100)); check_equal(read_integer(out + 48, 8), UINT64_C(200));
    check_equal(read_integer(out + 56, 8), UINT64_C(300));
}
static void close_opened(void) {
    turbowasm_wasi_fs_descriptor_info info;
    check(turbowasm_wasi_fs_descriptor_info_get(&fs, 5, &info));
    check_equal(info.file.object, UINT64_C(9));
    check_equal(info.rights_base, (uint64_t)TURBOWASM_WASI_RIGHT_FD_READ);
    check_equal(turbowasm_wasi_fs_close_fd(&fs, 5), 0u);
}
static void path_success(bool shared, bool grow) {
    setup(shared); probe.grow = grow; probe.mutate = shared; probe.embedded_nul = true;
    for (unsigned fn = OPEN; fn <= RENAME; ++fn) {
        prepare_paths(3, fn == RENAME ? 3 : 0);
        uint32_t out = probe.pages * 65536u - (fn == OPEN ? 4 : 64);
        check_equal(path_call(fn, out), 0u);
        if (fn == OPEN) { check_equal(read_integer(out, 4), UINT64_C(5)); close_opened(); }
        if (fn == STAT) check_stat(out);
    }
    check_equal(probe.calls, 6u); check_equal(probe.pages, grow ? 39u : 33u);
}
static void path_errors(bool shared) {
    setup(shared);
    for (unsigned fn = OPEN; fn <= RENAME; ++fn) {
        prepare_paths(3, fn == RENAME ? 3 : 0);
        uint32_t calls = probe.calls;
        probe.source = INITIAL_BYTES - 2;
        check_equal(path_call(fn, 32), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
        probe.source = UINT32_MAX;
        check_equal(path_call(fn, 32), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
        prepare_paths(3, fn == RENAME ? 3 : 0);
        if (fn <= STAT) check_equal(path_call(fn, INITIAL_BYTES - (fn == OPEN ? 3 : 63)), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
        if (fn == RENAME) {
            probe.target = INITIAL_BYTES - 2;
            check_equal(path_call(fn, 32), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
            prepare_paths(3, 3);
        }
        check_equal(probe.calls, calls);
        fill(32, 64, 0xa5); probe.error = TURBOWASM_WASI_ERRNO_IO;
        check_equal(path_call(fn, 32), probe.error);
        check_equal(probe.calls, calls + 1);
        for (unsigned i = 0; i < 64; ++i) check_equal(read_byte(32 + i), (uint8_t)0xa5);
        turbowasm_wasi_fs_descriptor_info info;
        check(!turbowasm_wasi_fs_descriptor_info_get(&fs, 5, &info));
        check_equal(probe.closes, 0u); probe.error = 0;
    }
}
static uint32_t random_call(uint32_t address, uint32_t length) {
    probe.source_length = length;
    turbowasm_value args[] = {i32(address), i32(length)};
    return invoke(&guest, RANDOM, args, 2, true);
}
static void random_cases(bool shared) {
    setup(shared);
    uint32_t lengths[] = {0, 1, 257, PAYLOAD_LIMIT, PAYLOAD_LIMIT + 1};
    for (unsigned i = 0; i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
        uint32_t length = lengths[i], calls = probe.calls;
        fill(128, length + 1, 0xa5);
        uint32_t error = shared && length > PAYLOAD_LIMIT ? TURBOWASM_WASI_ERRNO_NOMEM : 0;
        check_equal(random_call(128, length), error);
        check_equal(probe.calls, calls + (error ? 0 : 1));
        check_equal(read_byte(128 + length), (uint8_t)0xa5);
        if (length) {
            check_equal(read_byte(128), error ? (uint8_t)0xa5 : (uint8_t)0x7e);
            check_equal(read_byte(127 + length), error ? (uint8_t)0xa5 : (uint8_t)0x7e);
        }
    }
    uint32_t calls = probe.calls;
    check_equal(random_call(INITIAL_BYTES, 0), 0u);
    check_equal(probe.calls, calls + 1);
    check_equal(random_call(INITIAL_BYTES + 1, 0), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
    check_equal(random_call(INITIAL_BYTES, 1), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
    check_equal(random_call(UINT32_MAX, PAYLOAD_LIMIT + 1), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
    check_equal(probe.calls, calls + 1);
    fill(128, 33, 0xa5); probe.error = TURBOWASM_WASI_ERRNO_IO;
    check_equal(random_call(128, 32), probe.error);
    for (unsigned i = 0; i < 32; ++i) check_equal(read_byte(128 + i), shared ? (uint8_t)0xa5 : (uint8_t)0x7e);
    check_equal(read_byte(160), (uint8_t)0xa5);
}
static void fdstat_cases(bool shared) {
    setup(shared);
    turbowasm_value args[] = {i32(4), i32(INITIAL_BYTES - 23)};
    check_equal(invoke(&guest, FDSTAT, args, 2, true), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
    check_equal(probe.calls, 0u);
    args[1] = i32(INITIAL_BYTES - 24); fill(INITIAL_BYTES - 24, 24, 0xa5);
    probe.error = TURBOWASM_WASI_ERRNO_IO;
    check_equal(invoke(&guest, FDSTAT, args, 2, true), probe.error);
    for (unsigned i = 0; i < 24; ++i) check_equal(read_byte(INITIAL_BYTES - 24 + i), (uint8_t)0xa5);
    probe.error = 0; probe.grow = true;
    check_equal(invoke(&guest, FDSTAT, args, 2, true), 0u);
    check_equal(read_byte(INITIAL_BYTES - 24), (uint8_t)TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
    for (unsigned i = 1; i < 8; ++i) check_equal(read_byte(INITIAL_BYTES - 24 + i), 0u);
    check_equal(read_integer(INITIAL_BYTES - 16, 8), UINT64_MAX);
    check_equal(read_integer(INITIAL_BYTES - 8, 8), UINT64_MAX);
    check_equal(probe.pages, 34u); check_equal(probe.calls, 2u);
}

spec("protected Preview1 path, random and fdstat projections") {
    before_each() { memset(&probe, 0, sizeof(probe)); }
    after_each() {
        turbowasm_instance_destroy(&guest); turbowasm_linker_destroy(&linker);
        turbowasm_wasi_preview1_destroy(&wasi);
        for (uint32_t fd = 3; fd <= 5; ++fd) (void)turbowasm_wasi_fs_close_fd(&fs, fd);
        check_equal(turbowasm_wasi_fs_destroy(&fs), TURBOWASM_OK);
        turbowasm_module_destroy(&guest_module); turbowasm_instance_destroy(&owner);
        turbowasm_module_destroy(&owner_module);
    }
    it("snapshots every shared path including embedded NULs before provider mutation") { path_success(true, false); }
    it("preserves unshared path arguments and output ABI") { path_success(false, false); }
    it("keeps shared paths and output publication valid across provider memory growth") { path_success(true, true); }
    it("rechecks unshared path outputs after provider memory growth") { path_success(false, true); }
    it("rejects shared invalid ranges before effects and preserves failed outputs") { path_errors(true); }
    it("preserves unshared path errors and failed-open ownership") { path_errors(false); }
    it("applies the shared path budget before effects including aggregate rename bytes") {
        setup(true);
        for (unsigned fn = OPEN; fn <= UNLINK; ++fn) {
            prepare_paths(PAYLOAD_LIMIT, 0); check_equal(path_call(fn, 32), 0u);
            if (fn == OPEN) close_opened();
            uint32_t calls = probe.calls;
            prepare_paths(PAYLOAD_LIMIT + 1, 0);
            check_equal(path_call(fn, 32), (uint32_t)TURBOWASM_WASI_ERRNO_NOMEM);
            check_equal(probe.calls, calls);
            if (fn <= STAT) check_equal(path_call(fn, UINT32_MAX), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
        }
        prepare_paths(PAYLOAD_LIMIT/2, PAYLOAD_LIMIT/2); check_equal(path_call(RENAME, 32), 0u);
        uint32_t calls = probe.calls;
        prepare_paths(PAYLOAD_LIMIT/2, PAYLOAD_LIMIT/2 + 1);
        check_equal(path_call(RENAME, 32), (uint32_t)TURBOWASM_WASI_ERRNO_NOMEM);
        check_equal(probe.calls, calls);
        probe.target = UINT32_MAX;
        check_equal(path_call(RENAME, 32), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
    }
    it("keeps existing unshared path capacity above the shared snapshot bound") {
        setup(false);
        for (unsigned fn = OPEN; fn <= RENAME; ++fn) {
            prepare_paths(PAYLOAD_LIMIT + 1, fn == RENAME ? 3 : 0);
            check_equal(path_call(fn, 32), 0u); if (fn == OPEN) close_opened();
        }
    }
    it("preserves zero-length paths and supports overlapping path/output ranges") {
        setup(true);
        for (unsigned fn = OPEN; fn <= UNLINK; ++fn) {
            probe.source = INITIAL_BYTES; probe.source_length = 0;
            check_equal(path_call(fn, 32), 0u); if (fn == OPEN) close_opened();
            uint32_t calls = probe.calls; probe.source = INITIAL_BYTES + 1;
            check_equal(path_call(fn, 32), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
            check_equal(probe.calls, calls);
        }
        prepare_paths(3, 0); check_equal(path_call(OPEN, probe.source), 0u);
        check_equal(read_integer(probe.source, 4), UINT64_C(5)); close_opened();
        prepare_paths(3, 0); check_equal(path_call(STAT, probe.source), 0u); check_stat(probe.source);
    }
    it("bounds shared random requests and writes back only successful provider results") { random_cases(true); }
    it("preserves unshared random capacity and provider-written error bytes") { random_cases(false); }
    it("publishes shared random bytes after growth without holding the Runtime memory lock") {
        setup(true); probe.grow = true;
        check_equal(random_call(INITIAL_BYTES - 32, 32), 0u);
        check_equal(probe.pages, 34u); check_equal(probe.calls, 1u);
        for (unsigned i = 0; i < 32; ++i) check_equal(read_byte(INITIAL_BYTES - 32 + i), (uint8_t)0x7e);
    }
    it("copies shared fdstat metadata through provider errors and memory growth") { fdstat_cases(true); }
    it("preserves unshared fdstat layout and rechecks its output after growth") { fdstat_cases(false); }
}
