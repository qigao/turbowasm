#include "../src/module_internal.h"
#include "../src/validation_context.h"

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

static void expect_invalid(const uint8_t *bytes, size_t size) {
    turbowasm_module module = {0};

    assert(load(bytes, size, &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_throw_validates_defined_tag_payload(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: (i32, i64) -> (); type1: () -> () */
        0x01, 0x09,
        0x02,
        0x60, 0x02, 0x7f, 0x7e, 0x00,
        0x60, 0x00, 0x00,

        /* one function of type1 */
        0x03, 0x02,
        0x01, 0x01,

        /* tag0: type0 */
        0x0d, 0x03,
        0x01, 0x00, 0x00,

        /* i32.const 1; i64.const 2; throw tag0 */
        0x0a, 0x0a,
        0x01, 0x08,
        0x00,
        0x41, 0x01,
        0x42, 0x02,
        0x08, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_throw_validates_imported_tag_payload(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: (i32) -> (); type1: () -> () */
        0x01, 0x08,
        0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x00,

        /* import m.t as tag type0 */
        0x02, 0x08,
        0x01,
        0x01, 0x6d,
        0x01, 0x74,
        0x04, 0x00, 0x00,

        /* one function of type1 */
        0x03, 0x02,
        0x01, 0x01,

        /* i32.const 7; throw imported tag0 */
        0x0a, 0x08,
        0x01, 0x06,
        0x00,
        0x41, 0x07,
        0x08, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_throw_rejects_wrong_payload_and_tag_index(void) {
    static const uint8_t wrong_payload[] = {
        WASM_HEADER,
        0x01, 0x08,
        0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x00,
        0x03, 0x02,
        0x01, 0x01,
        0x0d, 0x03,
        0x01, 0x00, 0x00,
        0x0a, 0x08,
        0x01, 0x06,
        0x00,
        0x42, 0x01,
        0x08, 0x00,
        0x0b
    };
    static const uint8_t wrong_tag[] = {
        WASM_HEADER,
        0x01, 0x08,
        0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x00,
        0x03, 0x02,
        0x01, 0x01,
        0x0d, 0x03,
        0x01, 0x00, 0x00,
        0x0a, 0x08,
        0x01, 0x06,
        0x00,
        0x41, 0x01,
        0x08, 0x01,
        0x0b
    };

    expect_invalid(wrong_payload, sizeof(wrong_payload));
    expect_invalid(wrong_tag, sizeof(wrong_tag));
}

static void test_throw_ref_uses_exnref_semantics(void) {
    static const uint8_t valid[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x07,
        0x01, 0x05,
        0x00,
        0xd0, 0x69, /* ref.null exn */
        0x0a,       /* throw_ref */
        0x0b
    };
    static const uint8_t wrong_ref[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x07,
        0x01, 0x05,
        0x00,
        0xd0, 0x6f, /* ref.null extern */
        0x0a,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(valid, sizeof(valid), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);

    expect_invalid(wrong_ref, sizeof(wrong_ref));
}

static void test_try_table_catch_retains_metadata(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: (i32) -> (); type1: () -> i32 */
        0x01, 0x09,
        0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x01,

        0x0d, 0x03,
        0x01, 0x00, 0x00,

        /*
         * try_table (result i32) (catch tag0 0)
         *   i32.const 0
         * end
         */
        0x0a, 0x0d,
        0x01, 0x0b,
        0x00,
        0x1f, 0x7f,
        0x01,
        0x00, 0x00, 0x00,
        0x41, 0x00,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *function;
    const turbowasm_validation_control *control;

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);

    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    function = turbowasm_validation_context_function(
        &impl->validation, 0u);
    assert(function != NULL);
    assert(function->control_count == 1u);

    control = &function->controls[0];
    assert(control->kind == TURBOWASM_VALIDATION_CONTROL_TRY_TABLE);
    assert(control->opcode_offset == 0u);
    assert(control->body_offset == 6u);
    assert(control->end_offset == 8u);
    assert(control->catch_count == 1u);
    assert(control->catches != NULL);
    assert(control->catches[0].kind == TURBOWASM_VALIDATION_CATCH);
    assert(control->catches[0].tag_index == 0u);
    assert(control->catches[0].label_depth == 0u);

    turbowasm_module_destroy(&module);
}

static void test_try_table_catch_ref_and_catch_all_forms(void) {
    static const uint8_t catch_ref[] = {
        WASM_HEADER,

        /* type0: (i32) -> (); type1: () -> (i32, exnref) */
        0x01, 0x0a,
        0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x02, 0x7f, 0x69,

        0x03, 0x02,
        0x01, 0x01,

        0x0d, 0x03,
        0x01, 0x00, 0x00,

        0x0a, 0x0f,
        0x01, 0x0d,
        0x00,
        0x1f, 0x01,
        0x01,
        0x01, 0x00, 0x00,
        0x41, 0x00,
        0xd0, 0x69,
        0x0b,
        0x0b
    };
    static const uint8_t catch_all[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x0b,
        0x01, 0x09,
        0x00,
        0x1f, 0x40,
        0x01,
        0x02, 0x00,
        0x01,
        0x0b,
        0x0b
    };
    static const uint8_t catch_all_ref[] = {
        WASM_HEADER,

        /* type0: () -> exnref */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x69,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x0c,
        0x01, 0x0a,
        0x00,
        0x1f, 0x69,
        0x01,
        0x03, 0x00,
        0xd0, 0x69,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(catch_ref, sizeof(catch_ref), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);

    assert(load(catch_all, sizeof(catch_all), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);

    assert(load(catch_all_ref, sizeof(catch_all_ref), &module) ==
           TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_try_table_rejects_bad_catch_target(void) {
    static const uint8_t wrong_tag[] = {
        WASM_HEADER,
        0x01, 0x09,
        0x02,
        0x60, 0x01, 0x7f, 0x00,
        0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02,
        0x01, 0x01,
        0x0d, 0x03,
        0x01, 0x00, 0x00,
        0x0a, 0x0d,
        0x01, 0x0b,
        0x00,
        0x1f, 0x7f,
        0x01,
        0x00, 0x01, 0x00,
        0x41, 0x00,
        0x0b,
        0x0b
    };
    static const uint8_t wrong_depth[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x0b,
        0x01, 0x09,
        0x00,
        0x1f, 0x40,
        0x01,
        0x02, 0x01,
        0x01,
        0x0b,
        0x0b
    };
    static const uint8_t wrong_payload[] = {
        WASM_HEADER,

        /* type0: (i64) -> (); type1: () -> i32 */
        0x01, 0x09,
        0x02,
        0x60, 0x01, 0x7e, 0x00,
        0x60, 0x00, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x01,
        0x0d, 0x03,
        0x01, 0x00, 0x00,

        0x0a, 0x0d,
        0x01, 0x0b,
        0x00,
        0x1f, 0x7f,
        0x01,
        0x00, 0x00, 0x00,
        0x41, 0x00,
        0x0b,
        0x0b
    };

    expect_invalid(wrong_tag, sizeof(wrong_tag));
    expect_invalid(wrong_depth, sizeof(wrong_depth));
    expect_invalid(wrong_payload, sizeof(wrong_payload));
}

int main(void) {
    test_throw_validates_defined_tag_payload();
    test_throw_validates_imported_tag_payload();
    test_throw_rejects_wrong_payload_and_tag_index();
    test_throw_ref_uses_exnref_semantics();
    test_try_table_catch_retains_metadata();
    test_try_table_catch_ref_and_catch_all_forms();
    test_try_table_rejects_bad_catch_target();
    return 0;
}
