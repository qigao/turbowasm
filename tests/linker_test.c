#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static const uint8_t provider_bytes[] = {
    WASM_HEADER,

    /* type 0: (i32) -> i32 */
    0x01, 0x06,
    0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

    /* local function 0 uses type 0 */
    0x03, 0x02,
    0x01, 0x00,

    /* export function 0 as "inc" */
    0x07, 0x07,
    0x01,
    0x03, 0x69, 0x6e, 0x63,
    0x00, 0x00,

    /* local.get 0; i32.const 1; i32.add */
    0x0a, 0x09,
    0x01, 0x07,
    0x00,
    0x20, 0x00,
    0x41, 0x01,
    0x6a,
    0x0b
};

static const uint8_t consumer_bytes[] = {
    WASM_HEADER,

    /* type 0: (i32) -> i32 */
    0x01, 0x06,
    0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

    /* import math.inc as function 0 */
    0x02, 0x0c,
    0x01,
    0x04, 0x6d, 0x61, 0x74, 0x68,
    0x03, 0x69, 0x6e, 0x63,
    0x00, 0x00,

    /* local function 1 uses type 0 */
    0x03, 0x02,
    0x01, 0x00,

    /* export local function 1 as "run" */
    0x07, 0x07,
    0x01,
    0x03, 0x72, 0x75, 0x6e,
    0x00, 0x01,

    /* local.get 0; call imported function 0 */
    0x0a, 0x08,
    0x01, 0x06,
    0x00,
    0x20, 0x00,
    0x10, 0x00,
    0x0b
};

static const uint8_t wrong_provider_bytes[] = {
    WASM_HEADER,

    /* type 0: () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,

    0x03, 0x02,
    0x01, 0x00,

    0x07, 0x07,
    0x01,
    0x03, 0x69, 0x6e, 0x63,
    0x00, 0x00,

    0x0a, 0x06,
    0x01, 0x04,
    0x00,
    0x41, 0x07,
    0x0b
};

static const uint8_t unresolved_provider_bytes[] = {
    WASM_HEADER,

    /* type 0: (i32) -> i32 */
    0x01, 0x06,
    0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

    /* import base.inc as function 0 */
    0x02, 0x0c,
    0x01,
    0x04, 0x62, 0x61, 0x73, 0x65,
    0x03, 0x69, 0x6e, 0x63,
    0x00, 0x00,

    /* re-export unresolved function 0 as "inc" */
    0x07, 0x07,
    0x01,
    0x03, 0x69, 0x6e, 0x63,
    0x00, 0x00
};

static const uint8_t memory_import_bytes[] = {
    WASM_HEADER,

    /* import math.mem, min 1 page */
    0x02, 0x0d,
    0x01,
    0x04, 0x6d, 0x61, 0x74, 0x68,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x00, 0x01
};

static const uint8_t table_import_bytes[] = {
    WASM_HEADER,

    /* import math.tab, funcref, min 1 */
    0x02, 0x0e,
    0x01,
    0x04, 0x6d, 0x61, 0x74, 0x68,
    0x03, 0x74, 0x61, 0x62,
    0x01, 0x70, 0x00, 0x01
};

static turbowasm_name name_span(
    const uint8_t *bytes,
    uint32_t size) {
    turbowasm_name name = {bytes, size};
    return name;
}

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_I32;
    result.as.i32 = value;
    return result;
}

static void test_typed_cross_module_call(void) {
    uint8_t namespace_bytes[4] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};
    turbowasm_value argument = i32_value(41);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider,
               &provider_module) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(namespace_bytes, 4u),
               &provider) == TURBOWASM_OK);

    /* Namespace ownership is linker-local after define. */
    namespace_bytes[0] = (uint8_t)'x';

    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_OK);

    /* Resolved bindings do not retain the linker itself. */
    turbowasm_linker_destroy(&linker);

    assert(turbowasm_instance_invoke(
               &consumer,
               1u,
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 42);

    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_missing_module_rejected(void) {
    turbowasm_module consumer_module = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);

    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_LINK_ERROR);
    assert(consumer.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&consumer_module);
}

static void test_signature_mismatch_rejected(void) {
    static const uint8_t math_name[] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               wrong_provider_bytes,
               sizeof(wrong_provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider,
               &provider_module) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(math_name, 4u),
               &provider) == TURBOWASM_OK);

    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_TYPE_MISMATCH);
    assert(consumer.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_duplicate_namespace_rejected(void) {
    static const uint8_t math_name[] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    turbowasm_module provider_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider,
               &provider_module) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);

    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(math_name, 4u),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(math_name, 4u),
               &provider) == TURBOWASM_LINK_ERROR);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&provider_module);
}

static void test_unresolved_provider_export_rejected(void) {
    static const uint8_t math_name[] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               unresolved_provider_bytes,
               sizeof(unresolved_provider_bytes)) == TURBOWASM_OK);
    /* The legacy create path keeps function imports unresolved/fail-closed. */
    assert(turbowasm_instance_create(
               &provider,
               &provider_module) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(math_name, 4u),
               &provider) == TURBOWASM_OK);

    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_LINK_ERROR);
    assert(consumer.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_legacy_unlinked_memory_fails_closed(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module,
               memory_import_bytes,
               sizeof(memory_import_bytes)) == TURBOWASM_OK);

    assert(turbowasm_instance_create(
               &instance,
               &module) == TURBOWASM_UNSUPPORTED);
    assert(instance.impl == NULL);

    turbowasm_module_destroy(&module);
}

static void test_table_import_stays_explicitly_unsupported(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &module,
               table_import_bytes,
               sizeof(table_import_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);

    assert(turbowasm_instance_create_linked(
               &instance,
               &module,
               &linker) == TURBOWASM_UNSUPPORTED);
    assert(instance.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
}


static const uint8_t memory_provider_bytes[] = {
    WASM_HEADER,

    /* type 0: () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,

    /* two local functions use type 0 */
    0x03, 0x03,
    0x02, 0x00, 0x00,

    /* memory 0: min 1, max 2 */
    0x05, 0x04,
    0x01, 0x01, 0x01, 0x02,

    /* export mem, read, size */
    0x07, 0x15,
    0x03,
    0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00,
    0x04, 0x72, 0x65, 0x61, 0x64, 0x00, 0x00,
    0x04, 0x73, 0x69, 0x7a, 0x65, 0x00, 0x01,

    /* read(): i32.const 0; i32.load; size(): memory.size */
    0x0a, 0x0e,
    0x02,
    0x07, 0x00, 0x41, 0x00, 0x28, 0x02, 0x00, 0x0b,
    0x04, 0x00, 0x3f, 0x00, 0x0b
};

static const uint8_t memory_consumer_bytes[] = {
    WASM_HEADER,

    /* type 0: (i32) -> (); type 1: () -> i32 */
    0x01, 0x09,
    0x02,
    0x60, 0x01, 0x7f, 0x00,
    0x60, 0x00, 0x01, 0x7f,

    /* import math.mem, min 1, max 3 */
    0x02, 0x0e,
    0x01,
    0x04, 0x6d, 0x61, 0x74, 0x68,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x01, 0x01, 0x03,

    /* write, read, grow, size */
    0x03, 0x05,
    0x04, 0x00, 0x01, 0x01, 0x01,

    0x0a, 0x1f,
    0x04,

    /* write(v): [0] = v */
    0x09,
    0x00,
    0x41, 0x00,
    0x20, 0x00,
    0x36, 0x02, 0x00,
    0x0b,

    /* read(): [0] */
    0x07,
    0x00,
    0x41, 0x00,
    0x28, 0x02, 0x00,
    0x0b,

    /* grow(): memory.grow(1) */
    0x06,
    0x00,
    0x41, 0x01,
    0x40, 0x00,
    0x0b,

    /* size(): memory.size */
    0x04,
    0x00,
    0x3f, 0x00,
    0x0b
};

static const uint8_t memory_consumer_min_too_large_bytes[] = {
    WASM_HEADER,

    /* import math.mem, min 2, max 3 */
    0x02, 0x0e,
    0x01,
    0x04, 0x6d, 0x61, 0x74, 0x68,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x01, 0x02, 0x03
};

static void test_memory_import_is_one_live_growable_object(void) {
    static const uint8_t math_name[] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};
    turbowasm_value argument = i32_value(42);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               memory_provider_bytes,
               sizeof(memory_provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               memory_consumer_bytes,
               sizeof(memory_consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(math_name, 4u),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_OK);

    /* Consumer store is immediately visible through provider memory. */
    assert(turbowasm_instance_invoke(
               &consumer, 0u,
               &argument, 1u,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_OK);

    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &provider, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 42);

    /* Provider max=2 is a valid subtype of consumer max=3. */
    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &consumer, 2u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 1);

    /* Grow mutates the provider-owned object, not a copied consumer struct. */
    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &provider, 1u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 2);

    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &consumer, 3u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 2);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_memory_limits_mismatch_rejected(void) {
    static const uint8_t math_name[] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               memory_provider_bytes,
               sizeof(memory_provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               memory_consumer_min_too_large_bytes,
               sizeof(memory_consumer_min_too_large_bytes)) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(math_name, 4u),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_TYPE_MISMATCH);
    assert(consumer.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static const uint8_t mutable_global_provider_bytes[] = {
    WASM_HEADER,

    /* type 0: () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,

    /* local function 0 uses type 0 */
    0x03, 0x02,
    0x01, 0x00,

    /* mutable i32 global 0 = 5 */
    0x06, 0x06,
    0x01, 0x7f, 0x01, 0x41, 0x05, 0x0b,

    /* export global "g" and function "read" */
    0x07, 0x0c,
    0x02,
    0x01, 0x67, 0x03, 0x00,
    0x04, 0x72, 0x65, 0x61, 0x64, 0x00, 0x00,

    /* read(): global.get 0 */
    0x0a, 0x06,
    0x01, 0x04,
    0x00, 0x23, 0x00, 0x0b
};

static const uint8_t mutable_global_consumer_bytes[] = {
    WASM_HEADER,

    /* type 0: () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,

    /* import mutable i32 math.g as global 0 */
    0x02, 0x0b,
    0x01,
    0x04, 0x6d, 0x61, 0x74, 0x68,
    0x01, 0x67,
    0x03, 0x7f, 0x01,

    /* local function 0 uses type 0 */
    0x03, 0x02,
    0x01, 0x00,

    /*
     * global.get 0; i32.const 1; i32.add;
     * global.set 0; global.get 0
     */
    0x0a, 0x0d,
    0x01, 0x0b,
    0x00,
    0x23, 0x00,
    0x41, 0x01,
    0x6a,
    0x24, 0x00,
    0x23, 0x00,
    0x0b
};

static const uint8_t immutable_global_provider_bytes[] = {
    WASM_HEADER,

    /* immutable i32 global 0 = 9 */
    0x06, 0x06,
    0x01, 0x7f, 0x00, 0x41, 0x09, 0x0b,

    /* export global "g" */
    0x07, 0x05,
    0x01, 0x01, 0x67, 0x03, 0x00
};

static const uint8_t immutable_global_consumer_bytes[] = {
    WASM_HEADER,

    /* type 0: () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,

    /* import immutable i32 math.g as global 0 */
    0x02, 0x0b,
    0x01,
    0x04, 0x6d, 0x61, 0x74, 0x68,
    0x01, 0x67,
    0x03, 0x7f, 0x00,

    /* local function 0 uses type 0 */
    0x03, 0x02,
    0x01, 0x00,

    /* local immutable global 1 = global.get 0 */
    0x06, 0x06,
    0x01, 0x7f, 0x00, 0x23, 0x00, 0x0b,

    /* read local global 1 */
    0x0a, 0x06,
    0x01, 0x04,
    0x00, 0x23, 0x01, 0x0b
};

static void test_mutable_global_is_live_shared_state(void) {
    static const uint8_t math_name[] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               mutable_global_provider_bytes,
               sizeof(mutable_global_provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               mutable_global_consumer_bytes,
               sizeof(mutable_global_consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(math_name, 4u),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &consumer, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 6);

    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(
               &provider, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 6);

    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &consumer, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 7);

    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &provider, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result.as.i32 == 7);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_immutable_global_initializes_local_state(void) {
    static const uint8_t math_name[] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               immutable_global_provider_bytes,
               sizeof(immutable_global_provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               immutable_global_consumer_bytes,
               sizeof(immutable_global_consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(math_name, 4u),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &consumer, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 9);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_global_mutability_mismatch_rejected(void) {
    static const uint8_t math_name[] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               immutable_global_provider_bytes,
               sizeof(immutable_global_provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               mutable_global_consumer_bytes,
               sizeof(mutable_global_consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span(math_name, 4u),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_TYPE_MISMATCH);
    assert(consumer.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_typed_cross_module_call();
    test_missing_module_rejected();
    test_signature_mismatch_rejected();
    test_duplicate_namespace_rejected();
    test_unresolved_provider_export_rejected();
    test_legacy_unlinked_memory_fails_closed();
    test_table_import_stays_explicitly_unsupported();
    test_memory_import_is_one_live_growable_object();
    test_memory_limits_mismatch_rejected();
    test_mutable_global_is_live_shared_state();
    test_immutable_global_initializes_local_state();
    test_global_mutability_mismatch_rejected();
    return 0;
}
