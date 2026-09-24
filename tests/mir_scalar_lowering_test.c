#include "../src/jit/mir_backend.h"
#include "../src/module_internal.h"
#include "../src/validation_context.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static void compare_i32(
    const uint8_t *bytes,
    size_t size,
    int32_t expected) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *function;
    turbowasm_value interpreted = {0};
    turbowasm_value generated = {0};
    size_t interpreted_count = 0u;
    size_t generated_count = 0u;
    turbowasm_trap interpreted_trap = TURBOWASM_TRAP_NONE;
    turbowasm_trap generated_trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, size) == TURBOWASM_OK);
    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    function = turbowasm_validation_context_function(
        &impl->validation, 0u);
    assert(function != NULL);

    assert(turbowasm_mir_backend_create(&backend) == TURBOWASM_OK);
    assert(backend.is_function_eligible(
        backend.context, &impl->validation, 0u, function));
    assert(backend.compile_function(
               backend.context, &impl->validation, 0u, function,
               &compiled) == TURBOWASM_OK);
    assert(compiled.impl != NULL);

    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               &interpreted, 1u,
               &interpreted_count,
               &interpreted_trap) == TURBOWASM_OK);

    assert(backend.invoke(
               &compiled,
               (struct turbowasm_instance_impl *)instance.impl,
               NULL, 0u,
               &generated, 1u,
               &generated_count,
               &generated_trap,
               NULL) == TURBOWASM_OK);

    assert(interpreted_count == 1u);
    assert(generated_count == 1u);
    assert(interpreted_trap == TURBOWASM_TRAP_NONE);
    assert(generated_trap == TURBOWASM_TRAP_NONE);
    assert(interpreted.kind == TURBOWASM_VALUE_I32);
    assert(generated.kind == TURBOWASM_VALUE_I32);
    assert(interpreted.as.i32 == expected);
    assert(generated.as.i32 == interpreted.as.i32);

    generated_count = 123u;
    generated_trap = TURBOWASM_TRAP_UNREACHABLE;
    assert(backend.invoke(
               &compiled,
               (struct turbowasm_instance_impl *)instance.impl,
               NULL, 0u,
               &generated, 1u,
               &generated_count,
               &generated_trap,
               (turbowasm_jit_execution_control *)(uintptr_t)1u) ==
           TURBOWASM_UNSUPPORTED);
    assert(generated_count == 0u);
    assert(generated_trap == TURBOWASM_TRAP_NONE);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void compare_i64(
    const uint8_t *bytes,
    size_t size,
    int64_t expected) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {0};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *function;
    turbowasm_value interpreted = {0};
    turbowasm_value generated = {0};
    size_t interpreted_count = 0u;
    size_t generated_count = 0u;
    turbowasm_trap interpreted_trap = TURBOWASM_TRAP_NONE;
    turbowasm_trap generated_trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, size) == TURBOWASM_OK);
    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    function = turbowasm_validation_context_function(
        &impl->validation, 0u);
    assert(function != NULL);

    assert(turbowasm_mir_backend_create(&backend) == TURBOWASM_OK);
    assert(backend.is_function_eligible(
        backend.context, &impl->validation, 0u, function));
    assert(backend.compile_function(
               backend.context, &impl->validation, 0u, function,
               &compiled) == TURBOWASM_OK);

    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               &interpreted, 1u,
               &interpreted_count,
               &interpreted_trap) == TURBOWASM_OK);
    assert(backend.invoke(
               &compiled,
               (struct turbowasm_instance_impl *)instance.impl,
               NULL, 0u,
               &generated, 1u,
               &generated_count,
               &generated_trap,
               NULL) == TURBOWASM_OK);

    assert(interpreted_count == 1u);
    assert(generated_count == 1u);
    assert(interpreted_trap == TURBOWASM_TRAP_NONE);
    assert(generated_trap == TURBOWASM_TRAP_NONE);
    assert(interpreted.kind == TURBOWASM_VALUE_I64);
    assert(generated.kind == TURBOWASM_VALUE_I64);
    assert(interpreted.as.i64 == expected);
    assert(generated.as.i64 == interpreted.as.i64);

    backend.destroy_function(backend.context, &compiled);
    backend.destroy_backend(backend.context);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_unsupported_function_fails_closed(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x09,
        0x01, 0x07,
        0x00,
        0x41, 0x08,
        0x41, 0x02,
        0x6d,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_jit_backend backend = {0};
    turbowasm_compiled_function compiled = {(void *)(uintptr_t)1u};
    const turbowasm_module_impl *impl;
    const turbowasm_validation_function *function;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    impl = turbowasm_module_impl_get(&module);
    assert(impl != NULL);
    function = turbowasm_validation_context_function(
        &impl->validation, 0u);
    assert(function != NULL);

    assert(turbowasm_mir_backend_create(&backend) == TURBOWASM_OK);
    assert(!backend.is_function_eligible(
        backend.context, &impl->validation, 0u, function));
    assert(backend.compile_function(
               backend.context, &impl->validation, 0u, function,
               &compiled) == TURBOWASM_UNSUPPORTED);
    assert(compiled.impl == NULL);

    backend.destroy_backend(backend.context);
    turbowasm_module_destroy(&module);
}

int main(void) {
    static const uint8_t i32_bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x13,
        0x01, 0x11,
        0x00,
        0x41, 0x80, 0x80, 0x04,
        0x41, 0x80, 0x80, 0x04,
        0x6c,
        0x41, 0x07,
        0x6a,
        0x41, 0x07,
        0x6b,
        0x0b
    };
    static const uint8_t i64_bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7e,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x0f,
        0x01, 0x0d,
        0x00,
        0x42, 0x0a,
        0x42, 0x05,
        0x7c,
        0x42, 0x03,
        0x7e,
        0x42, 0x03,
        0x7d,
        0x0b
    };

    compare_i32(i32_bytes, sizeof(i32_bytes), 0);
    compare_i64(i64_bytes, sizeof(i64_bytes), INT64_C(42));
    test_unsupported_function_fails_closed();
    return 0;
}
