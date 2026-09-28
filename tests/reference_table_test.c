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
    uint32_t function_index) {
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

static void test_ref_null_and_is_null(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x07,
        0x01, 0x05,
        0x00, 0xd0, 0x70, 0xd1, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_i32(&instance, 0u, NULL, 0u) == 1);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_funcref_public_argument(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x70, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x07,
        0x01, 0x05,
        0x00, 0x20, 0x00, 0xd1, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0};

    argument.kind = TURBOWASM_VALUE_FUNCREF;
    argument.as.funcref.is_null = true;
    argument.as.funcref.function_index = UINT32_MAX;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_i32(&instance, 0u, &argument, 1u) == 1);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_table_get_out_of_bounds_traps(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x04, 0x04, 0x01, 0x70, 0x00, 0x01,
        0x0a, 0x09,
        0x01, 0x07,
        0x00, 0x41, 0x01, 0x25, 0x00, 0xd1, 0x0b
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
    assert(trap == TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_ref_func_table_set_and_call_indirect(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0 [] -> i32, type1 [] -> [] */
        0x01, 0x08,
        0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x00, 0x00,

        /* functions: target, setter, indirect caller */
        0x03, 0x04,
        0x03, 0x00, 0x01, 0x00,

        /* table0 funcref min=1 */
        0x04, 0x04,
        0x01, 0x70, 0x00, 0x01,

        /* declarative element declares ref.func 0 without initializing */
        0x09, 0x05,
        0x01, 0x03, 0x00, 0x01, 0x00,

        0x0a, 0x17,
        0x03,
        /* func0 -> 7 */
        0x04, 0x00, 0x41, 0x07, 0x0b,
        /* func1: table[0] = ref.func 0 */
        0x08, 0x00,
              0x41, 0x00,
              0xd2, 0x00,
              0x26, 0x00,
              0x0b,
        /* func2: call_indirect type0 at table[0] */
        0x07, 0x00,
              0x41, 0x00,
              0x11, 0x00, 0x00,
              0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    invoke_void(&instance, 1u);
    assert(invoke_i32(&instance, 2u, NULL, 0u) == 7);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_ref_func_table_set_and_return_call_indirect(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0 [] -> i32, type1 [] -> [] */
        0x01, 0x08,
        0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x00, 0x00,

        /* functions: target, setter, indirect tail caller */
        0x03, 0x04,
        0x03, 0x00, 0x01, 0x00,

        /* table0 funcref min=1 */
        0x04, 0x04,
        0x01, 0x70, 0x00, 0x01,

        /* declarative element declares ref.func 0 without initializing */
        0x09, 0x05,
        0x01, 0x03, 0x00, 0x01, 0x00,

        0x0a, 0x17,
        0x03,
        /* func0 -> 7 */
        0x04, 0x00, 0x41, 0x07, 0x0b,
        /* func1: table[0] = ref.func 0 */
        0x08, 0x00,
              0x41, 0x00,
              0xd2, 0x00,
              0x26, 0x00,
              0x0b,
        /* func2: return_call_indirect type0 at table[0] */
        0x07, 0x00,
              0x41, 0x00,
              0x13, 0x00, 0x00,
              0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    invoke_void(&instance, 1u);
    assert(invoke_i32(&instance, 2u, NULL, 0u) == 7);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_return_call_indirect_preserves_null_and_bounds_traps(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> i32, type1: (i32) -> i32 */
        0x01, 0x0a,
        0x02,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x01, 0x7f, 0x01, 0x7f,

        /* caller uses type1 */
        0x03, 0x02,
        0x01, 0x01,

        /* table0 funcref min=1, initially null */
        0x04, 0x04,
        0x01, 0x70, 0x00, 0x01,

        0x0a, 0x09,
        0x01,
        0x07, 0x00,
              0x20, 0x00,
              0x13, 0x00, 0x00,
              0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    argument.kind = TURBOWASM_VALUE_I32;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    argument.as.i32 = 0;
    assert(turbowasm_instance_invoke(
               &instance, 0u,
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_TRAPPED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_INDIRECT_CALL_NULL);

    argument.as.i32 = 1;
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(
               &instance, 0u,
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_TRAPPED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_return_call_indirect_preserves_type_mismatch_trap(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> i32, type1: () -> i64, type2: () -> () */
        0x01, 0x0c,
        0x03,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x00, 0x01, 0x7e,
        0x60, 0x00, 0x00,

        /* functions: i64 target, setter, i32 tail caller */
        0x03, 0x04,
        0x03, 0x01, 0x02, 0x00,

        /* table0 funcref min=1 */
        0x04, 0x04,
        0x01, 0x70, 0x00, 0x01,

        /* declarative element declares ref.func 0 */
        0x09, 0x05,
        0x01, 0x03, 0x00, 0x01, 0x00,

        0x0a, 0x17,
        0x03,
        /* func0: i64.const 7 */
        0x04, 0x00, 0x42, 0x07, 0x0b,
        /* func1: table[0] = ref.func 0 */
        0x08, 0x00,
              0x41, 0x00,
              0xd2, 0x00,
              0x26, 0x00,
              0x0b,
        /* func2: return_call_indirect expecting type0 */
        0x07, 0x00,
              0x41, 0x00,
              0x13, 0x00, 0x00,
              0x0b
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

    invoke_void(&instance, 1u);
    assert(turbowasm_instance_invoke(
               &instance, 2u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_TRAPPED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_typed_funcref_br_table_meet(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> (), type1: (i32) -> funcref */
        0x01, 0x09,
        0x02,
        0x60, 0x00, 0x00,
        0x60, 0x01, 0x7f, 0x01, 0x70,

        0x03, 0x02,
        0x01, 0x01,

        0x0a, 0x14,
        0x01, 0x12,
        0x00,
        /* outer label: (ref null func) */
        0x02, 0x70,
          /* inner label: (ref null type0) */
          0x02, 0x63, 0x00,
            /* value: ref.null type0; selector: local0 */
            0xd0, 0x00,
            0x20, 0x00,
            /* labels [outer, outer], default inner */
            0x0e, 0x02, 0x01, 0x01, 0x00,
          0x0b,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    int32_t selector;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    for (selector = 0; selector < 3; ++selector) {
        argument.kind = TURBOWASM_VALUE_I32;
        argument.as.i32 = selector;
        result_count = 0u;
        trap = TURBOWASM_TRAP_NONE;

        assert(turbowasm_instance_invoke(
                   &instance, 0u,
                   &argument, 1u,
                   &result, 1u,
                   &result_count,
                   &trap) == TURBOWASM_OK);
        assert(trap == TURBOWASM_TRAP_NONE);
        assert(result_count == 1u);
        assert(result.kind == TURBOWASM_VALUE_FUNCREF);
        assert(result.as.funcref.is_null);
    }

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_externref_table_round_trip(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: (externref) -> externref */
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x6f, 0x01, 0x6f,

        /* one function, type0 */
        0x03, 0x02,
        0x01, 0x00,

        /* table0 externref min=1 max=2 */
        0x04, 0x05,
        0x01, 0x6f, 0x01, 0x01, 0x02,

        /* table[0] = arg0; return table[0] */
        0x0a, 0x0e,
        0x01, 0x0c,
        0x00,
        0x41, 0x00,
        0x20, 0x00,
        0x26, 0x00,
        0x41, 0x00,
        0x25, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value argument = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    argument.kind = TURBOWASM_VALUE_EXTERNREF;
    argument.as.externref.is_null = false;
    argument.as.externref.token = (uintptr_t)42u;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_EXTERNREF);
    assert(!result.as.externref.is_null);
    assert(result.as.externref.token == (uintptr_t)42u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_defined_table_initializer_executes(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: () -> (); type1: () -> funcref */
        0x01, 0x08,
        0x02,
        0x60, 0x00, 0x00,
        0x60, 0x00, 0x01, 0x70,

        /* target uses type0; getter uses type1 */
        0x03, 0x03,
        0x02, 0x00, 0x01,

        /*
         * table0: min=1, element type (ref func), explicit initializer
         * ref.func 0.
         */
        0x04, 0x0a,
        0x01,
        0x40, 0x00,
        0x64, 0x70,
        0x00, 0x01,
        0xd2, 0x00, 0x0b,

        0x0a, 0x0b,
        0x02,
        /* func0 target */
        0x02, 0x00, 0x0b,
        /* func1 getter: table.get 0 at index 0 */
        0x06, 0x00, 0x41, 0x00, 0x25, 0x00, 0x0b
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
               &instance, 1u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_FUNCREF);
    assert(!result.as.funcref.is_null);
    assert(result.as.funcref.function_index == 0u);
    assert(result.as.funcref.owner != NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_nonnullable_table_rejects_null_initializer(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* table0: min=1, (ref func), explicit ref.null func */
        0x04, 0x0a,
        0x01,
        0x40, 0x00,
        0x64, 0x70,
        0x00, 0x01,
        0xd0, 0x70, 0x0b
    };
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_externref_null_is_null_executes(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x07,
        0x01, 0x05,
        0x00, 0xd0, 0x6f, 0xd1, 0x0b
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
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 1);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_ref_null_and_is_null();
    test_funcref_public_argument();
    test_table_get_out_of_bounds_traps();
    test_ref_func_table_set_and_call_indirect();
    test_ref_func_table_set_and_return_call_indirect();
    test_return_call_indirect_preserves_null_and_bounds_traps();
    test_return_call_indirect_preserves_type_mismatch_trap();
    test_typed_funcref_br_table_meet();
    test_externref_table_round_trip();
    test_defined_table_initializer_executes();
    test_nonnullable_table_rejects_null_initializer();
    test_externref_null_is_null_executes();
    return 0;
}
