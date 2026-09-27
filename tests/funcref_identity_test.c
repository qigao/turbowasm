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

    /* type 0: () -> i32; type 1: () -> funcref */
    0x01, 0x09,
    0x02,
    0x60, 0x00, 0x01, 0x7f,
    0x60, 0x00, 0x01, 0x70,

    /* target, getref */
    0x03, 0x03,
    0x02, 0x00, 0x01,

    /* exporting target declares it for ref.func */
    0x07, 0x13,
    0x02,
    0x06, 0x74, 0x61, 0x72, 0x67, 0x65, 0x74, 0x00, 0x00,
    0x06, 0x67, 0x65, 0x74, 0x72, 0x65, 0x66, 0x00, 0x01,

    /* target() => 42; getref() => ref.func target */
    0x0a, 0x0b,
    0x02,
    0x04, 0x00, 0x41, 0x2a, 0x0b,
    0x04, 0x00, 0xd2, 0x00, 0x0b
};

static const uint8_t consumer_bytes[] = {
    WASM_HEADER,

    /* type 0: () -> i32; type 1: () -> funcref */
    0x01, 0x09,
    0x02,
    0x60, 0x00, 0x01, 0x7f,
    0x60, 0x00, 0x01, 0x70,

    /* import p.getref as function 0, type 1 */
    0x02, 0x0c,
    0x01,
    0x01, 0x70,
    0x06, 0x67, 0x65, 0x74, 0x72, 0x65, 0x66,
    0x00, 0x01,

    /* run, get, grow_call, fill_call, copy_call */
    0x03, 0x06,
    0x05, 0x00, 0x01, 0x00, 0x00, 0x00,

    /* local funcref table, min 1 */
    0x04, 0x04,
    0x01, 0x70, 0x00, 0x01,

    0x0a, 0x49,
    0x05,

    /*
     * run():
     *   table[0] = p.getref()
     *   call_indirect type0 table[0]
     */
    0x0d,
    0x00,
    0x41, 0x00,
    0x10, 0x00,
    0x26, 0x00,
    0x41, 0x00,
    0x11, 0x00, 0x00,
    0x0b,

    /* get(): table.get 0, index 0 */
    0x06,
    0x00,
    0x41, 0x00,
    0x25, 0x00,
    0x0b,

    /* grow_call(): grow by 1 using foreign ref, then call slot 1 */
    0x0f,
    0x00,
    0x10, 0x00,
    0x41, 0x01,
    0xfc, 0x0f, 0x00,
    0x1a,
    0x41, 0x01,
    0x11, 0x00, 0x00,
    0x0b,

    /* fill_call(): fill slot 0 with foreign ref, then call it */
    0x10,
    0x00,
    0x41, 0x00,
    0x10, 0x00,
    0x41, 0x01,
    0xfc, 0x11, 0x00,
    0x41, 0x00,
    0x11, 0x00, 0x00,
    0x0b,

    /* copy_call(): copy slot 0 -> slot 1, then call slot 1 */
    0x11,
    0x00,
    0x41, 0x01,
    0x41, 0x00,
    0x41, 0x01,
    0xfc, 0x0e, 0x00, 0x00,
    0x41, 0x01,
    0x11, 0x00, 0x00,
    0x0b
};

static const uint8_t legacy_ownerless_ref_bytes[] = {
    WASM_HEADER,

    /* type 0: (funcref) -> funcref */
    0x01, 0x06,
    0x01, 0x60, 0x01, 0x70, 0x01, 0x70,

    0x03, 0x02,
    0x01, 0x00,

    /* identity(ref) */
    0x0a, 0x06,
    0x01, 0x04,
    0x00, 0x20, 0x00, 0x0b
};

static turbowasm_name one_char_name(uint8_t *byte) {
    turbowasm_name name = {byte, 1u};
    return name;
}

static void test_ownerless_host_ref_normalizes_on_entry(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    argument.kind = TURBOWASM_VALUE_FUNCREF;
    argument.as.funcref.is_null = false;
    argument.as.funcref.function_index = 0u;
    argument.as.funcref.owner = NULL;

    assert(turbowasm_module_load_borrowed(
               &module,
               legacy_ownerless_ref_bytes,
               sizeof(legacy_ownerless_ref_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_FUNCREF);
    assert(!result.as.funcref.is_null);
    assert(result.as.funcref.function_index == 0u);
    assert(result.as.funcref.owner != NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    uint8_t provider_namespace = (uint8_t)'p';
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};
    turbowasm_value provider_ref = {0};
    turbowasm_value consumer_ref = {0};
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

    assert(turbowasm_instance_invoke(
               &provider, 1u,
               NULL, 0u,
               &provider_ref, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(provider_ref.kind == TURBOWASM_VALUE_FUNCREF);
    assert(!provider_ref.as.funcref.is_null);
    assert(provider_ref.as.funcref.function_index == 0u);
    assert(provider_ref.as.funcref.owner != NULL);

    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               one_char_name(&provider_namespace),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_OK);

    /* Foreign ref enters a consumer-local table and dispatches to provider. */
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(
               &consumer, 1u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 42);

    /* table.get preserves the same owner identity, not just raw index 0. */
    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &consumer, 2u,
               NULL, 0u,
               &consumer_ref, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(consumer_ref.kind == TURBOWASM_VALUE_FUNCREF);
    assert(!consumer_ref.as.funcref.is_null);
    assert(consumer_ref.as.funcref.function_index == 0u);
    assert(consumer_ref.as.funcref.owner ==
           provider_ref.as.funcref.owner);

    /* grow/fill/copy preserve the same foreign function-instance identity. */
    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &consumer, 3u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u && result.as.i32 == 42);

    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &consumer, 4u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u && result.as.i32 == 42);

    result_count = 0u;
    assert(turbowasm_instance_invoke(
               &consumer, 5u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u && result.as.i32 == 42);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);

    test_ownerless_host_ref_normalizes_on_entry();
    return 0;
}
