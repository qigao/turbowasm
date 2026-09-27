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

static void test_non_function_import_fails_closed(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &module,
               memory_import_bytes,
               sizeof(memory_import_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);

    assert(turbowasm_instance_create_linked(
               &instance,
               &module,
               &linker) == TURBOWASM_UNSUPPORTED);
    assert(instance.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_typed_cross_module_call();
    test_missing_module_rejected();
    test_signature_mismatch_rejected();
    test_duplicate_namespace_rejected();
    test_unresolved_provider_export_rejected();
    test_non_function_import_fails_closed();
    return 0;
}
