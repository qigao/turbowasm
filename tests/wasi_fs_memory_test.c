#include <tinytest.h>
#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi_fs.h>
#include <string.h>
#include "fixtures/wasi_fs_memory.h"

enum { SEEK, TELL, STAT, PRESTAT, PRENAME, READ8, WRITE8 };
static turbowasm_module memory_module, guest_module;
static turbowasm_instance memory_owner, guest;
static turbowasm_linker linker;
static turbowasm_wasi_fs filesystem;
static turbowasm_wasi_preview1 wasi;
static struct { unsigned calls, pages; uint32_t error; bool grow; } probe;

/* Export one bounded memory and a grow-by-one-page function. The memory flags
 * select shared/unshared without changing the tested Preview1 program. */
static uint8_t memory_bytes[] = {
    0,97,115,109,1,0,0,0,
    1,5,1,0x60,0,1,0x7f, 3,2,1,0,
    5,4,1,1,1,4, 7,10,1,6,'m','e','m','o','r','y',2,0,
    10,8,1,6,0,0x41,1,0x40,0,0x0b
};
static turbowasm_name name(const char *s) {
    return (turbowasm_name){(const uint8_t *)s, (uint32_t)strlen(s)};
}
static turbowasm_value i32(uint32_t x) {
    return (turbowasm_value){.kind = TURBOWASM_VALUE_I32, .as.i32 = (int32_t)x};
}
static turbowasm_value i64(int64_t x) {
    return (turbowasm_value){.kind = TURBOWASM_VALUE_I64, .as.i64 = x};
}
static uint32_t invoke(turbowasm_instance *instance, uint32_t fn,
    const turbowasm_value *args, size_t argc, bool returns_value) {
    turbowasm_value result = {0};
    size_t count = 0;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    check_equal(turbowasm_instance_invoke(instance, fn, args, argc,
        &result, 1, &count, &trap), TURBOWASM_OK);
    check_equal(trap, TURBOWASM_TRAP_NONE);
    check_equal(count, returns_value ? (size_t)1 : (size_t)0);
    if (returns_value) check_equal(result.kind, TURBOWASM_VALUE_I32);
    return (uint32_t)result.as.i32;
}
static uint32_t operation(unsigned fn, uint32_t address) {
    turbowasm_value args[] = {i32(4), i32(address)};
    turbowasm_value seek_args[] = {i32(4), i64(-9), i32(TURBOWASM_WASI_WHENCE_END), i32(address)};
    return invoke(&guest, fn, fn == SEEK ? seek_args : args, fn == SEEK ? 4 : 2, true);
}
static uint32_t preopen(unsigned fn, uint32_t fd, uint32_t address, uint32_t length) {
    turbowasm_value args[] = {i32(fd), i32(address), i32(length)};
    return invoke(&guest, fn, args, fn == PRESTAT ? 2 : 3, true);
}
static uint8_t read_byte(uint32_t address) {
    turbowasm_value arg = i32(address);
    return (uint8_t)invoke(&guest, READ8, &arg, 1, true);
}
static void fill(uint32_t address, size_t length, uint8_t byte) {
    for (size_t i = 0; i < length; ++i) {
        turbowasm_value args[] = {i32(address + (uint32_t)i), i32(byte)};
        invoke(&guest, WRITE8, args, 2, false);
    }
}
static uint64_t read_u64(uint32_t address) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) result |= (uint64_t)read_byte(address + i) << (8 * i);
    return result;
}
static void provider_effect(turbowasm_wasi_fs_file file) {
    check_equal(file.object, UINT64_C(4));
    ++probe.calls;
    if (probe.grow) {
        /* A second instance owns the same memory. Growing it inside the
         * provider proves that no memory lock or raw output span is retained. */
        check_equal(invoke(&memory_owner, 0, NULL, 0, true), probe.pages);
        ++probe.pages;
    }
}
static uint32_t close_file(void *context, turbowasm_wasi_fs_file file) {
    (void)context; (void)file;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}
static uint32_t read_file(void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers, size_t count, uint32_t *read) {
    (void)context; (void)file; (void)buffers; (void)count;
    *read = 0;
    return TURBOWASM_WASI_ERRNO_NOSYS;
}
static uint32_t write_file(void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers, size_t count, uint32_t *written) {
    (void)context; (void)file; (void)buffers; (void)count;
    *written = 0;
    return TURBOWASM_WASI_ERRNO_NOSYS;
}
static uint32_t seek_file(void *context, turbowasm_wasi_fs_file file,
    int64_t offset, uint8_t whence, uint64_t *out) {
    (void)context;
    provider_effect(file);
    check_equal(offset, INT64_C(-9));
    check_equal(whence, (uint8_t)TURBOWASM_WASI_WHENCE_END);
    *out = UINT64_C(0x1122334455667788);
    return probe.error;
}
static uint32_t tell_file(void *context, turbowasm_wasi_fs_file file, uint64_t *out) {
    (void)context;
    provider_effect(file);
    *out = UINT64_C(0x8877665544332211);
    return probe.error;
}
static uint32_t stat_file(void *context, turbowasm_wasi_fs_file file, turbowasm_wasi_fs_stat *out) {
    (void)context;
    provider_effect(file);
    *out = (turbowasm_wasi_fs_stat){
        .device = 11, .inode = 22, .file_type = TURBOWASM_WASI_FILETYPE_REGULAR_FILE,
        .link_count = 3, .size = UINT64_C(0x1020304050607080),
        .accessed_ns = 100, .modified_ns = 200, .changed_ns = 300
    };
    return probe.error;
}
static void setup(bool shared) {
    memset(&probe, 0, sizeof(probe));
    probe.pages = 1;
    memory_bytes[22] = shared ? 3 : 1;
    const uint8_t *program = shared ? wasi_fs_memory_shared : wasi_fs_memory_unshared;
    size_t length = shared ? sizeof(wasi_fs_memory_shared) : sizeof(wasi_fs_memory_unshared);
    check_equal(turbowasm_module_load_borrowed(&memory_module, memory_bytes, sizeof(memory_bytes)), TURBOWASM_OK);
    check_equal(turbowasm_instance_create(&memory_owner, &memory_module), TURBOWASM_OK);
    check_equal(turbowasm_module_load_borrowed(&guest_module, program, length), TURBOWASM_OK);
    turbowasm_wasi_fs_config fs = {.descriptor_capacity = 2,
        .provider = {.close = close_file, .read = read_file, .write = write_file,
            .seek = seek_file, .tell = tell_file, .stat = stat_file}};
    check_equal(turbowasm_wasi_fs_init(&filesystem, &fs), TURBOWASM_OK);
    turbowasm_wasi_fs_descriptor descriptor = {0};
    check_equal(turbowasm_wasi_fs_bind_descriptor(&filesystem, 3,
        (turbowasm_wasi_fs_file){3, 1}, true, "/sandbox", &descriptor), TURBOWASM_OK);
    check_equal(turbowasm_wasi_fs_bind_descriptor(&filesystem, 4,
        (turbowasm_wasi_fs_file){4, 1}, false, NULL, &descriptor), TURBOWASM_OK);
    turbowasm_wasi_preview1_config config = {.allow_filesystem = true, .filesystem = &filesystem};
    check_equal(turbowasm_wasi_preview1_init(&wasi, &config), TURBOWASM_OK);
    check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
    check_equal(turbowasm_linker_define_instance(&linker, name("p"), &memory_owner), TURBOWASM_OK);
    check_equal(turbowasm_wasi_preview1_define(&wasi, &linker), TURBOWASM_OK);
    check_equal(turbowasm_instance_create_linked(&guest, &guest_module, &linker), TURBOWASM_OK);
}
static void outputs(bool shared, bool grow) {
    setup(shared);
    probe.grow = grow;
    for (unsigned fn = SEEK; fn <= STAT; ++fn) {
        uint32_t size = fn == STAT ? 64 : 8;
        uint32_t address = probe.pages * 65536u - size;
        fill(address, size, 0xa5);
        check_equal(operation(fn, address), (uint32_t)TURBOWASM_WASI_ERRNO_SUCCESS);
        if (fn == SEEK) check_equal(read_u64(address), UINT64_C(0x1122334455667788));
        else if (fn == TELL) check_equal(read_u64(address), UINT64_C(0x8877665544332211));
        else {
            check_equal(read_u64(address), UINT64_C(11));
            check_equal(read_u64(address + 8), UINT64_C(22));
            check_equal(read_byte(address + 16), (uint8_t)TURBOWASM_WASI_FILETYPE_REGULAR_FILE);
            for (unsigned i = 17; i < 24; ++i) check_equal(read_byte(address + i), (uint8_t)0);
            check_equal(read_u64(address + 24), UINT64_C(3));
            check_equal(read_u64(address + 32), UINT64_C(0x1020304050607080));
            check_equal(read_u64(address + 40), UINT64_C(100));
            check_equal(read_u64(address + 48), UINT64_C(200));
            check_equal(read_u64(address + 56), UINT64_C(300));
        }
    }
    check_equal(probe.calls, 3u);
    check_equal(probe.pages, grow ? 4u : 1u);
    fill(16, 12, 0xa5);
    check_equal(preopen(PRESTAT, 3, 16, 0), (uint32_t)TURBOWASM_WASI_ERRNO_SUCCESS);
    check_equal(read_u64(16), UINT64_C(8) << 32);
    check_equal(preopen(PRENAME, 3, 16, 12), (uint32_t)TURBOWASM_WASI_ERRNO_SUCCESS);
    for (unsigned i = 0; i < 8; ++i) check_equal(read_byte(16 + i), (uint8_t)"/sandbox"[i]);
    for (unsigned i = 8; i < 12; ++i) check_equal(read_byte(16 + i), (uint8_t)0xa5);
}
static void errors(bool shared) {
    setup(shared);
    for (unsigned fn = SEEK; fn <= STAT; ++fn) {
        uint32_t size = fn == STAT ? 64 : 8;
        unsigned calls = probe.calls;
        check_equal(operation(fn, 65536u - size + 1u), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
        check_equal(operation(fn, UINT32_MAX), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
        check_equal(probe.calls, calls);
        fill(32, size, 0xa5);
        probe.error = TURBOWASM_WASI_ERRNO_IO;
        check_equal(operation(fn, 32), probe.error);
        check_equal(probe.calls, calls + 1);
        for (unsigned i = 0; i < size; ++i) check_equal(read_byte(32 + i), (uint8_t)0xa5);
    }
    fill(65528, 8, 0xa5);
    check_equal(preopen(PRESTAT, 3, 65529, 0), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
    check_equal(preopen(PRENAME, 3, 65528, 9), (uint32_t)TURBOWASM_WASI_ERRNO_FAULT);
    check_equal(preopen(PRENAME, 3, 65528, 7), (uint32_t)TURBOWASM_WASI_ERRNO_NAMETOOLONG);
    check_equal(preopen(PRENAME, 3, UINT32_MAX, 0), (uint32_t)TURBOWASM_WASI_ERRNO_NAMETOOLONG);
    check_equal(preopen(PRESTAT, 4, UINT32_MAX, 0), (uint32_t)TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    check_equal(preopen(PRESTAT, 99, UINT32_MAX, 0), (uint32_t)TURBOWASM_WASI_ERRNO_BADF);
    for (unsigned i = 0; i < 8; ++i) check_equal(read_byte(65528 + i), (uint8_t)0xa5);
}

spec("Preview1 protected filesystem output") {
    after_each() {
        turbowasm_instance_destroy(&guest);
        turbowasm_linker_destroy(&linker);
        turbowasm_wasi_preview1_destroy(&wasi);
        if (filesystem.impl) {
            turbowasm_wasi_fs_close_fd(&filesystem, 4);
            turbowasm_wasi_fs_close_fd(&filesystem, 3);
            check_equal(turbowasm_wasi_fs_destroy(&filesystem), TURBOWASM_OK);
        }
        turbowasm_module_destroy(&guest_module);
        turbowasm_instance_destroy(&memory_owner);
        turbowasm_module_destroy(&memory_module);
    }
    it("encodes fixed metadata and preopen names in shared memory") { outputs(true, false); }
    it("preserves unshared metadata layouts") { outputs(false, false); }
    it("rejects invalid shared outputs before effects and preserves bytes on failure") { errors(true); }
    it("preserves unshared error precedence and bytes on failure") { errors(false); }
    it("publishes to shared memory after growth inside each provider callback") { outputs(true, true); }
    it("rechecks unshared storage after growth inside each provider callback") { outputs(false, true); }
}
