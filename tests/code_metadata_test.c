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

static void test_imported_and_local_metadata(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        0x01, 0x09,
        0x02,
        0x60, 0x00, 0x00,
        0x60, 0x01, 0x7f, 0x01, 0x7f,

        0x02, 0x07,
        0x01,
        0x01, 0x6d,
        0x01, 0x66,
        0x00, 0x00,

        0x03, 0x02, 0x01, 0x01,

        0x0a, 0x08, 0x01, 0x06,
        0x01, 0x01, 0x7e,
        0x20, 0x00, 0x0b
    };
    turbowasm_module module = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *imported;
    const turbowasm_validation_function *local;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);

    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    assert(impl->validation.function_count == 2u);

    imported = turbowasm_validation_context_function(
        &impl->validation, 0u);
    local = turbowasm_validation_context_function(
        &impl->validation, 1u);

    assert(imported != NULL);
    assert(imported->imported);
    assert(imported->local_types == NULL);
    assert(imported->local_count == 0u);
    assert(imported->code == NULL);
    assert(imported->code_size == 0u);

    assert(local != NULL);
    assert(!local->imported);
    assert(local->local_count == 2u);
    assert(local->local_types != NULL);
    assert(local->local_types[0] == 0x7fu);
    assert(local->local_types[1] == 0x7eu);

    assert(local->code != NULL);
    assert(local->code_size == 3u);
    assert(local->code[0] == 0x20u);
    assert(local->code[1] == 0x00u);
    assert(local->code[2] == 0x0bu);

    assert(local->code >= bytes);
    assert(local->code + local->code_size <= bytes + sizeof(bytes));

    turbowasm_module_destroy(&module);
}

int main(void) {
    test_imported_and_local_metadata();
    return 0;
}
