#include <turbowasm/turbowasm.h>

#include "instance_internal.h"

#include <salts/thread.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static const uint8_t provider_bytes[] = {
    WASM_HEADER,

    /* type0: (i32) -> i32 */
    0x01, 0x06,
    0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

    /* one local reader */
    0x03, 0x02,
    0x01, 0x00,

    /* shared memory32 min=1 max=2 */
    0x05, 0x04,
    0x01, 0x03, 0x01, 0x02,

    /* export memory as p.mem */
    0x07, 0x07,
    0x01, 0x03, 0x6d, 0x65, 0x6d, 0x02, 0x00,

    /* f0(addr) -> i32.load(addr) */
    0x0a, 0x09,
    0x01, 0x07,
    0x00,
    0x20, 0x00,
    0x28, 0x02, 0x00,
    0x0b
};

static const uint8_t consumer_bytes[] = {
    WASM_HEADER,

    /* type0: (i32,i32)->()
       type1: (i32)->i32
       type2: ()->i32 */
    0x01, 0x0f,
    0x03,
    0x60, 0x02, 0x7f, 0x7f, 0x00,
    0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x60, 0x00, 0x01, 0x7f,

    /* import p.mem shared min=1 max=2 */
    0x02, 0x0b,
    0x01,
    0x01, 0x70,
    0x03, 0x6d, 0x65, 0x6d,
    0x02, 0x03, 0x01, 0x02,

    /* f0 store, f1 grow, f2 size */
    0x03, 0x04,
    0x03, 0x00, 0x01, 0x02,

    0x0a, 0x17,
    0x03,

    /* f0(addr,value): i32.store */
    0x09,
    0x00,
    0x20, 0x00,
    0x20, 0x01,
    0x36, 0x02, 0x00,
    0x0b,

    /* f1(delta): memory.grow */
    0x06,
    0x00,
    0x20, 0x00,
    0x40, 0x00,
    0x0b,

    /* f2(): memory.size */
    0x04,
    0x00,
    0x3f, 0x00,
    0x0b
};

static turbowasm_name name_span(
    const char *text,
    uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
    return out;
}

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
               &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void invoke_store(
    turbowasm_instance *instance,
    uint32_t address,
    uint32_t value) {
    turbowasm_value args[2] = {
        i32_value((int32_t)address),
        i32_value((int32_t)value)
    };
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, 0u,
               args, 2u,
               NULL, 0u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
}

typedef struct store_thread_context {
    turbowasm_instance *instance;
    uint32_t address;
    uint32_t value;
    uint32_t iterations;
} store_thread_context;

static void store_thread(void *argument) {
    store_thread_context *context =
        (store_thread_context *)argument;
    uint32_t iteration;

    assert(context != NULL);
    for (iteration = 0u;
         iteration < context->iterations;
         ++iteration) {
        invoke_store(
            context->instance,
            context->address,
            context->value);
    }
}

typedef struct size_thread_context {
    turbowasm_instance *instance;
    uint32_t iterations;
    int32_t minimum_seen;
    int32_t maximum_seen;
} size_thread_context;

static void size_thread(void *argument) {
    size_thread_context *context =
        (size_thread_context *)argument;
    uint32_t iteration;

    assert(context != NULL);
    context->minimum_seen = INT32_MAX;
    context->maximum_seen = INT32_MIN;

    for (iteration = 0u;
         iteration < context->iterations;
         ++iteration) {
        int32_t pages = invoke_i32(
            context->instance, 2u, NULL, 0u);
        assert(pages == 1 || pages == 2);
        if (pages < context->minimum_seen)
            context->minimum_seen = pages;
        if (pages > context->maximum_seen)
            context->maximum_seen = pages;
    }
}

typedef struct grow_thread_context {
    turbowasm_instance *instance;
    int32_t previous_pages;
} grow_thread_context;

static void grow_thread(void *argument) {
    grow_thread_context *context =
        (grow_thread_context *)argument;
    turbowasm_value delta = i32_value(1);

    assert(context != NULL);
    context->previous_pages = invoke_i32(
        context->instance, 1u, &delta, 1u);
}

static void create_linked_consumers(
    turbowasm_instance *provider,
    turbowasm_module *consumer_module,
    turbowasm_instance *first,
    turbowasm_instance *second) {
    turbowasm_linker linker = {0};

    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_instance(
               &linker,
               name_span("p", 1u),
               provider) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               first, consumer_module, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               second, consumer_module, &linker) == TURBOWASM_OK);

    /* Bindings retain provider identity; linker lifetime ends here. */
    turbowasm_linker_destroy(&linker);
}

static void test_shared_backing_across_linked_instances(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer_a = {0};
    turbowasm_instance consumer_b = {0};
    store_thread_context a = {0};
    store_thread_context b = {0};
    salts_thread_t thread_a = NULL;
    salts_thread_t thread_b = NULL;
    turbowasm_value address;
    uint8_t *raw = NULL;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);

    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    create_linked_consumers(
        &provider, &consumer_module,
        &consumer_a, &consumer_b);

    /*
     * Public/raw span plumbing cannot leak an unlocked shared pointer.
     */
    assert(turbowasm_instance_memory_bounds(
               (const turbowasm_instance_impl *)provider.impl,
               0u, 0u, 0u, 1u,
               &raw) == TURBOWASM_UNSUPPORTED);

    a.instance = &consumer_a;
    a.address = 0u;
    a.value = UINT32_C(0x11223344);
    a.iterations = 500u;

    b.instance = &consumer_b;
    b.address = 4u;
    b.value = UINT32_C(0x55667788);
    b.iterations = 500u;

    assert(salts_thread_create(
               &thread_a, store_thread, &a) == 0);
    assert(salts_thread_create(
               &thread_b, store_thread, &b) == 0);
    assert(salts_thread_join(&thread_a) == 0);
    assert(salts_thread_join(&thread_b) == 0);

    address = i32_value(0);
    assert((uint32_t)invoke_i32(
               &provider, 0u, &address, 1u) ==
           UINT32_C(0x11223344));
    address = i32_value(4);
    assert((uint32_t)invoke_i32(
               &provider, 0u, &address, 1u) ==
           UINT32_C(0x55667788));

    turbowasm_instance_destroy(&consumer_b);
    turbowasm_instance_destroy(&consumer_a);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

static void test_concurrent_size_and_grow(void) {
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance grower = {0};
    turbowasm_instance reader = {0};
    grow_thread_context grow = {0};
    size_thread_context size = {0};
    salts_thread_t grow_thread_handle = NULL;
    salts_thread_t size_thread_handle = NULL;

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               provider_bytes,
               sizeof(provider_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &consumer_module,
               consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    create_linked_consumers(
        &provider, &consumer_module,
        &grower, &reader);

    grow.instance = &grower;
    size.instance = &reader;
    size.iterations = 500u;

    assert(salts_thread_create(
               &size_thread_handle,
               size_thread,
               &size) == 0);
    assert(salts_thread_create(
               &grow_thread_handle,
               grow_thread,
               &grow) == 0);

    assert(salts_thread_join(
               &grow_thread_handle) == 0);
    assert(salts_thread_join(
               &size_thread_handle) == 0);

    assert(grow.previous_pages == 1);
    assert(size.minimum_seen >= 1);
    assert(size.maximum_seen <= 2);
    assert(invoke_i32(&reader, 2u, NULL, 0u) == 2);

    turbowasm_instance_destroy(&reader);
    turbowasm_instance_destroy(&grower);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
}

int main(void) {
    test_shared_backing_across_linked_instances();
    test_concurrent_size_and_grow();
    return 0;
}
