#include "module_internal.h"
#include "reader.h"
#include "validate_type.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

static turbowasm_status parse(const uint8_t *bytes,
                              size_t size,
                              turbowasm_validation_value_type *type,
                              bool *generalized) {
    turbowasm_reader reader;
    turbowasm_status status;

    turbowasm_reader_init(&reader, bytes, size);
    status = turbowasm_validation_read_valtype(
        &reader, type, generalized);
    if (status == TURBOWASM_OK)
        assert(turbowasm_reader_remaining(&reader) == 0u);
    return status;
}

static void test_legacy_reference_types(void) {
    static const uint8_t funcref[] = {0x70};
    static const uint8_t externref[] = {0x6f};
    turbowasm_validation_value_type type;
    bool generalized = true;

    assert(parse(funcref, sizeof(funcref), &type, &generalized) ==
           TURBOWASM_OK);
    assert(!generalized);
    assert(type.carrier == 0x70u);
    assert(type.is_reference);
    assert(type.nullable);
    assert(type.heap_kind == TURBOWASM_VALIDATION_HEAP_FUNC);

    generalized = true;
    assert(parse(externref, sizeof(externref), &type, &generalized) ==
           TURBOWASM_OK);
    assert(!generalized);
    assert(type.carrier == 0x6fu);
    assert(type.is_reference);
    assert(type.nullable);
    assert(type.heap_kind == TURBOWASM_VALIDATION_HEAP_EXTERN);
}

static void test_general_function_reference_types(void) {
    static const uint8_t null_func[] = {0x63, 0x70};
    static const uint8_t nonnull_type0[] = {0x64, 0x00};
    static const uint8_t nullable_type1[] = {0x63, 0x01};
    turbowasm_validation_value_type type;
    bool generalized = false;

    assert(parse(null_func, sizeof(null_func), &type, &generalized) ==
           TURBOWASM_OK);
    assert(generalized);
    assert(type.carrier == 0x70u);
    assert(type.is_reference);
    assert(type.nullable);
    assert(type.heap_kind == TURBOWASM_VALIDATION_HEAP_FUNC);

    generalized = false;
    assert(parse(nonnull_type0, sizeof(nonnull_type0), &type, &generalized) ==
           TURBOWASM_OK);
    assert(generalized);
    assert(type.carrier == 0x70u);
    assert(type.is_reference);
    assert(!type.nullable);
    assert(type.heap_kind == TURBOWASM_VALIDATION_HEAP_TYPE_INDEX);
    assert(type.type_index == 0u);

    generalized = false;
    assert(parse(nullable_type1, sizeof(nullable_type1), &type, &generalized) ==
           TURBOWASM_OK);
    assert(generalized);
    assert(type.nullable);
    assert(type.heap_kind == TURBOWASM_VALIDATION_HEAP_TYPE_INDEX);
    assert(type.type_index == 1u);
}

static void test_legacy_function_type_retains_semantics(void) {
    static const uint8_t bytes[] = {
        0x00, 0x61, 0x73, 0x6d,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x06,
        0x01, 0x60,
        0x01, 0x6f,
        0x01, 0x70
    };
    turbowasm_module module = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_func_type *type;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    assert(impl->validation.type_count == 1u);

    type = &impl->validation.types[0];
    assert(type->defined);
    assert(type->param_count == 1u);
    assert(type->result_count == 1u);
    assert(type->params[0] == 0x6fu);
    assert(type->results[0] == 0x70u);

    assert(type->param_semantics[0].is_reference);
    assert(type->param_semantics[0].nullable);
    assert(type->param_semantics[0].heap_kind ==
           TURBOWASM_VALIDATION_HEAP_EXTERN);
    assert(type->result_semantics[0].is_reference);
    assert(type->result_semantics[0].nullable);
    assert(type->result_semantics[0].heap_kind ==
           TURBOWASM_VALIDATION_HEAP_FUNC);

    turbowasm_module_destroy(&module);
}

static void test_general_reference_global_retains_semantics(void) {
    static const uint8_t bytes[] = {
        0x00, 0x61, 0x73, 0x6d,
        0x01, 0x00, 0x00, 0x00,

        /* global0: immutable (ref null func) = ref.null func */
        0x06, 0x07,
        0x01,
        0x63, 0x70, 0x00,
        0xd0, 0x70, 0x0b
    };
    turbowasm_module module = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_global *global;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    assert(impl->validation.global_count == 1u);

    global = &impl->validation.globals[0];
    assert(global->value_type == 0x70u);
    assert(global->semantic_type.is_reference);
    assert(global->semantic_type.nullable);
    assert(global->semantic_type.heap_kind ==
           TURBOWASM_VALIDATION_HEAP_FUNC);

    turbowasm_module_destroy(&module);
}

static void test_unsupported_heap_type_stays_explicit(void) {
    static const uint8_t anyref[] = {0x63, 0x6e};
    turbowasm_validation_value_type type;
    bool generalized = false;

    assert(parse(anyref, sizeof(anyref), &type, &generalized) ==
           TURBOWASM_UNSUPPORTED);
}

int main(void) {
    test_legacy_reference_types();
    test_general_function_reference_types();
    test_legacy_function_type_retains_semantics();
    test_general_reference_global_retains_semantics();
    test_unsupported_heap_type_stays_explicit();
    return 0;
}
