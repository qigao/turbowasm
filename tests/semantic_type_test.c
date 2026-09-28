#include "reader.h"
#include "validate_type.h"

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
    test_unsupported_heap_type_stays_explicit();
    return 0;
}
