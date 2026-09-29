#include "../src/instance_internal.h"
#include "../src/artifact.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

enum {
    FAKE_BLOB_SIZE = 16u,
    FAKE_CACHE_CAPACITY = 64u
};

typedef struct fake_compiled {
    int32_t value;
} fake_compiled;

typedef struct fake_backend_state {
    uint8_t fingerprint_seed;
    uint32_t compile_attempts;
    uint32_t restore_attempts;
    uint32_t measure_attempts;
    uint32_t write_attempts;
    uint32_t invoke_count;
    uint32_t destroy_function_count;
    uint32_t destroy_backend_count;
} fake_backend_state;

typedef struct fake_cache {
    bool has_blob;
    bool fail_store;
    turbowasm_jit_artifact_key key;
    uint8_t bytes[FAKE_CACHE_CAPACITY];
    size_t size;
    uint32_t lookup_count;
    uint32_t hit_count;
    uint32_t release_count;
    uint32_t store_count;
} fake_cache;

static bool key_equal(
    const turbowasm_jit_artifact_key *a,
    const turbowasm_jit_artifact_key *b) {
    return a != NULL && b != NULL &&
           a->validation_feature_fingerprint ==
               b->validation_feature_fingerprint &&
           a->function_index == b->function_index &&
           memcmp(
               a->source_sha256,
               b->source_sha256,
               sizeof(a->source_sha256)) == 0 &&
           memcmp(
               a->backend_fingerprint,
               b->backend_fingerprint,
               sizeof(a->backend_fingerprint)) == 0;
}

static bool cache_lookup(
    void *context,
    const turbowasm_jit_artifact_key *key,
    turbowasm_jit_artifact_view *out) {
    fake_cache *cache = (fake_cache *)context;

    assert(cache != NULL);
    assert(key != NULL);
    assert(out != NULL);
    ++cache->lookup_count;
    out->bytes = NULL;
    out->size = 0u;

    if (!cache->has_blob || !key_equal(key, &cache->key))
        return false;

    ++cache->hit_count;
    out->bytes = cache->bytes;
    out->size = cache->size;
    return true;
}

static void cache_release(
    void *context,
    turbowasm_jit_artifact_view *view) {
    fake_cache *cache = (fake_cache *)context;

    assert(cache != NULL);
    assert(view != NULL);
    ++cache->release_count;
    view->bytes = NULL;
    view->size = 0u;
}

static turbowasm_status cache_store(
    void *context,
    const turbowasm_jit_artifact_key *key,
    const uint8_t *bytes,
    size_t size) {
    fake_cache *cache = (fake_cache *)context;

    assert(cache != NULL);
    assert(key != NULL);
    assert(bytes != NULL);
    ++cache->store_count;

    if (cache->fail_store)
        return TURBOWASM_UNSUPPORTED;
    if (size > sizeof(cache->bytes))
        return TURBOWASM_OUT_OF_MEMORY;

    cache->key = *key;
    memcpy(cache->bytes, bytes, size);
    cache->size = size;
    cache->has_blob = true;
    return TURBOWASM_OK;
}

static bool fake_eligible(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function) {
    (void)context;
    (void)validation;
    return function_index == 0u &&
           function != NULL &&
           !function->imported;
}

static turbowasm_status fake_compile(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function,
    turbowasm_compiled_function *out) {
    fake_backend_state *state = (fake_backend_state *)context;
    fake_compiled *compiled;

    (void)validation;
    assert(state != NULL);
    assert(function_index == 0u);
    assert(function != NULL);
    assert(out != NULL);

    ++state->compile_attempts;
    out->impl = NULL;

    compiled = (fake_compiled *)calloc(1u, sizeof(*compiled));
    if (compiled == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    compiled->value = 42;
    out->impl = compiled;
    return TURBOWASM_OK;
}

static bool fake_artifact_fingerprint(
    void *context,
    uint8_t out[TURBOWASM_JIT_ARTIFACT_FINGERPRINT_SIZE]) {
    fake_backend_state *state = (fake_backend_state *)context;
    size_t index;

    assert(state != NULL);
    assert(out != NULL);
    for (index = 0u;
         index < TURBOWASM_JIT_ARTIFACT_FINGERPRINT_SIZE;
         ++index) {
        out[index] = (uint8_t)(state->fingerprint_seed + index);
    }
    return true;
}

static uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static void write_u32le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
    p[2] = (uint8_t)(value >> 16u);
    p[3] = (uint8_t)(value >> 24u);
}

static uint32_t fake_checksum(
    uint32_t function_index,
    uint32_t value) {
    return UINT32_C(0x6a697463) ^
           function_index ^
           value;
}

static turbowasm_status fake_restore(
    void *context,
    const struct turbowasm_validation_context *validation,
    uint32_t function_index,
    const struct turbowasm_validation_function *function,
    const uint8_t *bytes,
    size_t size,
    turbowasm_compiled_function *out) {
    fake_backend_state *state = (fake_backend_state *)context;
    fake_compiled *compiled;
    uint32_t artifact_function;
    uint32_t value;
    uint32_t checksum;

    (void)validation;
    assert(state != NULL);
    assert(function != NULL);
    assert(out != NULL);
    ++state->restore_attempts;
    out->impl = NULL;

    if (bytes == NULL || size != FAKE_BLOB_SIZE ||
        memcmp(bytes, "TJCA", 4u) != 0)
        return TURBOWASM_MALFORMED_MODULE;

    artifact_function = read_u32le(bytes + 4u);
    value = read_u32le(bytes + 8u);
    checksum = read_u32le(bytes + 12u);
    if (artifact_function != function_index ||
        checksum != fake_checksum(artifact_function, value))
        return TURBOWASM_MALFORMED_MODULE;

    compiled = (fake_compiled *)calloc(1u, sizeof(*compiled));
    if (compiled == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    compiled->value = (int32_t)value;
    out->impl = compiled;
    return TURBOWASM_OK;
}

static turbowasm_status fake_measure(
    void *context,
    const turbowasm_compiled_function *compiled,
    size_t *out_size) {
    fake_backend_state *state = (fake_backend_state *)context;

    assert(state != NULL);
    assert(compiled != NULL);
    assert(compiled->impl != NULL);
    assert(out_size != NULL);
    ++state->measure_attempts;
    *out_size = FAKE_BLOB_SIZE;
    return TURBOWASM_OK;
}

static turbowasm_status fake_write(
    void *context,
    const turbowasm_compiled_function *compiled,
    uint8_t *output,
    size_t capacity,
    size_t *out_size) {
    fake_backend_state *state = (fake_backend_state *)context;
    const fake_compiled *function;

    assert(state != NULL);
    assert(compiled != NULL);
    assert(compiled->impl != NULL);
    assert(out_size != NULL);
    ++state->write_attempts;
    *out_size = FAKE_BLOB_SIZE;
    if (output == NULL || capacity < FAKE_BLOB_SIZE)
        return TURBOWASM_OUT_OF_MEMORY;

    function = (const fake_compiled *)compiled->impl;
    memcpy(output, "TJCA", 4u);
    write_u32le(output + 4u, 0u);
    write_u32le(output + 8u, (uint32_t)function->value);
    write_u32le(
        output + 12u,
        fake_checksum(0u, (uint32_t)function->value));
    return TURBOWASM_OK;
}

static turbowasm_status fake_invoke(
    const turbowasm_compiled_function *compiled,
    turbowasm_jit_invocation_context *context,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    const fake_compiled *function;
    fake_backend_state *state;

    assert(compiled != NULL && compiled->impl != NULL);
    assert(context != NULL && context->instance != NULL);
    assert(arguments == NULL);
    assert(argument_count == 0u);
    assert(results != NULL && result_capacity >= 1u);
    assert(result_count != NULL);
    assert(trap != NULL);

    state = (fake_backend_state *)
        context->instance->jit_backend.context;
    assert(state != NULL);
    ++state->invoke_count;

    function = (const fake_compiled *)compiled->impl;
    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = function->value;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static void fake_destroy_function(
    void *context,
    turbowasm_compiled_function *compiled) {
    fake_backend_state *state = (fake_backend_state *)context;

    assert(state != NULL);
    if (compiled != NULL && compiled->impl != NULL) {
        ++state->destroy_function_count;
        free(compiled->impl);
        compiled->impl = NULL;
    }
}

static void fake_destroy_backend(void *context) {
    fake_backend_state *state = (fake_backend_state *)context;
    assert(state != NULL);
    ++state->destroy_backend_count;
}

static turbowasm_jit_backend make_backend(
    fake_backend_state *state,
    bool persistent) {
    turbowasm_jit_backend backend = {0};

    backend.context = state;
    backend.is_function_eligible = fake_eligible;
    backend.compile_function = fake_compile;
    backend.invoke = fake_invoke;
    backend.destroy_function = fake_destroy_function;
    backend.destroy_backend = fake_destroy_backend;

    if (persistent) {
        backend.artifact_fingerprint = fake_artifact_fingerprint;
        backend.restore_function_artifact = fake_restore;
        backend.measure_function_artifact = fake_measure;
        backend.write_function_artifact = fake_write;
    }
    return backend;
}

static turbowasm_jit_artifact_cache make_cache(
    fake_cache *cache,
    size_t max_blob_bytes) {
    turbowasm_jit_artifact_cache out = {0};
    out.context = cache;
    out.max_blob_bytes = max_blob_bytes;
    out.lookup = cache_lookup;
    out.release = cache_release;
    out.store = cache_store;
    return out;
}

static int32_t invoke_i32(turbowasm_instance *instance) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void run_once(
    turbowasm_module *module,
    fake_cache *cache_state,
    size_t max_blob_bytes,
    fake_backend_state *backend_state) {
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    turbowasm_jit_backend backend =
        make_backend(backend_state, true);
    turbowasm_jit_artifact_cache cache =
        make_cache(cache_state, max_blob_bytes);

    assert(turbowasm_instance_create(
               &instance, module) == TURBOWASM_OK);
    impl = (turbowasm_instance_impl *)instance.impl;
    assert(impl != NULL);
    assert(turbowasm_jit_instance_attach_backend_with_cache(
               impl, &backend, 1u, &cache) == TURBOWASM_OK);
    assert(backend.context == NULL);
    assert(invoke_i32(&instance) == 42);
    turbowasm_instance_destroy(&instance);
}

static void seed_cache(
    turbowasm_module *module,
    fake_cache *cache,
    uint8_t fingerprint_seed) {
    fake_backend_state state = {0};
    uint8_t expected_sha[32] = {0};
    const uint8_t *source = turbowasm_module_bytes(module);
    size_t source_size = turbowasm_module_size(module);

    state.fingerprint_seed = fingerprint_seed;
    run_once(module, cache, FAKE_CACHE_CAPACITY, &state);

    assert(state.compile_attempts == 1u);
    assert(state.restore_attempts == 0u);
    assert(state.measure_attempts == 1u);
    assert(state.write_attempts == 1u);
    assert(cache->lookup_count == 1u);
    assert(cache->hit_count == 0u);
    assert(cache->store_count == 1u);
    assert(cache->has_blob);
    assert(cache->size == FAKE_BLOB_SIZE);
    assert(cache->key.function_index == 0u);
    assert(cache->key.validation_feature_fingerprint ==
           turbowasm_artifact_current_feature_fingerprint());

    turbowasm_sha256(source, source_size, expected_sha);
    assert(memcmp(
               cache->key.source_sha256,
               expected_sha,
               sizeof(expected_sha)) == 0);
}

static void test_miss_store_then_hit_restore(
    turbowasm_module *module) {
    fake_cache cache = {0};
    fake_backend_state restored_state = {0};

    seed_cache(module, &cache, 7u);

    restored_state.fingerprint_seed = 7u;
    run_once(
        module, &cache, FAKE_CACHE_CAPACITY,
        &restored_state);

    assert(restored_state.compile_attempts == 0u);
    assert(restored_state.restore_attempts == 1u);
    assert(restored_state.measure_attempts == 0u);
    assert(restored_state.write_attempts == 0u);
    assert(cache.lookup_count == 2u);
    assert(cache.hit_count == 1u);
    assert(cache.release_count == 1u);
    assert(cache.store_count == 1u);
}

static void test_backend_fingerprint_invalidates(
    turbowasm_module *module) {
    fake_cache cache = {0};
    fake_backend_state state = {0};

    seed_cache(module, &cache, 3u);
    state.fingerprint_seed = 4u;
    run_once(module, &cache, FAKE_CACHE_CAPACITY, &state);

    assert(state.restore_attempts == 0u);
    assert(state.compile_attempts == 1u);
    assert(cache.hit_count == 0u);
    assert(cache.store_count == 2u);
}

static void test_source_identity_invalidates(
    turbowasm_module *module,
    turbowasm_module *variant) {
    fake_cache cache = {0};
    fake_backend_state state = {0};

    seed_cache(module, &cache, 11u);
    state.fingerprint_seed = 11u;
    run_once(variant, &cache, FAKE_CACHE_CAPACITY, &state);

    assert(state.restore_attempts == 0u);
    assert(state.compile_attempts == 1u);
    assert(cache.hit_count == 0u);
}

static void test_corruption_falls_back_to_compile(
    turbowasm_module *module) {
    fake_cache cache = {0};
    fake_backend_state state = {0};

    seed_cache(module, &cache, 19u);
    cache.bytes[0] ^= 1u;

    state.fingerprint_seed = 19u;
    run_once(module, &cache, FAKE_CACHE_CAPACITY, &state);

    assert(state.restore_attempts == 1u);
    assert(state.compile_attempts == 1u);
    assert(cache.hit_count == 1u);
    assert(cache.release_count == 1u);
    assert(cache.store_count == 2u);
}

static void test_oversize_hit_skips_backend_restore(
    turbowasm_module *module) {
    fake_cache cache = {0};
    fake_backend_state state = {0};

    seed_cache(module, &cache, 23u);
    cache.size = FAKE_BLOB_SIZE;
    state.fingerprint_seed = 23u;

    run_once(module, &cache, FAKE_BLOB_SIZE - 1u, &state);

    assert(state.restore_attempts == 0u);
    assert(state.compile_attempts == 1u);
    assert(state.measure_attempts == 1u);
    assert(state.write_attempts == 0u);
    assert(cache.hit_count == 1u);
    assert(cache.release_count == 1u);
}

static void test_store_failure_is_nonfatal(
    turbowasm_module *module) {
    fake_cache cache = {0};
    fake_backend_state state = {0};

    cache.fail_store = true;
    state.fingerprint_seed = 29u;
    run_once(module, &cache, FAKE_CACHE_CAPACITY, &state);

    assert(state.compile_attempts == 1u);
    assert(state.restore_attempts == 0u);
    assert(cache.store_count == 1u);
    assert(!cache.has_blob);
}

static void test_nonpersistent_backend_ignores_cache(
    turbowasm_module *module) {
    fake_cache cache_state = {0};
    fake_backend_state backend_state = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    turbowasm_jit_backend backend =
        make_backend(&backend_state, false);
    turbowasm_jit_artifact_cache cache =
        make_cache(&cache_state, FAKE_CACHE_CAPACITY);

    assert(turbowasm_instance_create(
               &instance, module) == TURBOWASM_OK);
    impl = (turbowasm_instance_impl *)instance.impl;
    assert(impl != NULL);
    assert(turbowasm_jit_instance_attach_backend_with_cache(
               impl, &backend, 1u, &cache) == TURBOWASM_OK);
    assert(!impl->jit_artifact_cache_enabled);
    assert(invoke_i32(&instance) == 42);
    assert(backend_state.compile_attempts == 1u);
    assert(cache_state.lookup_count == 0u);
    assert(cache_state.store_count == 0u);
    turbowasm_instance_destroy(&instance);
}

int main(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x06,
        0x01, 0x04,
        0x00, 0x41, 0x2a, 0x0b
    };
    static const uint8_t variant_bytes[] = {
        WASM_HEADER,
        /* custom section with one payload byte changes exact source identity */
        0x00, 0x02, 0x00, 0x7a,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x06,
        0x01, 0x04,
        0x00, 0x41, 0x2a, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_module variant = {0};

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &variant,
               variant_bytes,
               sizeof(variant_bytes)) == TURBOWASM_OK);

    test_miss_store_then_hit_restore(&module);
    test_backend_fingerprint_invalidates(&module);
    test_source_identity_invalidates(&module, &variant);
    test_corruption_falls_back_to_compile(&module);
    test_oversize_hit_skips_backend_restore(&module);
    test_store_failure_is_nonfatal(&module);
    test_nonpersistent_backend_ignores_cache(&module);

    turbowasm_module_destroy(&variant);
    turbowasm_module_destroy(&module);
    return 0;
}
