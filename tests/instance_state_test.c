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

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_I32;
    result.as.i32 = value;
    return result;
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               arguments, argument_count,
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
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               arguments, argument_count,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static void test_globals_are_instance_local(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x09,
        0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x01, 0x7f, 0x00,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x06, 0x06,
        0x01, 0x7f, 0x01, 0x41, 0x01, 0x0b,
        0x0a, 0x0d,
        0x02,
        0x04, 0x00, 0x23, 0x00, 0x0b,
        0x06, 0x00, 0x20, 0x00, 0x24, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance a = {0};
    turbowasm_instance b = {0};
    turbowasm_value forty_two = i32_value(42);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&a, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&b, &module) == TURBOWASM_OK);

    assert(invoke_i32(&a, 0u, NULL, 0u) == 1);
    assert(invoke_i32(&b, 0u, NULL, 0u) == 1);
    invoke_void(&a, 1u, &forty_two, 1u);
    assert(invoke_i32(&a, 0u, NULL, 0u) == 42);
    assert(invoke_i32(&b, 0u, NULL, 0u) == 1);

    turbowasm_instance_destroy(&a);
    turbowasm_instance_destroy(&b);
    turbowasm_module_destroy(&module);
}

static void test_active_data_and_memory_are_instance_local(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x09,
        0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x01, 0x7f, 0x00,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0a, 0x13,
        0x02,
        0x07, 0x00, 0x41, 0x00, 0x28, 0x02, 0x00, 0x0b,
        0x09, 0x00, 0x41, 0x00, 0x20, 0x00,
              0x36, 0x02, 0x00, 0x0b,
        0x0b, 0x0a,
        0x01, 0x00, 0x41, 0x00, 0x0b,
        0x04, 0x2a, 0x00, 0x00, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_instance a = {0};
    turbowasm_instance b = {0};
    turbowasm_value ninety_nine = i32_value(99);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&a, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&b, &module) == TURBOWASM_OK);

    assert(invoke_i32(&a, 0u, NULL, 0u) == 42);
    assert(invoke_i32(&b, 0u, NULL, 0u) == 42);
    invoke_void(&a, 1u, &ninety_nine, 1u);
    assert(invoke_i32(&a, 0u, NULL, 0u) == 99);
    assert(invoke_i32(&b, 0u, NULL, 0u) == 42);

    turbowasm_instance_destroy(&a);
    turbowasm_instance_destroy(&b);
    turbowasm_module_destroy(&module);
}

static void test_memory_out_of_bounds_traps(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0a, 0x09,
        0x01, 0x07,
        0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value address = i32_value(65535);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               &address, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_memory_grow_returns_minus_one_on_limit(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x0a,
        0x02,
        0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x60, 0x00, 0x01, 0x7f,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x05, 0x04, 0x01, 0x01, 0x01, 0x02,
        0x0a, 0x0d,
        0x02,
        0x06, 0x00, 0x20, 0x00, 0x40, 0x00, 0x0b,
        0x04, 0x00, 0x3f, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value one = i32_value(1);

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(invoke_i32(&instance, 0u, &one, 1u) == 1);
    assert(invoke_i32(&instance, 1u, NULL, 0u) == 2);
    assert(invoke_i32(&instance, 0u, &one, 1u) == -1);
    assert(invoke_i32(&instance, 1u, NULL, 0u) == 2);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_active_element_enables_call_indirect(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x03, 0x02, 0x00, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x01,
        0x09, 0x07,
        0x01, 0x00, 0x41, 0x00, 0x0b, 0x01, 0x00,
        0x0a, 0x0e,
        0x02,
        0x04, 0x00, 0x41, 0x07, 0x0b,
        0x07, 0x00, 0x41, 0x00, 0x11, 0x00, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_i32(&instance, 1u, NULL, 0u) == 7);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_null_call_indirect_traps(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x01,
        0x0a, 0x09,
        0x01, 0x07,
        0x00, 0x41, 0x00, 0x11, 0x00, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_INDIRECT_CALL_NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_start_runs_after_initialization(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x08,
        0x02,
        0x60, 0x00, 0x00,
        0x60, 0x00, 0x01, 0x7f,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x06, 0x06,
        0x01, 0x7f, 0x01, 0x41, 0x01, 0x0b,
        0x08, 0x01, 0x00,
        0x0a, 0x0d,
        0x02,
        0x06, 0x00, 0x41, 0x05, 0x24, 0x00, 0x0b,
        0x04, 0x00, 0x23, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_i32(&instance, 1u, NULL, 0u) == 5);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_globals_are_instance_local();
    test_active_data_and_memory_are_instance_local();
    test_memory_out_of_bounds_traps();
    test_memory_grow_returns_minus_one_on_limit();
    test_active_element_enables_call_indirect();
    test_null_call_indirect_traps();
    test_start_runs_after_initialization();
    return 0;
}
