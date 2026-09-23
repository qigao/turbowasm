#include <turbowasm/turbowasm.h>

#include "../src/module_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static void test_retained_data_and_element_descriptors(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type 0 = [] -> [] */
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,

        /* import function m.f : type 0 */
        0x02, 0x07,
        0x01,
        0x01, 0x6d,
        0x01, 0x66,
        0x00, 0x00,

        /* table 0: funcref min=2 max=2 */
        0x04, 0x05,
        0x01,
        0x70, 0x01, 0x02, 0x02,

        /* memory 0: min=1 page */
        0x05, 0x03,
        0x01, 0x00, 0x01,

        /* two element segments:
         * 0: active table0 @ 1, legacy funcidx[0]
         * 1: passive funcref, [ref.null funcref] */
        0x09, 0x0d,
        0x02,
        0x00, 0x41, 0x01, 0x0b, 0x01, 0x00,
        0x05, 0x70, 0x01, 0xd0, 0x70, 0x0b,

        /* two data segments:
         * 0: active memory0 @ 2, "abc"
         * 1: passive, "xy" */
        0x0b, 0x0d,
        0x02,
        0x00, 0x41, 0x02, 0x0b, 0x03, 0x61, 0x62, 0x63,
        0x01, 0x02, 0x78, 0x79
    };
    static const uint8_t active_elem_offset[] = {
        0x41, 0x01, 0x0b
    };
    static const uint8_t passive_elem_expr[] = {
        0xd0, 0x70, 0x0b
    };
    static const uint8_t active_data_offset[] = {
        0x41, 0x02, 0x0b
    };
    turbowasm_module module = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_element_segment *element0;
    const turbowasm_validation_element_segment *element1;
    const turbowasm_validation_data_segment *data0;
    const turbowasm_validation_data_segment *data1;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);

    assert(impl->validation.element_segment_count == 2u);

    element0 = &impl->validation.element_segments[0];
    assert(element0->mode == TURBOWASM_VALIDATION_SEGMENT_ACTIVE);
    assert(element0->table_index == 0u);
    assert(element0->reference_type == 0x70u);
    assert(element0->offset.result_type == 0x7fu);
    assert(element0->offset.size == sizeof(active_elem_offset));
    assert(memcmp(element0->offset.bytes,
                  active_elem_offset,
                  sizeof(active_elem_offset)) == 0);
    assert(element0->item_count == 1u);
    assert(element0->items[0].kind ==
           TURBOWASM_VALIDATION_ELEMENT_FUNCTION_INDEX);
    assert(element0->items[0].function_index == 0u);

    element1 = &impl->validation.element_segments[1];
    assert(element1->mode == TURBOWASM_VALIDATION_SEGMENT_PASSIVE);
    assert(element1->reference_type == 0x70u);
    assert(element1->offset.bytes == NULL);
    assert(element1->item_count == 1u);
    assert(element1->items[0].kind ==
           TURBOWASM_VALIDATION_ELEMENT_CONST_EXPR);
    assert(element1->items[0].expression.result_type == 0x70u);
    assert(element1->items[0].expression.size ==
           sizeof(passive_elem_expr));
    assert(memcmp(element1->items[0].expression.bytes,
                  passive_elem_expr,
                  sizeof(passive_elem_expr)) == 0);

    assert(impl->validation.data_segment_count == 2u);

    data0 = &impl->validation.data_segments[0];
    assert(data0->mode == TURBOWASM_VALIDATION_SEGMENT_ACTIVE);
    assert(data0->memory_index == 0u);
    assert(data0->offset.result_type == 0x7fu);
    assert(data0->offset.size == sizeof(active_data_offset));
    assert(memcmp(data0->offset.bytes,
                  active_data_offset,
                  sizeof(active_data_offset)) == 0);
    assert(data0->data_size == 3u);
    assert(memcmp(data0->data, "abc", 3u) == 0);

    data1 = &impl->validation.data_segments[1];
    assert(data1->mode == TURBOWASM_VALIDATION_SEGMENT_PASSIVE);
    assert(data1->offset.bytes == NULL);
    assert(data1->data_size == 2u);
    assert(memcmp(data1->data, "xy", 2u) == 0);

    assert(element0->offset.bytes >= bytes);
    assert(element1->items[0].expression.bytes >= bytes);
    assert(data0->data >= bytes);
    assert(data1->data >= bytes);
    assert(data1->data + data1->data_size <= bytes + sizeof(bytes));

    turbowasm_module_destroy(&module);
}

int main(void) {
    test_retained_data_and_element_descriptors();
    return 0;
}
