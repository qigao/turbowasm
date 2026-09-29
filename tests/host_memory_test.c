#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

typedef struct fill_context {
    uint32_t memory_index;
    uint32_t calls;
} fill_context;

static turbowasm_name name_span(const char *text, uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_status host_fill(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    fill_context *fill = (fill_context *)context;
    turbowasm_host_memory_span span = {0};
    turbowasm_status status;
    uint32_t address;
    uint32_t length;

    assert(fill != NULL);
    assert(call != NULL);
    assert(arguments != NULL && argument_count == 2u);
    assert(results != NULL && result_capacity >= 1u);
    assert(result_count != NULL && trap != NULL);
    assert(arguments[0].kind == TURBOWASM_VALUE_I32);
    assert(arguments[1].kind == TURBOWASM_VALUE_I32);

    ++fill->calls;
    address = (uint32_t)arguments[0].as.i32;
    length = (uint32_t)arguments[1].as.i32;

    status = turbowasm_host_call_memory_span(
        call,
        fill->memory_index,
        address,
        length,
        &span,
        trap);
    if (status != TURBOWASM_OK)
        return status;

    assert(span.size == length);
    if (length != 0u) {
        assert(span.data != NULL);
        memset(span.data, 0x5a, length);
    }

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = (int32_t)length;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static const turbowasm_value_kind fill_params[] = {
    TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32
};
static const turbowasm_value_kind fill_results[] = {
    TURBOWASM_VALUE_I32
};

static const uint8_t local_memory_module[] = {
    WASM_HEADER,

    /* type0: (i32,i32)->i32; type1: ()->i32 */
    0x01, 0x0b, 0x02,
    0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x00, 0x01, 0x7f,

    /* import host.fill type0 */
    0x02, 0x0d, 0x01,
    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x04, 0x66, 0x69, 0x6c, 0x6c,
    0x00, 0x00,

    /* one local reader type1 */
    0x03, 0x02, 0x01, 0x01,

    /* memory0 min=1 */
    0x05, 0x03, 0x01, 0x00, 0x01,

    /* f1: i32.const 10; i32.load8_u */
    0x0a, 0x09, 0x01, 0x07,
    0x00,
    0x41, 0x0a,
    0x2d, 0x00, 0x00,
    0x0b
};

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_status expected_status,
    turbowasm_trap expected_trap) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status = turbowasm_instance_invoke(
        instance, function_index,
        arguments, argument_count,
        &result, 1u, &result_count, &trap);

    assert(status == expected_status);
    assert(trap == expected_trap);
    if (status != TURBOWASM_OK)
        return 0;

    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void test_local_memory_span_and_bounds(void) {
    const turbowasm_host_function_type type = {
        fill_params, 2u, fill_results, 1u
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    fill_context fill = {0};
    turbowasm_value args[2] = {{0}};

    assert(turbowasm_module_load_borrowed(
               &module,
               local_memory_module,
               sizeof(local_memory_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_span("host", 4u),
               name_span("fill", 4u),
               &type,
               host_fill,
               &fill) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);

    args[0].kind = TURBOWASM_VALUE_I32;
    args[0].as.i32 = 10;
    args[1].kind = TURBOWASM_VALUE_I32;
    args[1].as.i32 = 4;
    assert(invoke_i32(
               &instance, 0u, args, 2u,
               TURBOWASM_OK, TURBOWASM_TRAP_NONE) == 4);
    assert(invoke_i32(
               &instance, 1u, NULL, 0u,
               TURBOWASM_OK, TURBOWASM_TRAP_NONE) == 0x5a);

    /* End-of-memory zero-length range is valid. */
    args[0].as.i32 = 65536;
    args[1].as.i32 = 0;
    assert(invoke_i32(
               &instance, 0u, args, 2u,
               TURBOWASM_OK, TURBOWASM_TRAP_NONE) == 0);

    /* One byte beyond the end traps through the canonical memory trap. */
    args[0].as.i32 = 65535;
    args[1].as.i32 = 2;
    (void)invoke_i32(
        &instance, 0u, args, 2u,
        TURBOWASM_TRAPPED,
        TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    /* Invalid memory index is a host contract error, not a forged trap. */
    fill.memory_index = 1u;
    args[0].as.i32 = 0;
    args[1].as.i32 = 1;
    (void)invoke_i32(
        &instance, 0u, args, 2u,
        TURBOWASM_INVALID_ARGUMENT,
        TURBOWASM_TRAP_NONE);

    assert(fill.calls == 4u);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static const uint8_t provider_module_bytes[] = {
    WASM_HEADER,

    /* type0: ()->i32 */
    0x01, 0x05, 0x01,
    0x60, 0x00, 0x01, 0x7f,

    /* local reader type0 */
    0x03, 0x02, 0x01, 0x00,

    /* memory0 min=1 */
    0x05, 0x03, 0x01, 0x00, 0x01,

    /* export memory as "mem" */
    0x07, 0x07, 0x01,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x00,

    /* f0: i32.const 10; i32.load8_u */
    0x0a, 0x09, 0x01, 0x07,
    0x00,
    0x41, 0x0a,
    0x2d, 0x00, 0x00,
    0x0b
};

static const uint8_t imported_memory_consumer_bytes[] = {
    WASM_HEADER,

    /* type0: (i32,i32)->i32 */
    0x01, 0x07, 0x01,
    0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,

    /* import host.fill function0 and p.mem memory0 */
    0x02, 0x16, 0x02,

    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x04, 0x66, 0x69, 0x6c, 0x6c,
    0x00, 0x00,

    0x01, 0x70,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x00, 0x01
};

static void test_imported_memory_span_is_provider_live_object(void) {
    const turbowasm_host_function_type type = {
        fill_params, 2u, fill_results, 1u
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};
    fill_context fill = {0};
    turbowasm_value args[2] = {{0}};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_module_bytes,
               sizeof(provider_module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               imported_memory_consumer_bytes,
               sizeof(imported_memory_consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span("p", 1u),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_span("host", 4u),
               name_span("fill", 4u),
               &type,
               host_fill,
               &fill) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_OK);

    args[0].kind = TURBOWASM_VALUE_I32;
    args[0].as.i32 = 10;
    args[1].kind = TURBOWASM_VALUE_I32;
    args[1].as.i32 = 1;
    assert(invoke_i32(
               &consumer, 0u, args, 2u,
               TURBOWASM_OK, TURBOWASM_TRAP_NONE) == 1);

    assert(invoke_i32(
               &provider, 0u, NULL, 0u,
               TURBOWASM_OK, TURBOWASM_TRAP_NONE) == 0x5a);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_local_memory_span_and_bounds();
    test_imported_memory_span_is_provider_live_object();
    return 0;
}
