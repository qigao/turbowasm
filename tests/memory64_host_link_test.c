#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static turbowasm_name name(const char *bytes, uint32_t size) {
    turbowasm_name out = {(const uint8_t *)bytes, size};
    return out;
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
               &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static int64_t invoke_i64(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I64);
    return result.as.i64;
}

static turbowasm_status host_read(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_host_memory_span span = {0};
    uint64_t address;
    turbowasm_status status;

    (void)context;
    if (arguments == NULL || argument_count != 1u ||
        results == NULL || result_capacity < 1u ||
        result_count == NULL || trap == NULL ||
        arguments[0].kind != TURBOWASM_VALUE_I64)
        return TURBOWASM_INVALID_ARGUMENT;

    address = (uint64_t)arguments[0].as.i64;
    status = turbowasm_host_call_memory_span64(
        call, 0u, address, 1u, &span, trap);
    if (status != TURBOWASM_OK)
        return status;

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = span.data[0];
    *result_count = 1u;
    return TURBOWASM_OK;
}

static void test_host_span64(void) {
    static const uint8_t bytes[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x0a, 0x02,
        0x60, 0x01, 0x7e, 0x01, 0x7f,
        0x60, 0x00, 0x01, 0x7f,
        0x02, 0x0d, 0x01,
        0x04, 0x68, 0x6f, 0x73, 0x74,
        0x04, 0x72, 0x65, 0x61, 0x64,
        0x00, 0x00,
        0x03, 0x02, 0x01, 0x01,
        /* memory64 min=1 */
        0x05, 0x03, 0x01, 0x04, 0x01,
        0x0a, 0x08, 0x01,
        0x06, 0x00, 0x42, 0x07, 0x10, 0x00, 0x0b,
        /* active data at i64 offset 7 */
        0x0b, 0x07, 0x01,
        0x00, 0x42, 0x07, 0x0b, 0x01, 0x6b
    };
    const turbowasm_value_kind params[] = {TURBOWASM_VALUE_I64};
    const turbowasm_value_kind results[] = {TURBOWASM_VALUE_I32};
    const turbowasm_host_function_type type = {
        params, 1u, results, 1u
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name("host", 4u),
               name("read", 4u),
               &type, host_read, NULL) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);
    assert(invoke_i32(&instance, 1u) == 0x6b);

    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
}

static void test_imported_memory64(void) {
    static const uint8_t provider_bytes[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x05, 0x04, 0x01, 0x05, 0x01, 0x02,
        0x07, 0x07, 0x01,
        0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00
    };
    static const uint8_t consumer_bytes[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7e,
        0x02, 0x0b, 0x01,
        0x01, 0x70, 0x03, 0x6d, 0x65, 0x6d,
        0x02, 0x05, 0x01, 0x02,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x06, 0x01, 0x04, 0x00, 0x3f, 0x00, 0x0b
    };
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
               &linker, name("p", 1u), &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer,
               &consumer_module,
               &linker) == TURBOWASM_OK);
    assert(invoke_i64(&consumer, 0u) == 1);

    turbowasm_instance_destroy(&consumer);
    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_host_span64();
    test_imported_memory64();
    return 0;
}
