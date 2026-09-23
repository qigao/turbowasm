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

static turbowasm_status load(
    const uint8_t *bytes,
    size_t size,
    turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_I32;
    result.as.i32 = value;
    return result;
}

static turbowasm_value f32_value(float value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_F32;
    result.as.f32 = value;
    return result;
}

static void test_i32_arithmetic(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x09, 0x01, 0x07,
        0x00, 0x41, 0x02, 0x41, 0x03, 0x6a, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    uint32_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_call(
               &instance, 0u, NULL, 0u,
               &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 5);
    assert(trap == TURBOWASM_TRAP_NONE);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_parameter_local(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x06, 0x01, 0x04,
        0x00, 0x20, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = i32_value(42);
    turbowasm_value result = {0};
    uint32_t result_count = 0u;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_call(
               &instance, 0u, &argument, 1u,
               &result, 1u, &result_count, NULL) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 42);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_nested_local_call(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x03, 0x02, 0x00, 0x00,
        0x0a, 0x0b, 0x02,
        0x04, 0x00, 0x10, 0x01, 0x0b,
        0x04, 0x00, 0x41, 0x07, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    uint32_t result_count = 0u;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_call(
               &instance, 0u, NULL, 0u,
               &result, 1u, &result_count, NULL) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 7);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_explicit_return_discards_extra_operands(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x09, 0x01, 0x07,
        0x00,
        0x41, 0x01,
        0x41, 0x02,
        0x0f,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    uint32_t result_count = 0u;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_call(
               &instance, 0u, NULL, 0u,
               &result, 1u, &result_count, NULL) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 2);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_argument_contract(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x06, 0x01, 0x04,
        0x00, 0x20, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value wrong = f32_value(1.0f);
    turbowasm_value result = {0};
    uint32_t result_count = 0u;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_call(
               &instance, 0u, NULL, 0u,
               &result, 1u, &result_count, NULL) ==
           TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_instance_call(
               &instance, 0u, &wrong, 1u,
               &result, 1u, &result_count, NULL) ==
           TURBOWASM_TYPE_MISMATCH);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_unreachable_trap(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x05, 0x01, 0x03,
        0x00, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    uint32_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_call(
               &instance, 0u, NULL, 0u,
               NULL, 0u, &result_count, &trap) == TURBOWASM_TRAPPED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_UNREACHABLE);
    assert(turbowasm_trap_string(trap) != NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_call_stack_exhaustion(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x06, 0x01, 0x04,
        0x00, 0x10, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    uint32_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_call(
               &instance, 0u, NULL, 0u,
               NULL, 0u, &result_count, &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_CALL_STACK_EXHAUSTED);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_two_instances_share_immutable_module(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x06, 0x01, 0x04,
        0x00, 0x41, 0x09, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance first = {0};
    turbowasm_instance second = {0};
    turbowasm_value first_result = {0};
    turbowasm_value second_result = {0};
    uint32_t first_count = 0u;
    uint32_t second_count = 0u;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&first, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&second, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_module(&first) == &module);
    assert(turbowasm_instance_module(&second) == &module);

    assert(turbowasm_instance_call(
               &first, 0u, NULL, 0u,
               &first_result, 1u, &first_count, NULL) == TURBOWASM_OK);
    assert(turbowasm_instance_call(
               &second, 0u, NULL, 0u,
               &second_result, 1u, &second_count, NULL) == TURBOWASM_OK);
    assert(first_count == 1u);
    assert(second_count == 1u);
    assert(first_result.as.i32 == 9);
    assert(second_result.as.i32 == 9);

    turbowasm_instance_destroy(&second);
    turbowasm_instance_destroy(&first);
    turbowasm_module_destroy(&module);
}

static void test_imported_call_is_explicitly_unsupported(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
        0x02, 0x07,
        0x01, 0x01, 0x6d, 0x01, 0x66, 0x00, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    uint32_t result_count = 0u;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_call(
               &instance, 0u, NULL, 0u,
               &result, 1u, &result_count, NULL) ==
           TURBOWASM_UNSUPPORTED);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_start_module_fails_closed_for_instance(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x08, 0x01, 0x00,
        0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) ==
           TURBOWASM_UNSUPPORTED);
    assert(instance.impl == NULL);

    turbowasm_module_destroy(&module);
}

int main(void) {
    test_i32_arithmetic();
    test_parameter_local();
    test_nested_local_call();
    test_explicit_return_discards_extra_operands();
    test_argument_contract();
    test_unreachable_trap();
    test_call_stack_exhaustion();
    test_two_instances_share_immutable_module();
    test_imported_call_is_explicitly_unsupported();
    test_start_module_fails_closed_for_instance();
    return 0;
}
