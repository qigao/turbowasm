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

#define TYPE_SECTION_EMPTY_FN \
    0x01, 0x04, \
    0x01, 0x60, 0x00, 0x00

#define IMPORT_SECTION_FUNCTION \
    0x02, 0x07, \
    0x01, \
    0x01, 0x6d, \
    0x01, 0x66, \
    0x00, 0x00

#define MEMORY_SECTION_ONE_PAGE \
    0x05, 0x03, \
    0x01, 0x00, 0x01

static turbowasm_status load(const uint8_t *bytes, size_t size,
                             turbowasm_module *module) {
    return turbowasm_module_load_borrowed(module, bytes, size);
}

static void test_i32_global_and_export(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x06,
        0x01, 0x7f, 0x00, 0x41, 0x2a, 0x0b,
        0x07, 0x05,
        0x01, 0x01, 0x67, 0x03, 0x00
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.global_count == 1u);
    assert(summary.export_count == 1u);
    turbowasm_module_destroy(&module);
}

static void test_negative_i64_global(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x06,
        0x01, 0x7e, 0x00, 0x42, 0x7f, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_v128_global(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x16,
        0x01, 0x7b, 0x00,
        0xfd, 0x0c,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_ref_func_global(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        TYPE_SECTION_EMPTY_FN,
        IMPORT_SECTION_FUNCTION,
        0x06, 0x06,
        0x01, 0x70, 0x00, 0xd2, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_global_type_mismatch(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x06,
        0x01, 0x7e, 0x00, 0x41, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_global_get_imported_immutable(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x02, 0x08,
        0x01,
        0x01, 0x6d,
        0x01, 0x67,
        0x03, 0x7f, 0x00,
        0x06, 0x06,
        0x01, 0x7f, 0x00, 0x23, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.imported_global_count == 1u);
    assert(summary.global_count == 1u);
    turbowasm_module_destroy(&module);
}

static void test_global_get_rejects_imported_mutable(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x02, 0x08,
        0x01,
        0x01, 0x6d,
        0x01, 0x67,
        0x03, 0x7f, 0x01,
        0x06, 0x06,
        0x01, 0x7f, 0x00, 0x23, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_global_get_accepts_prior_local_immutable(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x0b,
        0x02,
        0x7f, 0x00, 0x41, 0x2a, 0x0b,
        0x7f, 0x00, 0x23, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_global_get_rejects_prior_local_mutable(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x0b,
        0x02,
        0x7f, 0x01, 0x41, 0x00, 0x0b,
        0x7f, 0x00, 0x23, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_global_get_rejects_forward_local_global(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x0b,
        0x02,
        0x7f, 0x00, 0x23, 0x01, 0x0b,
        0x7f, 0x00, 0x41, 0x00, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_known_nonconst_opcode_is_invalid(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /*
         * global f32 = f32.neg(f32.const 0)
         *
         * f32.neg is a known core opcode, but it is not admitted by the
         * constant-expression grammar.
         */
        0x06, 0x0a,
        0x01,
        0x7d, 0x00,
        0x43, 0x00, 0x00, 0x00, 0x00,
        0x8c,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_call_is_not_a_constant_expression(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> funcref */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x70,

        /* func0 uses type0 */
        0x03, 0x02,
        0x01, 0x00,

        /* global funcref = call 0: syntactically known, not constant */
        0x06, 0x06,
        0x01, 0x70, 0x00,
        0x10, 0x00, 0x0b,

        /* func0: ref.null func */
        0x0a, 0x06,
        0x01, 0x04,
        0x00, 0xd0, 0x70, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_extended_const_i32_arithmetic(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x0f,
        0x01, 0x7f, 0x00,
        0x41, 0x14,
        0x41, 0x02,
        0x6c,
        0x41, 0x02,
        0x6b,
        0x41, 0x04,
        0x6a,
        0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_extended_const_multiple_values_rejected(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x08,
        0x01, 0x7f, 0x00,
        0x41, 0x00, 0x41, 0x01, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_sleb32_overflow_rejected(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x06, 0x0a,
        0x01, 0x7f, 0x00,
        0x41, 0x80, 0x80, 0x80, 0x80, 0x10, 0x0b
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_active_data_with_count(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        MEMORY_SECTION_ONE_PAGE,
        0x0c, 0x01, 0x01,
        0x0b, 0x09,
        0x01,
        0x00,
        0x41, 0x00, 0x0b,
        0x03, 0x61, 0x62, 0x63
    };
    turbowasm_module module = {0};
    turbowasm_module_summary summary = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    assert(turbowasm_module_summary_get(&module, &summary));
    assert(summary.has_data_count);
    assert(summary.data_count == 1u);
    assert(summary.data_segment_count == 1u);
    turbowasm_module_destroy(&module);
}

static void test_passive_data_without_memory(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x0b, 0x04,
        0x01, 0x01, 0x01, 0xff
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_active_data_requires_memory(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x0b, 0x06,
        0x01, 0x00,
        0x41, 0x00, 0x0b,
        0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_data_offset_must_be_i32(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        MEMORY_SECTION_ONE_PAGE,
        0x0b, 0x06,
        0x01, 0x00,
        0x42, 0x00, 0x0b,
        0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_data_count_must_match_actual_segments(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        MEMORY_SECTION_ONE_PAGE,
        0x0c, 0x01, 0x02,
        0x0b, 0x06,
        0x01, 0x00,
        0x41, 0x00, 0x0b,
        0x00
    };
    turbowasm_module module = {0};

    assert(load(bytes, sizeof(bytes), &module) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

int main(void) {
    test_i32_global_and_export();
    test_negative_i64_global();
    test_v128_global();
    test_ref_func_global();
    test_global_type_mismatch();
    test_global_get_imported_immutable();
    test_global_get_rejects_imported_mutable();
    test_global_get_accepts_prior_local_immutable();
    test_global_get_rejects_prior_local_mutable();
    test_global_get_rejects_forward_local_global();
    test_known_nonconst_opcode_is_invalid();
    test_call_is_not_a_constant_expression();
    test_extended_const_i32_arithmetic();
    test_extended_const_multiple_values_rejected();
    test_sleb32_overflow_rejected();
    test_active_data_with_count();
    test_passive_data_without_memory();
    test_active_data_requires_memory();
    test_data_offset_must_be_i32();
    test_data_count_must_match_actual_segments();
    return 0;
}
