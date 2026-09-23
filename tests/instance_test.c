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

static turbowasm_status load_module(const uint8_t *bytes,
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

static void test_typed_arguments_and_arithmetic(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x07,
        0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,

        0x03, 0x02, 0x01, 0x00,

        0x0a, 0x09,
        0x01, 0x07,
        0x00,
        0x20, 0x00,
        0x20, 0x01,
        0x6a,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value args[2] = {i32_value(20), i32_value(22)};
    turbowasm_value results[1] = {{0}};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load_module(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               args, 2u,
               results, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(results[0].kind == TURBOWASM_VALUE_I32);
    assert(results[0].as.i32 == 42);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_locals_are_zero_initialized(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02, 0x01, 0x00,

        0x0a, 0x08,
        0x01, 0x06,
        0x01, 0x01, 0x7f,
        0x20, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load_module(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 0);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_nested_local_direct_call(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x03,
        0x02, 0x00, 0x00,

        0x0a, 0x0b,
        0x02,
        0x04, 0x00, 0x41, 0x07, 0x0b,
        0x04, 0x00, 0x10, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load_module(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 1u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 7);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}


static void test_return_discards_lower_temporaries(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02, 0x01, 0x00,

        0x0a, 0x0a,
        0x01, 0x08,
        0x00,
        0x41, 0x01,
        0x41, 0x02,
        0x0f,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load_module(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 2);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_wrong_argument_type_rejected(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

        0x03, 0x02, 0x01, 0x00,

        0x0a, 0x06,
        0x01, 0x04,
        0x00, 0x20, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value arg = f32_value(1.0f);
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load_module(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               &arg, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_TYPE_MISMATCH);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_unreachable_trap(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,

        0x03, 0x02, 0x01, 0x00,

        0x0a, 0x05,
        0x01, 0x03,
        0x00, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load_module(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_TRAPPED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_UNREACHABLE);
    assert(turbowasm_trap_string(trap) != NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_integer_divide_by_zero_trap(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02, 0x01, 0x00,

        0x0a, 0x09,
        0x01, 0x07,
        0x00,
        0x41, 0x01,
        0x41, 0x00,
        0x6d,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(load_module(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_two_instances_share_immutable_module(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02, 0x01, 0x00,

        0x0a, 0x06,
        0x01, 0x04,
        0x00, 0x41, 0x2a, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance a = {0};
    turbowasm_instance b = {0};
    turbowasm_value ra = {0};
    turbowasm_value rb = {0};
    size_t ca = 0u;
    size_t cb = 0u;
    turbowasm_trap ta = TURBOWASM_TRAP_NONE;
    turbowasm_trap tb = TURBOWASM_TRAP_NONE;

    assert(load_module(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&a, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&b, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_module(&a) == &module);
    assert(turbowasm_instance_module(&b) == &module);

    assert(turbowasm_instance_invoke(
               &a, 0u, NULL, 0u, &ra, 1u, &ca, &ta) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &b, 0u, NULL, 0u, &rb, 1u, &cb, &tb) == TURBOWASM_OK);
    assert(ca == 1u && cb == 1u);
    assert(ra.as.i32 == 42 && rb.as.i32 == 42);

    turbowasm_instance_destroy(&a);
    turbowasm_instance_destroy(&b);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_typed_arguments_and_arithmetic();
    test_locals_are_zero_initialized();
    test_nested_local_direct_call();
    test_return_discards_lower_temporaries();
    test_wrong_argument_type_rejected();
    test_unreachable_trap();
    test_integer_divide_by_zero_trap();
    test_two_instances_share_immutable_module();
    return 0;
}
