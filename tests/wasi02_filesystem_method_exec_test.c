#include "../src/wasi02_filesystem.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct bytebuf {
    uint8_t data[4096];
    size_t size;
} bytebuf;

static void put_u8(bytebuf *b, uint8_t v) {
    assert(b != NULL && b->size < sizeof(b->data));
    b->data[b->size++] = v;
}

static void put_bytes(bytebuf *b, const uint8_t *p, size_t n) {
    assert(b != NULL && (n == 0u || p != NULL));
    assert(n <= sizeof(b->data) - b->size);
    if (n != 0u) {
        memcpy(b->data + b->size, p, n);
        b->size += n;
    }
}

static void put_uleb(bytebuf *b, uint32_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7fu);
        v >>= 7u;
        if (v != 0u)
            byte |= 0x80u;
        put_u8(b, byte);
    } while (v != 0u);
}

static void put_name(bytebuf *b, const char *s) {
    size_t n = strlen(s);
    assert(n <= UINT32_MAX);
    put_uleb(b, (uint32_t)n);
    put_bytes(b, (const uint8_t *)s, n);
}

static void put_nameattr(bytebuf *b, const char *s) {
    put_u8(b, 0u);
    put_name(b, s);
}

static void put_section(bytebuf *module, uint8_t id, const bytebuf *payload) {
    assert(payload->size <= UINT32_MAX);
    put_u8(module, id);
    put_uleb(module, (uint32_t)payload->size);
    put_bytes(module, payload->data, payload->size);
}

static void put_core_header(bytebuf *b) {
    static const uint8_t header[] = {
        0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00
    };
    put_bytes(b, header, sizeof(header));
}

static void put_component_header(bytebuf *b) {
    static const uint8_t header[] = {
        0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00
    };
    put_bytes(b, header, sizeof(header));
}

static void build_memory_module(bytebuf *out) {
    bytebuf section = {0};

    put_core_header(out);

    put_uleb(&section, 1u);
    put_u8(&section, 0x00u);
    put_uleb(&section, 1u);
    put_section(out, 5u, &section);

    section.size = 0u;
    put_uleb(&section, 1u);
    put_name(&section, "mem");
    put_u8(&section, 0x02u);
    put_uleb(&section, 0u);
    put_section(out, 7u, &section);
}

static void build_consumer_module(bytebuf *out) {
    bytebuf section = {0};
    bytebuf body = {0};

    put_core_header(out);

    /* type 0: mkdir(i32 handle, i32 ptr, i32 len, i32 outptr) -> () */
    put_uleb(&section, 3u);
    put_u8(&section, 0x60u);
    put_uleb(&section, 4u);
    put_u8(&section, 0x7fu);
    put_u8(&section, 0x7fu);
    put_u8(&section, 0x7fu);
    put_u8(&section, 0x7fu);
    put_uleb(&section, 0u);
    /* type 1: run(i32 handle) -> i32 */
    put_u8(&section, 0x60u);
    put_uleb(&section, 1u);
    put_u8(&section, 0x7fu);
    put_uleb(&section, 1u);
    put_u8(&section, 0x7fu);
    /* type 2: drop(i32 handle) -> () */
    put_u8(&section, 0x60u);
    put_uleb(&section, 1u);
    put_u8(&section, 0x7fu);
    put_uleb(&section, 0u);
    put_section(out, 1u, &section);

    section.size = 0u;
    put_uleb(&section, 3u);
    put_name(&section, "m");
    put_name(&section, "mem");
    put_u8(&section, 0x02u);
    put_u8(&section, 0x00u);
    put_uleb(&section, 1u);
    put_name(&section, "p");
    put_name(&section, "mkdir");
    put_u8(&section, 0x00u);
    put_uleb(&section, 0u);
    put_name(&section, "p");
    put_name(&section, "drop");
    put_u8(&section, 0x00u);
    put_uleb(&section, 2u);
    put_section(out, 2u, &section);

    section.size = 0u;
    put_uleb(&section, 1u);
    put_uleb(&section, 1u);
    put_section(out, 3u, &section);

    section.size = 0u;
    put_uleb(&section, 1u);
    put_name(&section, "run");
    put_u8(&section, 0x00u);
    put_uleb(&section, 2u);
    put_section(out, 7u, &section);

    /*
     * memory[0] = 'x';
     * mkdir(handle, 0, 1, 8);
     * return memory[8] (result discriminant).
     */
    put_uleb(&body, 0u);
    put_u8(&body, 0x41u); put_u8(&body, 0x00u);
    put_u8(&body, 0x41u); put_u8(&body, 0xf8u); put_u8(&body, 0x00u);
    put_u8(&body, 0x3au); put_u8(&body, 0x00u); put_u8(&body, 0x00u);
    put_u8(&body, 0x20u); put_u8(&body, 0x00u);
    put_u8(&body, 0x41u); put_u8(&body, 0x00u);
    put_u8(&body, 0x41u); put_u8(&body, 0x01u);
    put_u8(&body, 0x41u); put_u8(&body, 0x08u);
    put_u8(&body, 0x10u); put_u8(&body, 0x00u);
    /* Return the transient canonical borrow before completing the call. */
    put_u8(&body, 0x20u); put_u8(&body, 0x00u);
    put_u8(&body, 0x10u); put_u8(&body, 0x01u);
    put_u8(&body, 0x41u); put_u8(&body, 0x08u);
    put_u8(&body, 0x2du); put_u8(&body, 0x00u); put_u8(&body, 0x00u);
    put_u8(&body, 0x0bu);

    section.size = 0u;
    put_uleb(&section, 1u);
    put_uleb(&section, (uint32_t)body.size);
    put_bytes(&section, body.data, body.size);
    put_section(out, 10u, &section);
}

static const char *const error_labels[] = {
    "access", "would-block", "already", "bad-descriptor", "busy",
    "deadlock", "quota", "exist", "file-too-large",
    "illegal-byte-sequence", "in-progress", "interrupted", "invalid",
    "io", "is-directory", "loop", "too-many-links", "message-size",
    "name-too-long", "no-device", "no-entry", "no-lock",
    "insufficient-memory", "insufficient-space", "not-directory",
    "not-empty", "not-recoverable", "unsupported", "no-tty",
    "no-such-device", "overflow", "not-permitted", "pipe", "read-only",
    "invalid-seek", "text-file-busy", "cross-device"
};

static void append_fs_types_type_section(bytebuf *component) {
    bytebuf s = {0};
    size_t i;

    put_uleb(&s, 1u);
    put_u8(&s, 0x42u);
    put_uleb(&s, 6u);

    /* export descriptor: type (sub resource), local type 0 */
    put_u8(&s, 0x04u);
    put_nameattr(&s, "descriptor");
    put_u8(&s, 0x03u);
    put_u8(&s, 0x01u);

    /* local type 1 = borrow<descriptor> */
    put_u8(&s, 0x01u);
    put_u8(&s, 0x68u);
    put_uleb(&s, 0u);

    /* local type 2 = error-code enum */
    put_u8(&s, 0x01u);
    put_u8(&s, 0x6du);
    put_uleb(&s, 37u);
    for (i = 0u; i < 37u; ++i)
        put_name(&s, error_labels[i]);

    /* local type 3 = result<_, error-code> */
    put_u8(&s, 0x01u);
    put_u8(&s, 0x6au);
    put_u8(&s, 0x00u);
    put_u8(&s, 0x01u);
    put_uleb(&s, 2u);

    /* local type 4 = (self: borrow<descriptor>, path: string) -> result */
    put_u8(&s, 0x01u);
    put_u8(&s, 0x40u);
    put_uleb(&s, 2u);
    put_name(&s, "self");
    put_uleb(&s, 1u);
    put_name(&s, "path");
    put_u8(&s, 0x73u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 3u);

    /* export the canonical method name */
    put_u8(&s, 0x04u);
    put_nameattr(&s, "[method]descriptor.create-directory-at");
    put_u8(&s, 0x01u);
    put_uleb(&s, 4u);

    put_section(component, 7u, &s);
}

static void build_method_component(bytebuf *out) {
    bytebuf memory_module = {0};
    bytebuf consumer_module = {0};
    bytebuf s = {0};

    put_component_header(out);
    build_memory_module(&memory_module);
    build_consumer_module(&consumer_module);
    put_section(out, 1u, &memory_module);
    put_section(out, 1u, &consumer_module);

    /* instantiate memory module -> core instance 0 */
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_uleb(&s, 0u);
    put_section(out, 2u, &s);

    /* alias memory 0 from core instance 0 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_u8(&s, 0x02u);
    put_u8(&s, 0x01u);
    put_uleb(&s, 0u);
    put_name(&s, "mem");
    put_section(out, 6u, &s);

    append_fs_types_type_section(out);

    /* import wasi:filesystem/types@0.2.8 as component instance 0 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_nameattr(&s, "wasi:filesystem/types@0.2.8");
    put_u8(&s, 0x05u);
    put_uleb(&s, 0u);
    put_section(out, 10u, &s);

    /* alias descriptor type -> outer type 1; alias method -> component func 0 */
    s.size = 0u;
    put_uleb(&s, 2u);
    put_u8(&s, 0x03u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_name(&s, "descriptor");
    put_u8(&s, 0x01u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_name(&s, "[method]descriptor.create-directory-at");
    put_section(out, 6u, &s);

    /* canon lower method with memory option -> core func 0 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x01u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_uleb(&s, 1u);
    put_u8(&s, 0x03u);
    put_uleb(&s, 0u);
    put_section(out, 8u, &s);

    /* canon resource.drop descriptor -> core func 1 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x03u);
    put_uleb(&s, 1u);
    put_section(out, 8u, &s);

    /* inline core instance 1 exports method and borrow release */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x01u);
    put_uleb(&s, 2u);
    put_name(&s, "mkdir");
    put_u8(&s, 0x00u);
    put_uleb(&s, 0u);
    put_name(&s, "drop");
    put_u8(&s, 0x00u);
    put_uleb(&s, 1u);
    put_section(out, 2u, &s);

    /* instantiate consumer module 1 -> core instance 2 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 1u);
    put_uleb(&s, 2u);
    put_name(&s, "m");
    put_u8(&s, 0x12u);
    put_uleb(&s, 0u);
    put_name(&s, "p");
    put_u8(&s, 0x12u);
    put_uleb(&s, 1u);
    put_section(out, 2u, &s);

    /* alias core consumer run -> core func 2 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_u8(&s, 0x00u);
    put_u8(&s, 0x01u);
    put_uleb(&s, 2u);
    put_name(&s, "run");
    put_section(out, 6u, &s);

    /* outer type 2 = borrow<descriptor>; type 3 = run(self)->u32 */
    s.size = 0u;
    put_uleb(&s, 2u);
    put_u8(&s, 0x68u);
    put_uleb(&s, 1u);
    put_u8(&s, 0x40u);
    put_uleb(&s, 1u);
    put_name(&s, "self");
    put_uleb(&s, 2u);
    put_u8(&s, 0x00u);
    put_u8(&s, 0x79u);
    put_section(out, 7u, &s);

    /* canon lift core run with type 3 -> component func 1 */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_u8(&s, 0x00u);
    put_uleb(&s, 2u);
    put_uleb(&s, 0u);
    put_uleb(&s, 3u);
    put_section(out, 8u, &s);

    /* export component func 1 as run */
    s.size = 0u;
    put_uleb(&s, 1u);
    put_nameattr(&s, "run");
    put_u8(&s, 0x01u);
    put_uleb(&s, 1u);
    put_u8(&s, 0x00u);
    put_section(out, 11u, &s);
}

typedef struct fake_fs {
    uint32_t close_calls;
    uint32_t create_calls;
} fake_fs;

static uint32_t fake_close(void *context, turbowasm_wasi_fs_file file) {
    fake_fs *fs = (fake_fs *)context;
    (void)file;
    ++fs->close_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_read(
    void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_buffer *buffers, size_t buffer_count,
    uint32_t *out_read) {
    (void)context; (void)file; (void)buffers; (void)buffer_count;
    if (out_read != NULL) *out_read = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_write(
    void *context, turbowasm_wasi_fs_file file,
    const turbowasm_wasi_const_buffer *buffers, size_t buffer_count,
    uint32_t *out_written) {
    (void)context; (void)file; (void)buffers; (void)buffer_count;
    if (out_written != NULL) *out_written = 0u;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t fake_create(
    void *context,
    turbowasm_wasi_fs_file directory,
    const uint8_t *path,
    size_t path_length) {
    fake_fs *fs = (fake_fs *)context;
    (void)directory;
    assert(path_length == 1u);
    assert(path != NULL && path[0] == (uint8_t)'x');
    ++fs->create_calls;
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static void test_component_method_exec(void) {
    bytebuf component_bytes = {0};
    fake_fs fake = {0};
    turbowasm_wasi_fs filesystem = {0};
    turbowasm_wasi_fs_config config = {0};
    turbowasm_wasi_fs_descriptor preopen_desc = {0};
    turbowasm_wasi02_filesystem bridge = {0};
    turbowasm_wasi02_value dirs = {0};
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_component_value argument = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    uint32_t resource;
    uint64_t rights =
        TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY |
        TURBOWASM_WASI_RIGHT_PATH_OPEN |
        TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET;

    build_method_component(&component_bytes);

    config.descriptor_capacity = 4u;
    config.provider.context = &fake;
    config.provider.close = fake_close;
    config.provider.read = fake_read;
    config.provider.write = fake_write;
    config.provider.path_create_directory = fake_create;
    assert(turbowasm_wasi_fs_init(
               &filesystem, &config) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_bind_descriptor_with_rights(
               &filesystem, 3u,
               (turbowasm_wasi_fs_file){UINT64_C(1), 1u},
               true, "/", rights, rights,
               &preopen_desc) == TURBOWASM_OK);

    assert(turbowasm_wasi02_filesystem_init(
               &bridge, &filesystem, 8u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_get_directories(
               &bridge, &dirs) == TURBOWASM_OK);
    resource =
        dirs.as.list.items[0].as.tuple.items[0].as.resource;
    turbowasm_wasi02_value_destroy(&dirs);

    assert(turbowasm_component_binary_load(
               &binary,
               component_bytes.data,
               component_bytes.size) == TURBOWASM_OK);
    assert(binary.import_count == 1u);
    assert(binary.canon_lower_count == 1u);
    {
        const turbowasm_component_type *resource_type =
            turbowasm_component_type_graph_get(
                &binary.type_graph, 1u);
        const turbowasm_component_type *borrow_type =
            turbowasm_component_type_graph_get(
                &binary.type_graph, 2u);
        const turbowasm_component_type *run_type =
            turbowasm_component_type_graph_get(
                &binary.type_graph, 3u);

        assert(resource_type != NULL);
        assert(resource_type->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE);
        assert(resource_type->as.resource.identity_alias);
        assert(borrow_type != NULL);
        assert(borrow_type->kind == TURBOWASM_COMPONENT_TYPE_BORROW);
        assert(borrow_type->as.handle.resource_type == 1u);
        assert(run_type != NULL);
        assert(run_type->kind == TURBOWASM_COMPONENT_TYPE_FUNCTION);
        assert(run_type->as.function.param_count == 1u);
        assert(run_type->as.function.params[0].kind ==
               TURBOWASM_COMPONENT_TYPE_REF_INDEXED);
        assert(run_type->as.function.params[0].as.indexed == 2u);
    }

    assert(turbowasm_wasi02_filesystem_component_exec_init(
               &exec, &binary, &bridge) == TURBOWASM_OK);
    assert(bridge.descriptor_identity_bound);

    argument.kind = TURBOWASM_COMPONENT_TYPE_BORROW;
    argument.as.resource_rep.kind = TURBOWASM_VALUE_I32;
    argument.as.resource_rep.as.i32 = (int32_t)resource;

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"run", 3u,
               &argument, 1u,
               &result, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == 0u);
    assert(fake.create_calls == 1u);
    assert(fake.close_calls == 0u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&binary);

    assert(turbowasm_wasi02_filesystem_descriptor_drop(
               &bridge, resource) == TURBOWASM_OK);
    assert(turbowasm_wasi02_filesystem_destroy(
               &bridge) == TURBOWASM_OK);
    assert(turbowasm_wasi_fs_close_descriptor(
               &filesystem, preopen_desc) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(turbowasm_wasi_fs_destroy(
               &filesystem) == TURBOWASM_OK);
    assert(fake.close_calls == 1u);
}

int main(void) {
    test_component_method_exec();
    return 0;
}
