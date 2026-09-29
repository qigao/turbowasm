#include <turbowasm/turbowasm.h>

#include "atomic.h"
#include "instance_internal.h"

#include <salts/thread.h>

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

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
    return out;
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void test_unshared_atomic_load_store(void) {
    static const uint8_t full_width[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        /* unshared min=1 */
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0a, 0x12, 0x01, 0x10, 0x00,
        0x41, 0x00,
        0x41, 0x2a,
        0xfe, 0x17, 0x02, 0x00,
        0x41, 0x00,
        0xfe, 0x10, 0x02, 0x00,
        0x0b
    };
    static const uint8_t narrow[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0a, 0x12, 0x01, 0x10, 0x00,
        0x41, 0x00,
        0x41, 0x7f, /* -1 */
        0xfe, 0x19, 0x00, 0x00,
        0x41, 0x00,
        0xfe, 0x12, 0x00, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};

    assert(turbowasm_module_load_borrowed(
               &module, full_width,
               sizeof(full_width)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_i32(&instance, 0u) == 42);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    module = (turbowasm_module){0};
    instance = (turbowasm_instance){0};
    assert(turbowasm_module_load_borrowed(
               &module, narrow,
               sizeof(narrow)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_i32(&instance, 0u) == 255);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_cmpxchg_and_runtime_alignment_trap(void) {
    static const uint8_t cmpxchg[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x03, 0x02, 0x00, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        0x0a, 0x1f, 0x02,
        /* f0: store 7; cmpxchg 7->9; return old */
        0x14, 0x00,
        0x41, 0x00,
        0x41, 0x07,
        0xfe, 0x17, 0x02, 0x00,
        0x41, 0x00,
        0x41, 0x07,
        0x41, 0x09,
        0xfe, 0x48, 0x02, 0x00,
        0x0b,
        /* f1: atomic load */
        0x08, 0x00,
        0x41, 0x00,
        0xfe, 0x10, 0x02, 0x00,
        0x0b
    };
    static const uint8_t misaligned[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x05, 0x03, 0x01, 0x00, 0x01,
        /* valid natural alignment immediate, effective address = 1 */
        0x0a, 0x0a, 0x01, 0x08, 0x00,
        0x41, 0x01,
        0xfe, 0x10, 0x02, 0x00,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, cmpxchg,
               sizeof(cmpxchg)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(invoke_i32(&instance, 0u) == 7);
    assert(invoke_i32(&instance, 1u) == 9);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    module = (turbowasm_module){0};
    instance = (turbowasm_instance){0};
    assert(turbowasm_module_load_borrowed(
               &module, misaligned,
               sizeof(misaligned)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_TRAPPED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_UNALIGNED_ATOMIC);
    assert(strcmp(
               turbowasm_trap_string(trap),
               "unaligned_atomic") == 0);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static const uint8_t shared_provider[] = {
    WASM_HEADER,
    /* () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    /* shared memory min=1 max=1 */
    0x05, 0x04, 0x01, 0x03, 0x01, 0x01,
    /* export mem */
    0x07, 0x07, 0x01,
    0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00,
    /* atomic load address 0 */
    0x0a, 0x0a, 0x01, 0x08, 0x00,
    0x41, 0x00,
    0xfe, 0x10, 0x02, 0x00,
    0x0b
};

static const uint8_t shared_consumer[] = {
    WASM_HEADER,
    /* () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,
    /* import p.mem shared */
    0x02, 0x0b, 0x01,
    0x01, 0x70,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x03, 0x01, 0x01,
    0x03, 0x02, 0x01, 0x00,
    /* i32.atomic.rmw.add address0, value1 */
    0x0a, 0x0c, 0x01, 0x0a, 0x00,
    0x41, 0x00,
    0x41, 0x01,
    0xfe, 0x1e, 0x02, 0x00,
    0x0b
};

typedef struct increment_context {
    turbowasm_instance *instance;
    uint32_t iterations;
} increment_context;

static void increment_thread(void *argument) {
    increment_context *context =
        (increment_context *)argument;
    uint32_t iteration;

    assert(context != NULL);
    for (iteration = 0u;
         iteration < context->iterations;
         ++iteration) {
        (void)invoke_i32(context->instance, 0u);
    }
}

static void test_shared_atomic_rmw_is_lossless(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer_a = {0};
    turbowasm_instance consumer_b = {0};
    turbowasm_linker linker = {0};
    increment_context a = {0};
    increment_context b = {0};
    salts_thread_t thread_a = NULL;
    salts_thread_t thread_b = NULL;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               shared_provider,
               sizeof(shared_provider)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               shared_consumer,
               sizeof(shared_consumer)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               (turbowasm_name){
                   (const uint8_t *)"p", 1u},
               &provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer_a,
               &consumer_module,
               &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &consumer_b,
               &consumer_module,
               &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    a.instance = &consumer_a;
    a.iterations = 1000u;
    b.instance = &consumer_b;
    b.iterations = 1000u;

    assert(salts_thread_create(
               &thread_a, increment_thread, &a) == 0);
    assert(salts_thread_create(
               &thread_b, increment_thread, &b) == 0);
    assert(salts_thread_join(&thread_a) == 0);
    assert(salts_thread_join(&thread_b) == 0);

    assert(invoke_i32(&provider, 0u) == 2000);

    turbowasm_instance_destroy(&consumer_b);
    turbowasm_instance_destroy(&consumer_a);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_backing_rmw_families_and_narrow_cmpxchg(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    uint32_t initial = UINT32_C(0x0f0f0f0f);
    uint32_t actual = 0u;
    uint8_t narrow = 0xffu;
    uint64_t old = 0u;
    const struct {
        uint32_t opcode;
        uint64_t operand;
        uint32_t expected_new;
    } cases[] = {
        {0x1eu, 1u, UINT32_C(0x0f0f0f10)},
        {0x25u, 1u, UINT32_C(0x0f0f0f0e)},
        {0x2cu, UINT32_C(0x00ff00ff), UINT32_C(0x000f000f)},
        {0x33u, UINT32_C(0xf0000000), UINT32_C(0xff0f0f0f)},
        {0x3au, UINT32_C(0xffffffff), UINT32_C(0xf0f0f0f0)},
        {0x41u, UINT32_C(0x12345678), UINT32_C(0x12345678)}
    };
    size_t index;

    assert(turbowasm_module_load_borrowed(
               &module,
               shared_provider,
               sizeof(shared_provider)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    impl = (turbowasm_instance_impl *)instance.impl;
    assert(impl != NULL);

    for (index = 0u;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        const turbowasm_atomic_descriptor *descriptor =
            turbowasm_atomic_descriptor_find(cases[index].opcode);

        assert(descriptor != NULL);
        assert(turbowasm_instance_memory_write_bytes(
                   impl, 0u, 0u, 0u,
                   &initial, sizeof(initial)) == TURBOWASM_OK);
        trap = TURBOWASM_TRAP_NONE;
        assert(turbowasm_instance_memory_atomic(
                   impl, 0u, 0u, 0u,
                   descriptor,
                   cases[index].operand,
                   0u, 0u,
                   &old, &trap) == TURBOWASM_OK);
        assert((uint32_t)old == initial);
        assert(trap == TURBOWASM_TRAP_NONE);
        assert(turbowasm_instance_memory_read_bytes(
                   impl, 0u, 0u, 0u,
                   &actual, sizeof(actual)) == TURBOWASM_OK);
        assert(actual == cases[index].expected_new);
    }

    assert(turbowasm_instance_memory_write_bytes(
               impl, 0u, 0u, 0u,
               &narrow, 1u) == TURBOWASM_OK);
    {
        const turbowasm_atomic_descriptor *descriptor =
            turbowasm_atomic_descriptor_find(0x4au);
        assert(descriptor != NULL);
        assert(turbowasm_instance_memory_atomic(
                   impl, 0u, 0u, 0u,
                   descriptor,
                   0u,
                   UINT64_C(0x1ff),
                   UINT64_C(0x123),
                   &old, &trap) == TURBOWASM_OK);
        assert(old == UINT64_C(0xff));
        assert(turbowasm_instance_memory_read_bytes(
                   impl, 0u, 0u, 0u,
                   &narrow, 1u) == TURBOWASM_OK);
        assert(narrow == 0x23u);
    }

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_unshared_atomic_load_store();
    test_cmpxchg_and_runtime_alignment_trap();
    test_backing_rmw_families_and_narrow_cmpxchg();
    test_shared_atomic_rmw_is_lossless();
    return 0;
}
