#include "instance_internal.h"
#include "module_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct synthetic_memory_fixture {
    turbowasm_validation_memory validation[2];
    turbowasm_module_impl module_impl;
    turbowasm_module module;
    turbowasm_instance_memory memories[2];
    turbowasm_instance_impl instance;
} synthetic_memory_fixture;

static void fixture_init(
    synthetic_memory_fixture *fixture,
    uint32_t count,
    bool shared,
    uint32_t bytes) {
    uint32_t index;

    assert(fixture != NULL);
    assert(count != 0u && count <= 2u);
    memset(fixture, 0, sizeof(*fixture));

    fixture->module.impl = &fixture->module_impl;
    fixture->module_impl.validation.memories = fixture->validation;
    fixture->module_impl.validation.memory_count = count;

    fixture->instance.module = &fixture->module;
    fixture->instance.memories = fixture->memories;
    fixture->instance.memory_count = count;

    for (index = 0u; index < count; ++index) {
        fixture->validation[index].imported = false;
        fixture->validation[index].shared = shared;
        fixture->validation[index].page_size = bytes;
        fixture->validation[index].limits.minimum = 1u;
        fixture->validation[index].limits.maximum = 1u;
        fixture->validation[index].limits.has_maximum = true;

        fixture->memories[index].pages = 1u;
        fixture->memories[index].maximum_pages = 1u;
        fixture->memories[index].page_size = bytes;
        fixture->memories[index].has_maximum = true;

        assert(turbowasm_instance_memory_storage_init(
                   &fixture->memories[index],
                   shared,
                   bytes) == TURBOWASM_OK);
    }
}

static void fixture_destroy(
    synthetic_memory_fixture *fixture,
    uint32_t count) {
    uint32_t index;
    if (fixture == NULL)
        return;
    for (index = 0u; index < count; ++index)
        turbowasm_instance_memory_storage_destroy(
            &fixture->memories[index]);
}

static void test_shared_storage_lifecycle_and_access(void) {
    synthetic_memory_fixture fixture;
    uint8_t input[] = {1u, 2u, 3u, 4u, 5u, 6u};
    uint8_t output[8] = {0};
    uint8_t *raw = NULL;
    uint32_t previous = 0u;

    fixture_init(&fixture, 1u, true, 64u);

    assert(fixture.memories[0].shared);
    assert(fixture.memories[0].access_lock_initialized);
    assert(fixture.memories[0].storage_initialized);
    assert(turbowasm_instance_memory_storage_init(
               &fixture.memories[0], true, 64u) ==
           TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_instance_memory_write_bytes(
               &fixture.instance,
               0u, 0u, 0u,
               input, sizeof(input)) == TURBOWASM_OK);
    assert(turbowasm_instance_memory_read_bytes(
               &fixture.instance,
               0u, 0u, 0u,
               output, sizeof(input)) == TURBOWASM_OK);
    assert(memcmp(input, output, sizeof(input)) == 0);

    assert(turbowasm_instance_memory_fill_bytes(
               &fixture.instance,
               0u, 8u, 0x5au, 4u) == TURBOWASM_OK);
    memset(output, 0, sizeof(output));
    assert(turbowasm_instance_memory_read_bytes(
               &fixture.instance,
               0u, 8u, 0u,
               output, 4u) == TURBOWASM_OK);
    assert(output[0] == 0x5au &&
           output[1] == 0x5au &&
           output[2] == 0x5au &&
           output[3] == 0x5au);

    /* Same backing uses one write lock and memmove overlap semantics. */
    assert(turbowasm_instance_memory_copy_bytes(
               &fixture.instance,
               0u, 0u,
               2u, 0u, 6u) == TURBOWASM_OK);
    memset(output, 0, sizeof(output));
    assert(turbowasm_instance_memory_read_bytes(
               &fixture.instance,
               0u, 0u, 0u,
               output, sizeof(output)) == TURBOWASM_OK);
    assert(output[0] == 1u);
    assert(output[1] == 2u);
    assert(output[2] == 1u);
    assert(output[3] == 2u);
    assert(output[4] == 3u);
    assert(output[5] == 4u);
    assert(output[6] == 5u);
    assert(output[7] == 6u);

    assert(turbowasm_instance_memory_write_bytes(
               &fixture.instance,
               0u, 63u, 0u,
               input, 2u) == TURBOWASM_TRAPPED);

    /*
     * Shared raw-pointer access and grow stay fail-closed until all callers
     * are migrated to guarded operations.
     */
    assert(turbowasm_instance_memory_bounds(
               &fixture.instance,
               0u, 0u, 0u, 1u,
               &raw) == TURBOWASM_UNSUPPORTED);
    assert(turbowasm_instance_memory_grow(
               &fixture.instance,
               0u, 0u, &previous) == TURBOWASM_OK);
    assert(previous == 1u);

    fixture_destroy(&fixture, 1u);
    assert(fixture.memories[0].data == NULL);
    assert(!fixture.memories[0].shared);
    assert(!fixture.memories[0].access_lock_initialized);
    assert(!fixture.memories[0].storage_initialized);

    /* Idempotent cleanup. */
    turbowasm_instance_memory_storage_destroy(
        &fixture.memories[0]);
}

static void test_unshared_direct_path_is_preserved(void) {
    synthetic_memory_fixture fixture;
    uint8_t value = 0x2au;
    uint8_t output = 0u;
    uint8_t *raw = NULL;

    fixture_init(&fixture, 1u, false, 64u);
    assert(!fixture.memories[0].shared);
    assert(!fixture.memories[0].access_lock_initialized);

    assert(turbowasm_instance_memory_write_bytes(
               &fixture.instance,
               0u, 10u, 0u,
               &value, 1u) == TURBOWASM_OK);
    assert(turbowasm_instance_memory_read_bytes(
               &fixture.instance,
               0u, 10u, 0u,
               &output, 1u) == TURBOWASM_OK);
    assert(output == 0x2au);

    assert(turbowasm_instance_memory_bounds(
               &fixture.instance,
               0u, 10u, 0u, 1u,
               &raw) == TURBOWASM_OK);
    assert(raw != NULL && *raw == 0x2au);

    fixture_destroy(&fixture, 1u);
}

static void test_imported_memory_resolves_provider_backing(void) {
    synthetic_memory_fixture provider;
    turbowasm_validation_memory consumer_validation = {0};
    turbowasm_module_impl consumer_module_impl = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance_memory placeholder = {0};
    turbowasm_linked_memory binding = {0};
    turbowasm_instance_impl consumer = {0};
    uint8_t value = 0x77u;
    uint8_t output = 0u;

    fixture_init(&provider, 1u, true, 64u);

    consumer_validation.imported = true;
    consumer_validation.shared = true;
    consumer_validation.page_size = 64u;
    consumer_validation.limits.minimum = 1u;
    consumer_validation.limits.maximum = 1u;
    consumer_validation.limits.has_maximum = true;

    consumer_module_impl.validation.memories =
        &consumer_validation;
    consumer_module_impl.validation.memory_count = 1u;
    consumer_module.impl = &consumer_module_impl;

    binding.provider = &provider.instance;
    binding.memory_index = 0u;

    consumer.module = &consumer_module;
    consumer.memories = &placeholder;
    consumer.memory_count = 1u;
    consumer.linked_memories = &binding;
    consumer.linked_memory_count = 1u;

    assert(turbowasm_instance_memory_write_bytes(
               &consumer, 0u, 12u, 0u,
               &value, 1u) == TURBOWASM_OK);
    assert(turbowasm_instance_memory_read_bytes(
               &provider.instance,
               0u, 12u, 0u,
               &output, 1u) == TURBOWASM_OK);
    assert(output == 0x77u);

    fixture_destroy(&provider, 1u);
}

typedef struct copy_thread_context {
    turbowasm_instance_impl *instance;
    uint32_t destination_memory;
    uint32_t source_memory;
    turbowasm_status status;
} copy_thread_context;

static void copy_thread(void *argument) {
    copy_thread_context *context =
        (copy_thread_context *)argument;
    uint32_t iteration;

    assert(context != NULL);
    context->status = TURBOWASM_OK;
    for (iteration = 0u; iteration < 200u; ++iteration) {
        context->status =
            turbowasm_instance_memory_copy_bytes(
                context->instance,
                context->destination_memory,
                context->source_memory,
                0u, 0u, 1024u);
        if (context->status != TURBOWASM_OK)
            return;
    }
}

static void test_cross_memory_copy_uses_stable_lock_order(void) {
    synthetic_memory_fixture fixture;
    copy_thread_context forward = {0};
    copy_thread_context reverse = {0};
    cmeta_thread_t thread_a = NULL;
    cmeta_thread_t thread_b = NULL;
    uint8_t a[1024];
    uint8_t b[1024];

    fixture_init(&fixture, 2u, true, 1024u);
    memset(a, 0x11, sizeof(a));
    memset(b, 0x22, sizeof(b));

    assert(turbowasm_instance_memory_write_bytes(
               &fixture.instance, 0u, 0u, 0u,
               a, sizeof(a)) == TURBOWASM_OK);
    assert(turbowasm_instance_memory_write_bytes(
               &fixture.instance, 1u, 0u, 0u,
               b, sizeof(b)) == TURBOWASM_OK);

    forward.instance = &fixture.instance;
    forward.destination_memory = 1u;
    forward.source_memory = 0u;
    reverse.instance = &fixture.instance;
    reverse.destination_memory = 0u;
    reverse.source_memory = 1u;

    assert(cmeta_thread_create(
               &thread_a, copy_thread, &forward) == 0);
    assert(cmeta_thread_create(
               &thread_b, copy_thread, &reverse) == 0);
    assert(cmeta_thread_join(&thread_a) == 0);
    assert(cmeta_thread_join(&thread_b) == 0);
    cmeta_thread_destroy(&thread_a);
    cmeta_thread_destroy(&thread_b);

    assert(forward.status == TURBOWASM_OK);
    assert(reverse.status == TURBOWASM_OK);

    fixture_destroy(&fixture, 2u);
}

int main(void) {
    test_shared_storage_lifecycle_and_access();
    test_unshared_direct_path_is_preserved();
    test_imported_memory_resolves_provider_backing();
    test_cross_memory_copy_uses_stable_lock_order();
    return 0;
}
