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

static void test_valid_atomic_load(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        /* () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        /* shared memory min=1 max=1 */
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        /* i32.const 0; i32.atomic.load align=2 offset=0 */
        0x0a, 0x0a, 0x01, 0x08, 0x00,
        0x41, 0x00,
        0xfe, 0x10, 0x02, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);
}

static void test_atomic_alignment_must_be_exact(void) {
    static const uint8_t too_small[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        /* i32.atomic.load requires align=2, not align=1 */
        0x0a, 0x0a, 0x01, 0x08, 0x00,
        0x41, 0x00,
        0xfe, 0x10, 0x01, 0x00,
        0x0b
    };
    static const uint8_t too_large[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        /* i32.atomic.load requires align=2, not align=3 */
        0x0a, 0x0a, 0x01, 0x08, 0x00,
        0x41, 0x00,
        0xfe, 0x10, 0x03, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, too_small, sizeof(too_small)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);

    assert(turbowasm_module_load_borrowed(
               &module, too_large, sizeof(too_large)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_atomic_stack_signatures(void) {
    static const uint8_t store_missing_value[] = {
        WASM_HEADER,
        /* () -> () */
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        /* address only; i32.atomic.store needs address + value */
        0x0a, 0x0a, 0x01, 0x08, 0x00,
        0x41, 0x00,
        0xfe, 0x17, 0x02, 0x00,
        0x0b
    };
    static const uint8_t cmpxchg_missing_replacement[] = {
        WASM_HEADER,
        /* () -> i32 */
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        /* address + expected only; replacement missing */
        0x0a, 0x0c, 0x01, 0x0a, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0xfe, 0x48, 0x02, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module,
               store_missing_value,
               sizeof(store_missing_value)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);

    assert(turbowasm_module_load_borrowed(
               &module,
               cmpxchg_missing_replacement,
               sizeof(cmpxchg_missing_replacement)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_wait_alignment_validation(void) {
    static const uint8_t wait32_valid[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        0x0a, 0x0e, 0x01, 0x0c, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0x42, 0x00,
        0xfe, 0x01, 0x02, 0x00,
        0x0b
    };
    static const uint8_t wait32_bad_alignment[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        0x0a, 0x0e, 0x01, 0x0c, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0x42, 0x00,
        0xfe, 0x01, 0x01, 0x00,
        0x0b
    };
    static const uint8_t wait64_valid[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        0x0a, 0x0e, 0x01, 0x0c, 0x00,
        0x41, 0x00,
        0x42, 0x00,
        0x42, 0x00,
        0xfe, 0x02, 0x03, 0x00,
        0x0b
    };
    static const uint8_t wait64_bad_alignment[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        0x0a, 0x0e, 0x01, 0x0c, 0x00,
        0x41, 0x00,
        0x42, 0x00,
        0x42, 0x00,
        0xfe, 0x02, 0x02, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, wait32_valid,
               sizeof(wait32_valid)) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);

    module = (turbowasm_module){0};
    assert(turbowasm_module_load_borrowed(
               &module, wait32_bad_alignment,
               sizeof(wait32_bad_alignment)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);

    assert(turbowasm_module_load_borrowed(
               &module, wait64_valid,
               sizeof(wait64_valid)) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);

    module = (turbowasm_module){0};
    assert(turbowasm_module_load_borrowed(
               &module, wait64_bad_alignment,
               sizeof(wait64_bad_alignment)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_notify_alignment_validation(void) {
    static const uint8_t valid[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        0x0a, 0x0c, 0x01, 0x0a, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0xfe, 0x00, 0x02, 0x00,
        0x0b
    };
    static const uint8_t bad_alignment[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
        0x0a, 0x0c, 0x01, 0x0a, 0x00,
        0x41, 0x00,
        0x41, 0x00,
        0xfe, 0x00, 0x01, 0x00,
        0x0b
    };
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, valid, sizeof(valid)) == TURBOWASM_OK);
    turbowasm_module_destroy(&module);

    module = (turbowasm_module){0};
    assert(turbowasm_module_load_borrowed(
               &module,
               bad_alignment,
               sizeof(bad_alignment)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

static void test_atomic_fence_validation(void) {
    static const uint8_t fence[] = {
        WASM_HEADER,
        /* () -> (), no memory required */
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        /* atomic.fence subopcode 3 + reserved zero immediate */
        0x0a, 0x07, 0x01, 0x05, 0x00,
        0xfe, 0x03, 0x00,
        0x0b
    };
    static const uint8_t invalid_reserved[] = {
        WASM_HEADER,
        0x01, 0x04,
        0x01, 0x60, 0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x07, 0x01, 0x05, 0x00,
        0xfe, 0x03, 0x01,
        0x0b
    };
    turbowasm_module module = {0};

    assert(turbowasm_module_load_borrowed(
               &module, fence, sizeof(fence)) ==
           TURBOWASM_OK);
    turbowasm_module_destroy(&module);

    module = (turbowasm_module){0};
    assert(turbowasm_module_load_borrowed(
               &module,
               invalid_reserved,
               sizeof(invalid_reserved)) ==
           TURBOWASM_MALFORMED_MODULE);
    assert(module.impl == NULL);
}

int main(void) {
    test_valid_atomic_load();
    test_atomic_alignment_must_be_exact();
    test_atomic_stack_signatures();
    test_wait_alignment_validation();
    test_notify_alignment_validation();
    test_atomic_fence_validation();
    return 0;
}
