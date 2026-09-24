#include <turbowasm/turbowasm.h>

#include "module_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static void assert_signature(
    const turbowasm_validation_context *context,
    const turbowasm_validation_control *control,
    const uint8_t *expected_start,
    uint32_t expected_start_count,
    const uint8_t *expected_end,
    uint32_t expected_end_count) {
    const uint8_t *start_types = NULL;
    const uint8_t *end_types = NULL;
    uint32_t start_count = UINT32_MAX;
    uint32_t end_count = UINT32_MAX;
    uint32_t index;

    assert(turbowasm_validation_control_signature(
        context, control,
        &start_types, &start_count,
        &end_types, &end_count));

    assert(start_count == expected_start_count);
    assert(end_count == expected_end_count);

    for (index = 0u; index < start_count; ++index)
        assert(start_types[index] == expected_start[index]);
    for (index = 0u; index < end_count; ++index)
        assert(end_types[index] == expected_end[index]);
}

static void test_retained_control_signatures(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0 [] -> [], type1 [] -> i32, type2 i32 -> i32 */
        0x01, 0x0d,
        0x03,
        0x60, 0x00, 0x00,
        0x60, 0x00, 0x01, 0x7f,
        0x60, 0x01, 0x7f, 0x01, 0x7f,

        /* three local functions: type0, type1, type2 */
        0x03, 0x04,
        0x03, 0x00, 0x01, 0x02,

        0x0a, 0x17,
        0x03,

        /* func0: block [] -> [] */
        0x05,
        0x00,
        0x02, 0x40,
        0x0b,
        0x0b,

        /* func1: block [] -> i32 using inline block result type */
        0x07,
        0x00,
        0x02, 0x7f,
        0x41, 0x07,
        0x0b,
        0x0b,

        /* func2: local.get 0; block type2 (i32 -> i32) */
        0x07,
        0x00,
        0x20, 0x00,
        0x02, 0x02,
        0x0b,
        0x0b
    };

    turbowasm_module module = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *function;
    const turbowasm_validation_control *control;
    const turbowasm_validation_func_type *type2;
    const uint8_t i32_type[1] = {0x7f};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);

    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    assert(impl->validation.function_count == 3u);
    assert(impl->validation.type_count == 3u);

    /* Empty block signature. */
    function = turbowasm_validation_context_function(
        &impl->validation, 0u);
    assert(function != NULL);
    assert(function->control_count == 1u);
    control = &function->controls[0];
    assert(control->kind == TURBOWASM_VALIDATION_CONTROL_BLOCK);
    assert(control->type_index == UINT32_MAX);
    assert(control->inline_result_type == 0u);
    assert_signature(
        &impl->validation, control,
        NULL, 0u, NULL, 0u);

    /* Inline [] -> i32 block signature. */
    function = turbowasm_validation_context_function(
        &impl->validation, 1u);
    assert(function != NULL);
    assert(function->control_count == 1u);
    control = &function->controls[0];
    assert(control->type_index == UINT32_MAX);
    assert(control->inline_result_type == 0x7fu);
    assert_signature(
        &impl->validation, control,
        NULL, 0u, i32_type, 1u);

    /* Type-index block signature reuses the retained canonical type. */
    function = turbowasm_validation_context_function(
        &impl->validation, 2u);
    assert(function != NULL);
    assert(function->control_count == 1u);
    control = &function->controls[0];
    assert(control->type_index == 2u);
    assert(control->inline_result_type == 0u);

    type2 = turbowasm_validation_context_type(
        &impl->validation, 2u);
    assert(type2 != NULL);
    assert(type2->defined);
    assert(type2->param_count == 1u);
    assert(type2->result_count == 1u);

    {
        const uint8_t *start_types = NULL;
        const uint8_t *end_types = NULL;
        uint32_t start_count = 0u;
        uint32_t end_count = 0u;

        assert(turbowasm_validation_control_signature(
            &impl->validation, control,
            &start_types, &start_count,
            &end_types, &end_count));
        assert(start_count == 1u);
        assert(end_count == 1u);
        assert(start_types == type2->params);
        assert(end_types == type2->results);
        assert(start_types[0] == 0x7fu);
        assert(end_types[0] == 0x7fu);
    }

    turbowasm_module_destroy(&module);
}

int main(void) {
    test_retained_control_signatures();
    return 0;
}
