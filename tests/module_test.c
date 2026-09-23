#include <turbowasm/turbowasm.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t valid_module[] = {
    0x00, 0x61, 0x73, 0x6d,
    0x01, 0x00, 0x00, 0x00
};

static void test_valid_header(void) {
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, valid_module, sizeof(valid_module)) == TURBOWASM_OK);
    assert(turbowasm_module_bytes(&module) == valid_module);
    assert(turbowasm_module_size(&module) == sizeof(valid_module));

    turbowasm_module_destroy(&module);
    assert(module.impl == NULL);
    assert(turbowasm_module_bytes(&module) == NULL);
    assert(turbowasm_module_size(&module) == 0u);

    turbowasm_module_destroy(&module);
}

static void test_truncated(void) {
    turbowasm_module module = {0};
    size_t size;

    for (size = 0u; size < sizeof(valid_module); ++size) {
        assert(turbowasm_module_load_borrowed(
                   &module, valid_module, size) == TURBOWASM_MALFORMED_MODULE);
        assert(module.impl == NULL);
    }
}

static void test_bad_magic(void) {
    uint8_t bytes[sizeof(valid_module)];
    turbowasm_module module = {0};
    size_t index;

    for (index = 0u; index < sizeof(bytes); ++index)
        bytes[index] = valid_module[index];
    bytes[0] = 0xffu;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_bad_version(void) {
    uint8_t bytes[sizeof(valid_module)];
    turbowasm_module module = {0};
    size_t index;

    for (index = 0u; index < sizeof(bytes); ++index)
        bytes[index] = valid_module[index];
    bytes[4] = 0x02u;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_live_handle_rejected(void) {
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, valid_module, sizeof(valid_module)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module, valid_module, sizeof(valid_module)) ==
           TURBOWASM_INVALID_ARGUMENT);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_valid_header();
    test_truncated();
    test_bad_magic();
    test_bad_version();
    test_live_handle_rejected();
    return 0;
}
