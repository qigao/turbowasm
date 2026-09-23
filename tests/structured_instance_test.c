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

#define FUNCTION_TYPE0 \
    0x03, 0x02, 0x01, 0x00

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_I32;
    result.as.i32 = value;
    return result;
}

static turbowasm_status invoke_i32(
    const uint8_t *bytes,
    size_t size,
    const turbowasm_value *arguments,
    size_t argument_count,
    int32_t *out) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    status = turbowasm_module_load_borrowed(&module, bytes, size);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_instance_create(&instance, &module);
    if (status != TURBOWASM_OK) {
        turbowasm_module_destroy(&module);
        return status;
    }

    status = turbowasm_instance_invoke(
        &instance, 0u,
        arguments, argument_count,
        &result, 1u,
        &result_count,
        &trap);
    if (status == TURBOWASM_OK) {
        assert(trap == TURBOWASM_TRAP_NONE);
        assert(result_count == 1u);
        assert(result.kind == TURBOWASM_VALUE_I32);
        *out = result.as.i32;
    }

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return status;
}

static void test_if_else_paths(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        FUNCTION_TYPE0,
        0x0a, 0x0e,
        0x01, 0x0c,
        0x00,
        0x20, 0x00,
        0x04, 0x7f,
        0x41, 0x07,
        0x05,
        0x41, 0x09,
        0x0b,
        0x0b
    };
    turbowasm_value true_arg = i32_value(1);
    turbowasm_value false_arg = i32_value(0);
    int32_t result = 0;

    assert(invoke_i32(bytes, sizeof(bytes),
                      &true_arg, 1u, &result) == TURBOWASM_OK);
    assert(result == 7);

    assert(invoke_i32(bytes, sizeof(bytes),
                      &false_arg, 1u, &result) == TURBOWASM_OK);
    assert(result == 9);
}

static void test_br_exits_block_and_skips_trap(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        FUNCTION_TYPE0,
        0x0a, 0x0c,
        0x01, 0x0a,
        0x00,
        0x02, 0x7f,
        0x41, 0x2a,
        0x0c, 0x00,
        0x00,
        0x0b,
        0x0b
    };
    int32_t result = 0;

    assert(invoke_i32(bytes, sizeof(bytes),
                      NULL, 0u, &result) == TURBOWASM_OK);
    assert(result == 42);
}

static void test_br_if_preserves_fallthrough_value(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        FUNCTION_TYPE0,
        0x0a, 0x10,
        0x01, 0x0e,
        0x00,
        0x02, 0x7f,
        0x41, 0x07,
        0x20, 0x00,
        0x0d, 0x00,
        0x1a,
        0x41, 0x09,
        0x0b,
        0x0b
    };
    turbowasm_value true_arg = i32_value(1);
    turbowasm_value false_arg = i32_value(0);
    int32_t result = 0;

    assert(invoke_i32(bytes, sizeof(bytes),
                      &true_arg, 1u, &result) == TURBOWASM_OK);
    assert(result == 7);

    assert(invoke_i32(bytes, sizeof(bytes),
                      &false_arg, 1u, &result) == TURBOWASM_OK);
    assert(result == 9);
}

static void test_loop_backedge(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        FUNCTION_TYPE0,
        0x0a, 0x12,
        0x01, 0x10,
        0x00,
        0x03, 0x40,
        0x20, 0x00,
        0x41, 0x01,
        0x6b,
        0x22, 0x00,
        0x0d, 0x00,
        0x0b,
        0x20, 0x00,
        0x0b
    };
    turbowasm_value arg = i32_value(3);
    int32_t result = -1;

    assert(invoke_i32(bytes, sizeof(bytes),
                      &arg, 1u, &result) == TURBOWASM_OK);
    assert(result == 0);
}

static void test_br_table_targets(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        FUNCTION_TYPE0,
        0x0a, 0x12,
        0x01, 0x10,
        0x00,
        0x02, 0x7f,
        0x02, 0x7f,
        0x41, 0x0b,
        0x20, 0x00,
        0x0e, 0x01, 0x00, 0x01,
        0x0b,
        0x0b,
        0x0b
    };
    turbowasm_value inner = i32_value(0);
    turbowasm_value outer = i32_value(1);
    int32_t result = 0;

    assert(invoke_i32(bytes, sizeof(bytes),
                      &inner, 1u, &result) == TURBOWASM_OK);
    assert(result == 11);

    assert(invoke_i32(bytes, sizeof(bytes),
                      &outer, 1u, &result) == TURBOWASM_OK);
    assert(result == 11);
}

static void test_typeidx_multi_value_block(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x0d,
        0x02,
        0x60, 0x00, 0x02, 0x7f, 0x7e,
        0x60, 0x02, 0x7f, 0x7e, 0x02, 0x7f, 0x7e,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x0b,
        0x01, 0x09,
        0x00,
        0x41, 0x07,
        0x42, 0x08,
        0x02, 0x01,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value results[2] = {{0}};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               results, 2u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 2u);
    assert(results[0].kind == TURBOWASM_VALUE_I32);
    assert(results[0].as.i32 == 7);
    assert(results[1].kind == TURBOWASM_VALUE_I64);
    assert(results[1].as.i64 == 8);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_if_else_paths();
    test_br_exits_block_and_skips_trap();
    test_br_if_preserves_fallthrough_value();
    test_loop_backedge();
    test_br_table_targets();
    test_typeidx_multi_value_block();
    return 0;
}
