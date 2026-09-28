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

static void test_typed_throw_catch_returns_payload(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* type0: (i32)->(); type1: ()->i32 */
        0x01, 0x09, 0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x01, 0x7f,
        /* function0 type1 */
        0x03, 0x02, 0x01, 0x01,
        /* tag0 type0 */
        0x0d, 0x03, 0x01, 0x00, 0x00,
        /* code */
        0x0a, 0x13, 0x01, 0x11,
        0x00,
        0x02, 0x7f,                   /* block (result i32) */
        0x1f, 0x40, 0x01,             /* try_table [] + 1 catch */
        0x00, 0x00, 0x00,             /* catch tag0 -> label0 */
        0x41, 0x2a, 0x08, 0x00,       /* i32.const 42; throw 0 */
        0x0b,                          /* end try */
        0x41, 0x00,                    /* normal result */
        0x0b, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u, NULL, 0u,
               &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 42);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_tag_identity_not_structural_signature(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* type0: (i32)->(); type1: ()->i32 */
        0x01, 0x09, 0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x01,
        /* two distinct tags with the same signature */
        0x0d, 0x05, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x0a, 0x1b, 0x01, 0x19,
        0x00,
        0x02, 0x7f,                   /* outer result i32 */
        0x02, 0x7f,                   /* inner result i32 */
        0x1f, 0x40, 0x02,
        0x00, 0x00, 0x00,             /* tag0 -> inner */
        0x00, 0x01, 0x01,             /* tag1 -> outer */
        0x41, 0x07, 0x08, 0x01,       /* throw tag1 payload 7 */
        0x0b,
        0x41, 0x00,                    /* normal inner result */
        0x0b,
        0x41, 0x64, 0x6a,             /* +100 if tag0 matched wrongly */
        0x0b, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u, NULL, 0u,
               &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 7);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_catch_ref_returns_rethrowable_exnref(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* type0: ()->(); type1: ()->exnref */
        0x01, 0x09, 0x02,
        0x60, 0x00, 0x00,
        0x60, 0x00, 0x01, 0x69,
        0x03, 0x02, 0x01, 0x01,
        0x0d, 0x03, 0x01, 0x00, 0x00,
        0x0a, 0x12, 0x01, 0x10,
        0x00,
        0x02, 0x69,                   /* block (result exnref) */
        0x1f, 0x40, 0x01,
        0x01, 0x00, 0x00,             /* catch_ref tag0 -> label0 */
        0x08, 0x00,                   /* throw tag0 */
        0x0b,
        0xd0, 0x69,                   /* normal null exnref */
        0x0b, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u, NULL, 0u,
               &result, 1u, &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_EXNREF);
    assert(!result.as.exnref.is_null);
    assert(result.as.exnref.exception != NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_null_throw_ref_traps(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x08, 0x01, 0x06,
        0x00, 0xd0, 0x69, 0x0a, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u, NULL, 0u,
               NULL, 0u, &result_count, &trap) == TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_NULL_REFERENCE);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_uncaught_exception_is_not_a_trap(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x08, 0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x01,
        0x0d, 0x03, 0x01, 0x00, 0x00,
        0x0a, 0x08, 0x01, 0x06,
        0x00, 0x41, 0x01, 0x08, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u, NULL, 0u,
               NULL, 0u, &result_count, &trap) == TURBOWASM_EXCEPTION);
    assert(trap == TURBOWASM_TRAP_NONE);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_typed_throw_catch_returns_payload();
    test_tag_identity_not_structural_signature();
    test_catch_ref_returns_rethrowable_exnref();
    test_null_throw_ref_traps();
    test_uncaught_exception_is_not_a_trap();
    return 0;
}
