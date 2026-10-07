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
    check_null(binary.async_builtins); check_equal(binary.async_builtin_count, 0u);
}

static void builtin(const uint8_t *definition, size_t count) {
    uint8_t payload[80] = {1};
    check_less(count, sizeof(payload));
    memcpy(payload + 1u, definition, count);
    section(8, payload, count + 1u);
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

    it("decodes all async builtin families with exact memory32 and memory64 signatures") {
        /* P means the selected memory pointer; all other entries are Core i32.
         * Packed endpoint handles and context storage have independent widths. */
        static const struct {
            uint8_t definition[8], size, params, results, pointer_params;
            bool pointer_result, i64_result, context;
        } cases[] = {
            {{0x05},1,0,0,0,false,false,false},
            {{0x06,1},2,1,1,0,false,false,false},
            {{0x09,1,0,0},4,0,0,0,false,false,false},
            {{0x0a,0x7e,1},3,0,1,0,false,true,true},
            {{0x0b,0x7e,0},3,1,0,1,false,false,true},
            {{0x0c,0},2,0,1,0,false,false,false},
            {{0x0d},1,1,0,0,false,false,false},
            {{0x0e,1},2,0,1,0,false,true,false},
            {{0x0f,1,2,3,0,6},6,3,1,6,true,false,false},
            {{0x10,1,1,3,0},5,3,1,6,true,false,false},
            {{0x11,1,1},3,1,1,0,false,false,false},
            {{0x12,1,0},3,1,1,0,false,false,false},
            {{0x13,1},2,1,0,0,false,false,false},
            {{0x14,1},2,1,0,0,false,false,false},
            {{0x15,2},2,0,1,0,false,true,false},
            {{0x16,2,1,3,0},5,2,1,2,false,false,false},
            {{0x17,2,2,3,0,6},6,2,1,2,false,false,false},
            {{0x18,2,0},3,1,1,0,false,false,false},
            {{0x19,2,1},3,1,1,0,false,false,false},
            {{0x1a,2},2,1,0,0,false,false,false},
            {{0x1b,2},2,1,0,0,false,false,false},
            {{0x1f},1,0,1,0,false,false,false},
            {{0x20,0,0},3,2,1,2,false,false,false},
            {{0x21,0,0},3,2,1,2,false,false,false},
            {{0x22},1,1,0,0,false,false,false},
            {{0x23},1,2,0,0,false,false,false},
            {{0x24},1,0,0,0,false,false,false},
            {{0x25},1,0,0,0,false,false,false},
            {{0x2e,1},2,2,0,0,false,false,false},
            {{0x2f,2},2,2,0,0,false,false,false}
        };
        static const uint8_t types[] = {2,0x66,0,0x65,0};
        unsigned wide; size_t i;
        for (wide = 0; wide < 2; ++wide) {
            base(true, wide != 0); section(7, types, sizeof(types));
            for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
                builtin(cases[i].definition, cases[i].size);
            check_equal(decode(), TURBOWASM_OK);
            check_equal(binary.async_builtin_count, 30u); check_equal(binary.core_function_count, 31u);
            for (i = 0; i < 30u; ++i) {
                turbowasm_component_flat_signature signature;
                const turbowasm_component_async_builtin *b = &binary.async_builtins[i];
                unsigned p;
                check_equal(b->kind, cases[i].definition[0]); check_equal(b->core_function_index, i + 1u);
                check_equal(turbowasm_component_async_builtin_signature(&binary.type_graph, b,
                    wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32, &signature), TURBOWASM_OK);
                check_equal(signature.param_count, cases[i].params); check_equal(signature.result_count, cases[i].results);
                check_false(signature.params_indirect); check_false(signature.results_indirect);
                for (p = 0; p < signature.param_count; ++p)
                    check_equal(signature.params[p], ((cases[i].pointer_params & (1u << p)) && (wide || cases[i].context))
                        ? TURBOWASM_COMPONENT_FLAT_I64 : TURBOWASM_COMPONENT_FLAT_I32);
                if (signature.result_count)
                    check_equal(signature.results[0], cases[i].i64_result || (wide && cases[i].pointer_result)
                        ? TURBOWASM_COMPONENT_FLAT_I64 : TURBOWASM_COMPONENT_FLAT_I32);
            }
            turbowasm_component_binary_destroy(&binary); check_equal(live_allocations, 0u);
        }
        start(); builtin(cases[0].definition, cases[0].size);
        check_equal(turbowasm_component_load_borrowed_with_config(&component, bytes, length, &config), TURBOWASM_UNSUPPORTED);
    }

    it("requires destination realloc for dynamic endpoint payloads and keeps nested handles opaque") {
        static const uint8_t types[] = {5,0x66,1,0x79,0x65,1,0x73,0x66,1,0x73,0x65,1,3,0x66,0};
        uint8_t copy[] = {0x0f,1,1,3,0};
        unsigned opcode, index;
        for (opcode = 0; opcode < 4; ++opcode) {
            bool reading = opcode == 0 || opcode == 2;
            copy[0] = opcode < 2 ? (uint8_t)(0x0f + opcode) : (uint8_t)(0x16 + opcode - 2);
            copy[1] = opcode < 2 ? 3 : 2;
            base(true, false); section(7, types, sizeof(types)); builtin(copy, sizeof(copy));
            if (reading) failed(TURBOWASM_MALFORMED_MODULE);
            else { check_equal(decode(), TURBOWASM_OK); turbowasm_component_binary_destroy(&binary); }
            {
                uint8_t options[] = {copy[0],copy[1],4,3,0,4,0,2,6};
                base(true, true); section(7, types, sizeof(types)); builtin(options, sizeof(options));
                check_equal(decode(), TURBOWASM_OK);
                check_true(binary.async_builtins[0].has_realloc); check_true(binary.async_builtins[0].is_async);
                check_equal(binary.async_builtins[0].string_encoding, TURBOWASM_COMPONENT_STRING_LATIN1_UTF16);
                turbowasm_component_binary_destroy(&binary);
            }
        }
        for (index = 1; index <= 5; ++index) {
            uint8_t no_memory[] = {index == 2 || index == 4 ? 0x16 : 0x0f, (uint8_t)index,0};
            base(true, false); section(7, types, sizeof(types)); builtin(no_memory, sizeof(no_memory));
            if (index != 5) failed(TURBOWASM_MALFORMED_MODULE);
            else {
                turbowasm_component_flat_signature sig;
                check_equal(decode(), TURBOWASM_OK);
                check_equal(turbowasm_component_async_builtin_signature(&binary.type_graph, &binary.async_builtins[0],
                    TURBOWASM_COMPONENT_POINTER_I64, &sig), TURBOWASM_OK);
                check_equal(sig.params[1], TURBOWASM_COMPONENT_FLAT_I32); check_equal(sig.results[0], TURBOWASM_COMPONENT_FLAT_I32);
                turbowasm_component_binary_destroy(&binary);
            }
        }
        copy[0] = 0x16; copy[1] = 4;
        base(true, false); section(7, types, sizeof(types)); builtin(copy, sizeof(copy));
        check_equal(decode(), TURBOWASM_OK); check_false(binary.async_builtins[0].has_realloc);
    }

    it("rejects malformed async builtin immediates options kinds and forward references") {
        static const uint8_t types[] = {2,0x66,0,0x65,0};
        static const struct { uint8_t data[12], size; } invalid[] = {
            {{0x06,2},2}, {{0x0c,1},2}, {{0x20,1,0},3}, {{0x21,0,1},3},
            {{0x0a,0x7f,2},3}, {{0x0b,0x7d,0},3}, {{0x0e,2},2}, {{0x15,1},2},
            {{0x2e,0},2}, {{0x2f,3},2}, {{0x11,1,2},3}, {{0x19,2,2},3},
            {{0x0f,1,2,0,1},5}, {{0x0f,1,2,6,6},5}, {{0x0f,1,2,3,0,3,0},7},
            {{0x0f,1,3,3,0,4,0,4,0},9}, {{0x0f,1,1,4,0},5},
            {{0x0f,1,2,3,0,4,1},7}, {{0x0f,1,1,5,0},5}, {{0x16,2,1,7,0},5},
            {{0x09,2,0},3}, {{0x09,1,1,0},4}, {{0x09,0,0,0},4},
            {{0x09,1,0,1,6},5}, {{0x09,1,0,2,3,0,4,0},8}, {{0x09,0,0x73,0},4}
        };
        size_t i;
        for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            base(true, false); section(7, types, sizeof(types)); builtin(invalid[i].data, invalid[i].size);
            failed(TURBOWASM_MALFORMED_MODULE);
        }
        for (i = 0x1c; i <= 0x1e; ++i) {
            uint8_t opcode = (uint8_t)i;
            start(); builtin(&opcode, 1); failed(TURBOWASM_UNSUPPORTED);
        }
        { static const uint8_t thread_index[] = {0x26};
          start(); builtin(thread_index, sizeof(thread_index)); failed(TURBOWASM_UNSUPPORTED); }
    }

    it("enforces context storage width across canonical sections and preserves failed signature outputs") {
        static const uint8_t get[] = {0x0a,0x7f,0}, set[] = {0x0b,0x7e,1};
        static const uint8_t set32[] = {0x0b,0x7f,1}, intervening[] = {0x24};
        turbowasm_component_flat_signature before, output;
        turbowasm_component_async_builtin invalid = {0};
        start(); builtin(get, sizeof(get)); builtin(intervening, sizeof(intervening)); builtin(set32, sizeof(set32));
        check_equal(decode(), TURBOWASM_OK);
        check_equal(turbowasm_component_async_builtin_signature(&binary.type_graph, &binary.async_builtins[2],
            TURBOWASM_COMPONENT_POINTER_I64, &output), TURBOWASM_OK);
        check_equal(output.param_count, 1u); check_equal(output.params[0], TURBOWASM_COMPONENT_FLAT_I32);
        turbowasm_component_binary_destroy(&binary);
        builtin(set, sizeof(set)); failed(TURBOWASM_MALFORMED_MODULE);
        memset(&before, 0x55, sizeof(before)); output = before;
        invalid.kind = TURBOWASM_COMPONENT_CONTEXT_GET; invalid.context_index = 2;
        check_equal(turbowasm_component_async_builtin_signature(&binary.type_graph, &invalid,
            TURBOWASM_COMPONENT_POINTER_I32, &output), TURBOWASM_INVALID_ARGUMENT);
        check_equal(&output, &before, sizeof(output));
        invalid.kind = (turbowasm_component_async_builtin_kind)0x26;
        check_equal(turbowasm_component_async_builtin_signature(&binary.type_graph, &invalid,
            TURBOWASM_COMPONENT_POINTER_I32, &output), TURBOWASM_UNSUPPORTED);
        check_equal(&output, &before, sizeof(output));
    }

    it("decodes direct and indirect task return payloads with memory and encoding options") {
        uint8_t tuple[21] = {1,0x6f,17};
        static const uint8_t result[] = {0x09,0,1,2,3,0,1};
        static const uint8_t scalar[] = {0x09,0,0x7a,0};
        unsigned wide;
        memset(tuple + 3, 0x79, 17); /* tuple of 17 u32 */
        for (wide = 0; wide < 2; ++wide) {
            turbowasm_component_flat_signature sig;
            base(true, wide != 0); section(7, tuple, 20); builtin(result, sizeof(result)); builtin(scalar, sizeof(scalar));
            check_equal(decode(), TURBOWASM_OK);
            check_equal(turbowasm_component_async_builtin_signature(&binary.type_graph, &binary.async_builtins[0],
                wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32, &sig), TURBOWASM_OK);
            check_true(sig.params_indirect); check_equal(sig.param_count, 1u); check_equal(sig.result_count, 0u);
            check_equal(sig.params[0], wide ? TURBOWASM_COMPONENT_FLAT_I64 : TURBOWASM_COMPONENT_FLAT_I32);
            check_equal(turbowasm_component_async_builtin_signature(&binary.type_graph, &binary.async_builtins[1],
                TURBOWASM_COMPONENT_POINTER_I32, &sig), TURBOWASM_OK);
            check_false(sig.params_indirect); check_equal(sig.params[0], TURBOWASM_COMPONENT_FLAT_I32);
            turbowasm_component_binary_destroy(&binary);
        }
        { static const uint8_t missing_memory[] = {0x09,0,1,0};
          base(true, false); section(7, tuple, 20); builtin(missing_memory, sizeof(missing_memory)); failed(TURBOWASM_MALFORMED_MODULE); }
    }

    it("rejects every truncated builtin immediate and releases previously decoded entries") {
        static const uint8_t types[] = {1,0x66,0};
        static const struct { uint8_t data[10], size; } cases[] = {
            {{0x06,1},2}, {{0x09,0,0x73,1,3,0},6}, {{0x0a,0x7f,0},3},
            {{0x0c,0},2}, {{0x0e,1},2}, {{0x0f,1,3,3,0,4,0,6},8},
            {{0x11,1,1},3}, {{0x20,0,0},3}, {{0x2e,1},2}
        };
        static const uint8_t prior[] = {0x05};
        size_t i, n;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
            for (n = 0; n < cases[i].size; ++n) {
                base(true, false); section(7, types, sizeof(types)); builtin(prior, sizeof(prior)); builtin(cases[i].data, n);
                failed(TURBOWASM_MALFORMED_MODULE);
            }
    }

    it("releases every retained allocation when async metadata decoding runs out of memory") {
        static const uint8_t opts[] = {3,0,6,7,0};
        size_t limit; bool succeeded = false;
        base(true, false); lift(opts, sizeof(opts), 3u);
        { static const uint8_t definition[] = {0x09,0,0x73,1,3,0};
          unsigned i; for (i = 0; i < 20; ++i) builtin(definition, sizeof(definition)); }
        for (limit = 0u; limit < 256u; ++limit) {
            turbowasm_status status;
            allowance = limit; status = decode(); allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                succeeded = true; turbowasm_component_binary_destroy(&binary); break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_null(binary.bytes); check_null(binary.type_graph.types);
            check_false(binary.async_metadata); check_equal(live_allocations, 0u);
            check_null(binary.async_builtins); check_equal(binary.async_builtin_count, 0u);
        }
        check_true(succeeded); check_equal(live_allocations, 0u);
    }
}
