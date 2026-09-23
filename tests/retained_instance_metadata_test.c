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

static void test_retained_limits_and_global_initializer(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* table: funcref, min=2, max=4 */
        0x04, 0x05,
        0x01,
        0x70, 0x01, 0x02, 0x04,

        /* memory: min=1 page, max=3 pages */
        0x05, 0x04,
        0x01,
        0x01, 0x01, 0x03,

        /* immutable i32 global initialized with i32.const 42 */
        0x06, 0x06,
        0x01,
        0x7f, 0x00,
        0x41, 0x2a, 0x0b
    };
    static const uint8_t expected_initializer[] = {
        0x41, 0x2a, 0x0b
    };
    turbowasm_module module = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_table *table;
    const turbowasm_validation_memory *memory;
    const turbowasm_validation_global *global;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);

    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);

    assert(impl->validation.table_count == 1u);
    table = &impl->validation.tables[0];
    assert(!table->imported);
    assert(table->reference_type == 0x70u);
    assert(table->limits.minimum == 2u);
    assert(table->limits.has_maximum);
    assert(table->limits.maximum == 4u);

    assert(impl->validation.memory_count == 1u);
    memory = &impl->validation.memories[0];
    assert(!memory->imported);
    assert(memory->limits.minimum == 1u);
    assert(memory->limits.has_maximum);
    assert(memory->limits.maximum == 3u);

    assert(impl->validation.global_count == 1u);
    global = &impl->validation.globals[0];
    assert(!global->imported);
    assert(global->value_type == 0x7fu);
    assert(!global->mutable_value);
    assert(global->initializer != NULL);
    assert(global->initializer_size == sizeof(expected_initializer));
    assert(memcmp(global->initializer,
                  expected_initializer,
                  sizeof(expected_initializer)) == 0);

    assert(global->initializer >= bytes);
    assert(global->initializer + global->initializer_size <=
           bytes + sizeof(bytes));

    turbowasm_module_destroy(&module);
}

static void test_limits_without_maximum(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* table: funcref, min=3, no maximum */
        0x04, 0x04,
        0x01,
        0x70, 0x00, 0x03,

        /* memory: min=2 pages, no maximum */
        0x05, 0x03,
        0x01,
        0x00, 0x02
    };
    turbowasm_module module = {0};
    const turbowasm_module_impl *impl;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);

    assert(impl->validation.tables[0].limits.minimum == 3u);
    assert(!impl->validation.tables[0].limits.has_maximum);

    assert(impl->validation.memories[0].limits.minimum == 2u);
    assert(!impl->validation.memories[0].limits.has_maximum);

    turbowasm_module_destroy(&module);
}

int main(void) {
    test_retained_limits_and_global_initializer();
    test_limits_without_maximum();
    return 0;
}
