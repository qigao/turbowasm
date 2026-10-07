#include "component_binary.h"
#include "component_exec.h"
#include <turbowasm/component.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static turbowasm_component_binary binary;
static turbowasm_component_exec exec;
static turbowasm_component component;
static turbowasm_runtime_config config;
static size_t live_allocations, allowance;
static uint8_t bytes[2048];
static size_t length;

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0u) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live_allocations;
    return p;
}
static void deallocate(void *context, void *p) {
    (void)context; if (p != NULL) --live_allocations; free(p);
}
static void start(void) {
    static const uint8_t header[] = {0,0x61,0x73,0x6d,0x0d,0,1,0};
    memcpy(bytes, header, sizeof(header)); length = sizeof(header);
}
static void section(uint8_t id, const uint8_t *payload, size_t count) {
    check_less(count, 128u); check_less_equal(length + 2u + count, sizeof(bytes));
    bytes[length++] = id; bytes[length++] = (uint8_t)count;
    memcpy(bytes + length, payload, count); length += count;
}
static turbowasm_status decode(void) {
    return turbowasm_component_binary_decode_async_metadata(&binary, bytes, length, &config);
}
static void base(bool async_function, bool wide) {
    uint8_t module[] = {
        0,0x61,0x73,0x6d,1,0,0,0,
        1,4,1,0x60,0,0, 3,2,1,0,
        5,3,1,0,1,
        7,9,2,1,'f',0,0,1,'m',2,0,
        10,4,1,2,0,0x0b
    };
    static const uint8_t instance[] = {1,0,0,0};
    static const uint8_t aliases[] = {2,0,0,1,0,1,'f',0,2,1,0,1,'m'};
    uint8_t type[] = {1,0x43,0,1,0};
    start(); if (!async_function) type[1] = 0x40;
    if (wide) module[21] = 4;
    section(1, module, sizeof(module)); section(2, instance, sizeof(instance));
    section(6, aliases, sizeof(aliases)); section(7, type, sizeof(type));
}
static void lift(const uint8_t *options, size_t size, uint8_t count) {
    uint8_t payload[40] = {1,0,0,0,0};
    check_less_equal(size, sizeof(payload) - 6u);
    payload[4] = count; if (size != 0u) memcpy(payload + 5u, options, size);
    payload[5u + size] = 0;
    section(8, payload, 6u + size);
}
static void lower(const uint8_t *options, size_t size, uint8_t count, uint8_t index) {
    uint8_t payload[40] = {1,1,0,0,0};
    check_less_equal(size, sizeof(payload) - 5u);
    payload[3] = index; payload[4] = count;
    if (size != 0u) memcpy(payload + 5u, options, size);
    section(8, payload, 5u + size);
}
static void failed(turbowasm_status expected) {
    check_equal(decode(), expected);
    check_null(binary.bytes); check_null(binary.type_graph.types);
    check_false(binary.async_metadata); check_equal(live_allocations, 0u);
}

spec("private Component async binary metadata") {
    before_each() {
        allowance = SIZE_MAX; live_allocations = 0u;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        start();
    }
    after_each() {
        allowance = SIZE_MAX;
        turbowasm_component_exec_destroy(&exec);
        turbowasm_component_destroy(&component);
        turbowasm_component_binary_destroy(&binary);
        check_equal(live_allocations, 0u);
    }

    it("retains async functions and typed or unit endpoints without admitting public execution") {
        static const uint8_t types[] = {
            5, 0x3f,0x7f,0, 0x69,0, 0x65,1,1, 0x66,0,
            0x43,1,1,'x',2,0,3
        };
        section(7, types, sizeof(types));
        check_equal(turbowasm_component_load_borrowed_with_config(&component, bytes, length, &config), TURBOWASM_UNSUPPORTED);
        check_equal(live_allocations, 0u);
        check_equal(decode(), TURBOWASM_OK);
        check_true(binary.async_metadata); check_equal(binary.type_graph.count, 5u);
        check_equal(binary.type_graph.types[2].kind, TURBOWASM_COMPONENT_TYPE_FUTURE);
        check_equal(binary.type_graph.types[2].as.async_value.payload.as.indexed, 1u);
        check_false(binary.type_graph.types[3].as.async_value.has_payload);
        check_true(binary.type_graph.types[4].as.function.is_async);
        check_equal(binary.type_graph.types[4].as.function.params[0].as.indexed, 2u);
        allowance = 0u;
        check_equal(turbowasm_component_exec_init(&exec, &binary), TURBOWASM_UNSUPPORTED);
        check_false(exec.initialized); check_null(exec.binary);
    }

    it("preserves async flags and endpoint payloads in instance-local aliases") {
        static const uint8_t types[] = {
            1,0x42,6,
            1,0x65,1,0x79,
            4,0,1,'t',3,0,0,
            1,0x43,0,0,1,
            4,0,1,'f',3,0,2,
            4,0,1,'g',1,3,
            1,0x40,0,1,0
        };
        const turbowasm_component_type_graph *local;
        section(7, types, sizeof(types)); check_equal(decode(), TURBOWASM_OK);
        local = &binary.type_graph.types[0].as.instance->type_graph;
        check_equal(local->count, 5u);
        check_equal(local->types[1].kind, TURBOWASM_COMPONENT_TYPE_FUTURE);
        check_equal(local->types[1].as.async_value.payload.as.inline_type, TURBOWASM_COMPONENT_TYPE_U32);
        check_true(local->types[2].as.function.is_async); check_true(local->types[3].as.function.is_async);
        check_false(local->types[4].as.function.is_async);
        check_equal(local->types[3].as.function.result.as.indexed, 1u);
    }

    it("rejects invalid async payload references and transitive borrow ownership") {
        static const uint8_t invalid[][16] = {
            {1,0x65,2},
            {1,0x65,1,0},
            {1,0x66,1,0x74},
            {3,0x3f,0x7f,0,0x68,0,0x65,1,1},
            {2,0x40,0,1,0,0x65,1,0},
            {2,0x74,0x66,1,0},
            {4,0x3f,0x7f,0,0x68,0,0x70,1,0x65,1,2}
        };
        static const size_t sizes[] = {3u,4u,4u,9u,8u,5u,11u};
        unsigned i;
        for (i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            start(); section(7, invalid[i], sizes[i]); failed(TURBOWASM_MALFORMED_MODULE);
        }
    }

    it("retains stackful and callback lift options under both memory widths") {
        unsigned width, callback;
        for (width = 0u; width < 2u; ++width)
            for (callback = 0u; callback < 2u; ++callback) {
                static const uint8_t opts[] = {3,0,4,0,1,6,7,0};
                base(true, width != 0u); lift(opts, callback ? sizeof(opts) : sizeof(opts) - 2u, callback ? 5u : 4u);
                check_equal(decode(), TURBOWASM_OK);
                check_equal(binary.canon_lift_count, 1u); check_true(binary.canon_lifts[0].is_async);
                check_equal(binary.canon_lifts[0].has_callback, callback != 0u);
                check_equal(binary.canon_lifts[0].callback_function_index, 0u);
                check_equal(binary.canon_lifts[0].string_encoding, TURBOWASM_COMPONENT_STRING_UTF16);
                check_true(binary.canon_lifts[0].has_memory); check_true(binary.canon_lifts[0].has_realloc);
                turbowasm_component_binary_destroy(&binary);
            }
    }

    it("rejects duplicate conflicting and out-of-range lift options") {
        static const uint8_t invalid[][8] = {
            {6,6}, {7,0}, {6,7,0,7,0}, {6,5,0}, {6,7,1},
            {6,3,1}, {6,4,1}, {6,0,1}, {6,4,0}, {6,3,0,3,0}
        };
        static const uint8_t sizes[] = {2,2,5,3,3,3,3,3,3,5};
        static const uint8_t counts[] = {2,1,3,2,2,2,2,3,2,3};
        unsigned i;
        for (i = 0u; i < sizeof(sizes); ++i) {
            base(true, false); lift(invalid[i], sizes[i], counts[i]); failed(TURBOWASM_MALFORMED_MODULE);
        }
        base(false, false); lift(invalid[0], 1u, 1u); failed(TURBOWASM_MALFORMED_MODULE);
    }

    it("requires async lowering to have memory and an async-typed callee") {
        static const uint8_t valid[] = {6,3,0};
        static const uint8_t callback[] = {6,3,0,7,0};
        static const uint8_t duplicate[] = {6,3,0,6};
        base(true, false); lift(NULL, 0u, 0u); lower(valid, sizeof(valid), 2u, 0u);
        check_equal(decode(), TURBOWASM_OK);
        check_true(binary.canon_lowers[0].is_async); check_true(binary.canon_lowers[0].has_memory);
        turbowasm_component_binary_destroy(&binary);
        base(false, false); lift(NULL, 0u, 0u); lower(valid, sizeof(valid), 2u, 0u); failed(TURBOWASM_MALFORMED_MODULE);
        base(true, false); lift(NULL, 0u, 0u); lower(valid, 1u, 1u, 0u); failed(TURBOWASM_MALFORMED_MODULE);
        base(true, false); lift(NULL, 0u, 0u); lower(callback, sizeof(callback), 3u, 0u); failed(TURBOWASM_MALFORMED_MODULE);
        base(true, false); lift(NULL, 0u, 0u); lower(duplicate, sizeof(duplicate), 3u, 0u); failed(TURBOWASM_MALFORMED_MODULE);
    }

    it("resolves async lowered callees through local and imported-instance aliases") {
        static const uint8_t opts[] = {6,3,0};
        static const uint8_t local_instance[] = {1,1,1,0,1,'f',1,0};
        static const uint8_t alias[] = {1,1,0,0,1,'f'};
        static const uint8_t instance_type[] = {1,0x42,2,1,0x43,0,1,0,4,0,1,'f',1,0};
        static const uint8_t import_instance[] = {1,0,1,'i',5,1};
        static const uint8_t export_function[] = {1,0,1,'f',1,0,0};
        static const uint8_t import_function[] = {1,0,1,'f',1,0};
        base(true, false); lift(NULL, 0u, 0u); section(5, local_instance, sizeof(local_instance));
        section(6, alias, sizeof(alias)); lower(opts, sizeof(opts), 2u, 1u);
        check_equal(decode(), TURBOWASM_OK); check_true(binary.canon_lowers[0].is_async);
        turbowasm_component_binary_destroy(&binary);
        base(true, false); section(7, instance_type, sizeof(instance_type));
        section(10, import_instance, sizeof(import_instance)); section(6, alias, sizeof(alias));
        lower(opts, sizeof(opts), 2u, 0u);
        check_equal(decode(), TURBOWASM_OK); check_true(binary.canon_lowers[0].is_async);
        turbowasm_component_binary_destroy(&binary);
        base(true, false); lift(NULL, 0u, 0u); section(11, export_function, sizeof(export_function));
        lower(opts, sizeof(opts), 2u, 1u);
        check_equal(decode(), TURBOWASM_OK); check_true(binary.canon_lowers[0].is_async);
        turbowasm_component_binary_destroy(&binary);
        base(true, false); section(10, import_function, sizeof(import_function)); lower(opts, sizeof(opts), 2u, 0u);
        check_equal(decode(), TURBOWASM_OK); check_true(binary.canon_lowers[0].is_async);
    }

    it("rejects truncated option indices and failed semantic decoding without retaining metadata") {
        static const uint8_t incomplete_callback[] = {6,7};
        static const uint8_t incomplete_realloc[] = {6,3,0,4};
        static const uint8_t invalid_optional_payload[] = {1,0x65,1};
        base(true, false); lift(incomplete_callback, sizeof(incomplete_callback), 2u); failed(TURBOWASM_MALFORMED_MODULE);
        base(true, false); lift(incomplete_realloc, sizeof(incomplete_realloc), 3u); failed(TURBOWASM_MALFORMED_MODULE);
        start(); section(7, invalid_optional_payload, sizeof(invalid_optional_payload)); failed(TURBOWASM_MALFORMED_MODULE);
    }

    it("releases every retained allocation when async metadata decoding runs out of memory") {
        static const uint8_t opts[] = {3,0,6,7,0};
        size_t limit; bool succeeded = false;
        base(true, false); lift(opts, sizeof(opts), 3u);
        for (limit = 0u; limit < 256u; ++limit) {
            turbowasm_status status;
            allowance = limit; status = decode(); allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                succeeded = true; turbowasm_component_binary_destroy(&binary); break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_null(binary.bytes); check_null(binary.type_graph.types);
            check_false(binary.async_metadata); check_equal(live_allocations, 0u);
        }
        check_true(succeeded); check_equal(live_allocations, 0u);
    }
}
