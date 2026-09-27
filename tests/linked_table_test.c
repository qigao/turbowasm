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

    /* type0 [] -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,

    /* target, call-table, table-size */
    0x03, 0x04,
    0x03, 0x00, 0x00, 0x00,

    /* table0 funcref min=1 max=2 */
    0x04, 0x05,
    0x01, 0x70, 0x01, 0x01, 0x02,

    /* export table "tab", function "call", function "size" */
    0x07, 0x15,
    0x03,
    0x03, 0x74, 0x61, 0x62, 0x01, 0x00,
    0x04, 0x63, 0x61, 0x6c, 0x6c, 0x00, 0x01,
    0x04, 0x73, 0x69, 0x7a, 0x65, 0x00, 0x02,

    /* active element: table[0] = function 0 */
    0x09, 0x07,
    0x01,
    0x00,
    0x41, 0x00, 0x0b,
    0x01, 0x00,

    0x0a, 0x14,
    0x03,

    /* target() -> 42 */
    0x04, 0x00, 0x41, 0x2a, 0x0b,

    /* call(): call_indirect type0 at table[0] */
    0x07, 0x00, 0x41, 0x00, 0x11, 0x00, 0x00, 0x0b,

    /* size(): table.size 0 */
    0x05, 0x00, 0xfc, 0x10, 0x00, 0x0b
};

static const uint8_t consumer_bytes[] = {
    WASM_HEADER,

    /* type0 [] -> i32; type1 [] -> [] */
    0x01, 0x08,
    0x02,
    0x60, 0x00, 0x01, 0x7f,
    0x60, 0x00, 0x00,

    /* import p.tab, funcref min=1 max=3 */
    0x02, 0x0c,
    0x01,
    0x01, 0x70,
    0x03, 0x74, 0x61, 0x62,
    0x01, 0x70, 0x01, 0x01, 0x03,

    /* call, local-target, set-local, grow, size */
    0x03, 0x06,
    0x05, 0x00, 0x00, 0x01, 0x00, 0x00,

    /* export local-target so ref.func is declared */
    0x07, 0x09,
    0x01,
    0x05, 0x6c, 0x6f, 0x63, 0x61, 0x6c,
    0x00, 0x01,

    0x0a, 0x27,
    0x05,

    /* call(): shared table[0] */
    0x07, 0x00, 0x41, 0x00, 0x11, 0x00, 0x00, 0x0b,

    /* local-target() -> 7 */
    0x04, 0x00, 0x41, 0x07, 0x0b,

    /* set-local(): table[0] = ref.func local-target */
    0x08, 0x00,
          0x41, 0x00,
          0xd2, 0x01,
          0x26, 0x00,
          0x0b,

    /* grow(): table.grow by 1 with null */
    0x09, 0x00,
          0xd0, 0x70,
          0x41, 0x01,
          0xfc, 0x0f, 0x00,
          0x0b,

    /* size(): table.size 0 */
    0x05, 0x00, 0xfc, 0x10, 0x00, 0x0b
};

static const uint8_t min_mismatch_consumer_bytes[] = {
    WASM_HEADER,

    /* import p.tab, funcref min=2 max=3 */
    0x02, 0x0c,
    0x01,
    0x01, 0x70,
    0x03, 0x74, 0x61, 0x62,
    0x01, 0x70, 0x01, 0x02, 0x03
};

static const uint8_t active_element_consumer_bytes[] = {
    WASM_HEADER,

    /* type0 [] -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,

    /* import p.tab, funcref min=1 max=3 */
    0x02, 0x0c,
    0x01,
    0x01, 0x70,
    0x03, 0x74, 0x61, 0x62,
    0x01, 0x70, 0x01, 0x01, 0x03,

    /* local target */
    0x03, 0x02,
    0x01, 0x00,

    /* export target so ref is declared */
    0x07, 0x0a,
    0x01,
    0x06, 0x74, 0x61, 0x72, 0x67, 0x65, 0x74,
    0x00, 0x00,

    /* active element writes local function 0 into imported table slot 0 */
    0x09, 0x07,
    0x01,
    0x00,
    0x41, 0x00, 0x0b,
    0x01, 0x00,

    /* target() -> 9 */
    0x0a, 0x06,
    0x01,
    0x04, 0x00, 0x41, 0x09, 0x0b
};

static turbowasm_name provider_name(void) {
    static const uint8_t p = (uint8_t)'p';
    turbowasm_name name = {&p, 1u};
    return name;
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void invoke_void(
    turbowasm_instance *instance,
    uint32_t function_index) {
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static void test_shared_table_is_one_live_object(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker, provider_name(),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_OK);

    /* Provider-created table entry dispatches correctly from consumer. */
    assert(invoke_i32(&consumer, 0u) == 42);

    /*
     * Consumer installs its own local function into the provider-owned table.
     * Provider then observes the same entry and dispatches back to consumer.
     */
    invoke_void(&consumer, 2u);
    assert(invoke_i32(&provider, 1u) == 7);
    assert(invoke_i32(&consumer, 0u) == 7);

    /* Grow through consumer mutates the same provider-owned table object. */
    assert(invoke_i32(&consumer, 3u) == 1);
    assert(invoke_i32(&provider, 2u) == 2);
    assert(invoke_i32(&consumer, 4u) == 2);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_table_link_uses_runtime_current_minimum(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module grower_module = {0};
    turbowasm_module second_consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance grower = {0};
    turbowasm_instance second_consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
               &grower_module,
               consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &second_consumer_module,
               min_mismatch_consumer_bytes,
               sizeof(min_mismatch_consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker, provider_name(),
               &provider) == TURBOWASM_OK);

    assert(turbowasm_instance_create_linked(
               &second_consumer,
               &second_consumer_module,
               &linker) == TURBOWASM_TYPE_MISMATCH);
    assert(second_consumer.impl == NULL);

    assert(turbowasm_instance_create_linked(
               &grower,
               &grower_module,
               &linker) == TURBOWASM_OK);

    /* table.grow mutates the provider-owned table from size 1 to size 2. */
    assert(invoke_i32(&grower, 3u) == 1);
    assert(invoke_i32(&provider, 2u) == 2);

    turbowasm_instance_destroy(&grower);

    /*
     * Provider declaration remains min=1, while the exported external table
     * now has current minimum 2. Import min=2 must therefore be admitted.
     */
    assert(turbowasm_instance_create_linked(
               &second_consumer,
               &second_consumer_module,
               &linker) == TURBOWASM_OK);

    turbowasm_instance_destroy(&second_consumer);
    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&second_consumer_module);
    turbowasm_module_destroy(&grower_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_table_limits_mismatch_rejected(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               min_mismatch_consumer_bytes,
               sizeof(min_mismatch_consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker, provider_name(),
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

static void test_active_element_initializes_imported_table(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(invoke_i32(&provider, 1u) == 42);

    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               active_element_consumer_bytes,
               sizeof(active_element_consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker, provider_name(),
               &provider) == TURBOWASM_OK);

    /*
     * Instance initialization applies the active consumer element segment to
     * the imported/provider-owned table before returning.
     */
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_OK);
    assert(invoke_i32(&provider, 1u) == 9);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_shared_table_is_one_live_object();
    test_table_link_uses_runtime_current_minimum();
    test_table_limits_mismatch_rejected();
    test_active_element_initializes_imported_table();
    return 0;
}
