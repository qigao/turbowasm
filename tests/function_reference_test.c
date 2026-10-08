#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

enum { MODULE_CAPACITY = 256, MAX_BODY_SIZE = 120, TARGET_RESULT = 42 };

/* A bounded fixture: function 0 returns 42; function 1 takes (ref null 0).
 * Function 0 is declared in an element segment for ref.func validation. */
static size_t make_module(uint8_t *bytes, const uint8_t *body, size_t size) {
    static const uint8_t prefix[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x0b, 0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x01, 0x63, 0x00, 0x01, 0x7f,
        0x03, 0x03, 0x02, 0x00, 0x01,
        0x09, 0x05, 0x01, 0x03, 0x00, 0x01, 0x00
    };
    static const uint8_t target[] = {0x02, 0x04, 0x00, 0x41, 0x2a, 0x0b};
    size_t used = sizeof(prefix);
    assert(size <= MAX_BODY_SIZE);
    memcpy(bytes, prefix, used);
    bytes[used++] = 0x0a;
    bytes[used++] = (uint8_t)(sizeof(target) + 1u + size);
    memcpy(bytes + used, target, sizeof(target));
    used += sizeof(target);
    bytes[used++] = (uint8_t)size;
    memcpy(bytes + used, body, size);
    return used + size;
}

static void check_body(const uint8_t *body, size_t size, bool null_argument,
                       turbowasm_status expected, int32_t expected_value) {
    uint8_t bytes[MODULE_CAPACITY];
    size_t module_size = make_module(bytes, body, size);
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0}, result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t result_count = 0u;
    turbowasm_status status = turbowasm_module_load_borrowed(
        &module, bytes, module_size);
    if (expected == TURBOWASM_MALFORMED_MODULE) {
        assert(status == expected);
        assert(module.impl == NULL);
        return;
    }
    assert(status == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    argument.kind = TURBOWASM_VALUE_FUNCREF;
    argument.as.funcref.is_null = null_argument;
    argument.as.funcref.function_index = 0u;
    assert(turbowasm_instance_invoke(
        &instance, 1u, &argument, 1u, &result, 1u,
        &result_count, &trap) == expected);
    if (expected == TURBOWASM_OK) {
        assert(result_count == 1u);
        assert(result.kind == TURBOWASM_VALUE_I32);
        assert(result.as.i32 == expected_value);
        assert(trap == TURBOWASM_TRAP_NONE);
    } else {
        assert(trap == TURBOWASM_TRAP_NULL_REFERENCE);
    }
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_reference_calls(void) {
    static const uint8_t call[] = {0x00, 0x20, 0x00, 0x14, 0x00, 0x0b};
    static const uint8_t tail[] = {0x00, 0x20, 0x00, 0x15, 0x00, 0x0b};
    static const uint8_t nonnull[] = {
        0x00, 0x20, 0x00, 0xd4, 0x14, 0x00, 0x0b
    };
    check_body(call, sizeof(call), false, TURBOWASM_OK, TARGET_RESULT);
    check_body(call, sizeof(call), true, TURBOWASM_TRAPPED, 0);
    check_body(tail, sizeof(tail), false, TURBOWASM_OK, TARGET_RESULT);
    check_body(tail, sizeof(tail), true, TURBOWASM_TRAPPED, 0);
    check_body(nonnull, sizeof(nonnull), false, TURBOWASM_OK, TARGET_RESULT);
    check_body(nonnull, sizeof(nonnull), true, TURBOWASM_TRAPPED, 0);
}

static void test_reference_branches(void) {
    static const uint8_t on_null[] = {
        0x00, 0x41, 0x07, 0x20, 0x00, 0xd5, 0x00,
        0x14, 0x00, 0x6a, 0x0b
    };
    static const uint8_t on_nonnull[] = {
        0x00, 0x02, 0x64, 0x00,
        0x20, 0x00, 0xd6, 0x00, 0x41, 0x07, 0x0f,
        0x0b, 0x14, 0x00, 0x0b
    };
    check_body(on_null, sizeof(on_null), true, TURBOWASM_OK, 7);
    check_body(on_null, sizeof(on_null), false, TURBOWASM_OK, TARGET_RESULT + 7);
    check_body(on_nonnull, sizeof(on_nonnull), true, TURBOWASM_OK, 7);
    check_body(on_nonnull, sizeof(on_nonnull), false, TURBOWASM_OK, TARGET_RESULT);
}

static void test_local_initialization(void) {
    static const uint8_t initialized[] = {
        0x01, 0x01, 0x64, 0x00,
        0xd2, 0x00, 0x21, 0x01, 0x20, 0x01, 0x14, 0x00, 0x0b
    };
    static const uint8_t tee[] = {
        0x01, 0x01, 0x64, 0x00,
        0xd2, 0x00, 0x22, 0x01, 0x1a, 0x20, 0x01, 0x14, 0x00, 0x0b
    };
    static const uint8_t uninitialized[] = {
        0x01, 0x01, 0x64, 0x00, 0x20, 0x01, 0x14, 0x00, 0x0b
    };
    static const uint8_t block_local[] = {
        0x01, 0x01, 0x64, 0x00,
        0x02, 0x40, 0xd2, 0x00, 0x21, 0x01, 0x0b,
        0x20, 0x01, 0x14, 0x00, 0x0b
    };
    static const uint8_t else_local[] = {
        0x01, 0x01, 0x64, 0x00,
        0x41, 0x01, 0x04, 0x40, 0xd2, 0x00, 0x21, 0x01,
        0x05, 0x20, 0x01, 0x1a, 0x0b, 0x41, 0x00, 0x0b
    };
    check_body(initialized, sizeof(initialized), true, TURBOWASM_OK, TARGET_RESULT);
    check_body(tee, sizeof(tee), true, TURBOWASM_OK, TARGET_RESULT);
    check_body(uninitialized, sizeof(uninitialized), true, TURBOWASM_MALFORMED_MODULE, 0);
    check_body(block_local, sizeof(block_local), true, TURBOWASM_MALFORMED_MODULE, 0);
    check_body(else_local, sizeof(else_local), true, TURBOWASM_MALFORMED_MODULE, 0);
}

static void test_bottom_references(void) {
    static const uint8_t nofunc[] = {0x00, 0xd0, 0x73, 0x14, 0x00, 0x0b};
    static const uint8_t noextern[] = {0x00, 0xd0, 0x72, 0xd1, 0x0b};
    static const uint8_t invalid_nonnull[] = {0x00, 0x41, 0x00, 0xd4, 0x0b};
    static const uint8_t wrong_signature[] = {0x00, 0xd2, 0x00, 0x14, 0x01, 0x0b};
    static const uint8_t unknown_type[] = {0x00, 0xd2, 0x00, 0x14, 0x02, 0x0b};
    check_body(nofunc, sizeof(nofunc), true, TURBOWASM_TRAPPED, 0);
    check_body(noextern, sizeof(noextern), true, TURBOWASM_OK, 1);
    check_body(invalid_nonnull, sizeof(invalid_nonnull), true, TURBOWASM_MALFORMED_MODULE, 0);
    check_body(wrong_signature, sizeof(wrong_signature), true, TURBOWASM_MALFORMED_MODULE, 0);
    check_body(unknown_type, sizeof(unknown_type), true, TURBOWASM_MALFORMED_MODULE, 0);
}

static void test_tail_reference_reuses_frame(void) {
    static const uint8_t bytes[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x09, 0x05, 0x01, 0x03, 0x00, 0x01, 0x00,
        0x0a, 0x16, 0x01, 0x14,
        0x00, 0x20, 0x00, 0x45, 0x04, 0x7f, 0x41, 0x00,
        0x05, 0x20, 0x00, 0x41, 0x01, 0x6b, 0xd2, 0x00,
        0x15, 0x00, 0x0b, 0x0b
    };
    enum { ITERATIONS = 10000 };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0}, result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t count;
    assert(turbowasm_module_load_borrowed(&module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    argument.kind = TURBOWASM_VALUE_I32;
    argument.as.i32 = ITERATIONS;
    assert(turbowasm_instance_invoke(&instance, 0u, &argument, 1u,
        &result, 1u, &count, &trap) == TURBOWASM_OK);
    assert(count == 1u && result.as.i32 == 0 && trap == TURBOWASM_TRAP_NONE);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_host_reference_type_admission(void) {
    static const uint8_t body[] = {0x00, 0x20, 0x00, 0x14, 0x00, 0x0b};
    enum { PARAMETER_REFERENCE_FORM_OFFSET = 17 };
    uint8_t bytes[MODULE_CAPACITY];
    size_t size = make_module(bytes, body, sizeof(body));
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0}, result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t count;
    bytes[PARAMETER_REFERENCE_FORM_OFFSET] = 0x64;
    assert(turbowasm_module_load_borrowed(&module, bytes, size) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    argument.kind = TURBOWASM_VALUE_FUNCREF;
    argument.as.funcref.is_null = true;
    assert(turbowasm_instance_invoke(&instance, 1u, &argument, 1u,
        &result, 1u, &count, &trap) == TURBOWASM_TYPE_MISMATCH);
    argument.as.funcref.is_null = false;
    argument.as.funcref.function_index = 1u;
    assert(turbowasm_instance_invoke(&instance, 1u, &argument, 1u,
        &result, 1u, &count, &trap) == TURBOWASM_TYPE_MISMATCH);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_reference_calls();
    test_reference_branches();
    test_local_initialization();
    test_bottom_references();
    test_tail_reference_reuses_frame();
    test_host_reference_type_admission();
    return 0;
}
