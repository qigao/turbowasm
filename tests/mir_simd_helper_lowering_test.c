#include "../src/instance_internal.h"
#include "../src/jit/mir_backend.h"
#include "../src/module_internal.h"
#include "../src/validation_context.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static const uint8_t simd_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60,
    0x00, 0x01, 0x7f, 0x03, 0x09, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, 0x0a, 0xe4, 0x01, 0x08, 0x14,
    0x00, 0x41, 0xff, 0xff, 0xff, 0xff, 0x07, 0xfd, 0x11, 0x41, 0x01, 0xfd,
    0x11, 0xfd, 0xae, 0x01, 0xfd, 0xa4, 0x01, 0x0b, 0x0f, 0x00, 0x41, 0x07,
    0xfd, 0x11, 0x41, 0x07, 0xfd, 0x11, 0xfd, 0x37, 0xfd, 0xa4, 0x01, 0x0b,
    0x1f, 0x00, 0x43, 0x00, 0x00, 0x00, 0x40, 0xfd, 0x13, 0x43, 0x00, 0x00,
    0x40, 0x40, 0xfd, 0x13, 0xfd, 0xe6, 0x01, 0x43, 0x00, 0x00, 0xc0, 0x40,
    0xfd, 0x13, 0xfd, 0x41, 0xfd, 0xa4, 0x01, 0x0b, 0x21, 0x00, 0x41, 0x7f,
    0xfd, 0x11, 0x41, 0x00, 0xfd, 0x11, 0xfd, 0x0c, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0xfd, 0x52, 0xfd, 0xa4, 0x01, 0x0b, 0x0e, 0x00, 0x41, 0x78, 0xfd, 0x11,
    0x41, 0x01, 0xfd, 0xac, 0x01, 0xfd, 0xa4, 0x01, 0x0b, 0x23, 0x00, 0x41,
    0x00, 0xfd, 0x0c, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff,
    0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x0b, 0x04, 0x00, 0x41,
    0x00, 0xfd, 0x00, 0x04, 0x00, 0xfd, 0xa4, 0x01, 0x0b, 0x0d, 0x00, 0x41,
    0xfa, 0xff, 0x03, 0xfd, 0x00, 0x04, 0x00, 0xfd, 0xa4, 0x01, 0x0b, 0x3a,
    0x00, 0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfd, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xfd, 0x0d, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xfd, 0x64, 0x0b
};

static const turbowasm_validation_function *function_at(
    const turbowasm_module_impl *module,
    uint32_t function_index) {
    assert(module != NULL);
    return turbowasm_validation_context_function(
        &module->validation, function_index);
}

static void init_jit_context(
    turbowasm_jit_invocation_context *context,
    turbowasm_instance *instance) {
    memset(context, 0, sizeof(*context));
    context->instance =
        (turbowasm_instance_impl *)instance->impl;
    context->execution = NULL;
    context->depth = 0u;
    context->call_status = TURBOWASM_OK;
    context->call_trap = TURBOWASM_TRAP_NONE;
}

static void compare_i32_success(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    turbowasm_instance *instance,
    uint32_t function_index,
    int32_t expected) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    const turbowasm_validation_function *function =
        function_at(impl, function_index);
    turbowasm_compiled_function compiled = {0};
    turbowasm_value interpreted = {0};
    turbowasm_value generated = {0};
    size_t interpreted_count = 0u;
    size_t generated_count = 0u;
    turbowasm_trap interpreted_trap = TURBOWASM_TRAP_NONE;
    turbowasm_trap generated_trap = TURBOWASM_TRAP_NONE;
    turbowasm_jit_invocation_context context;
    salts_v128 outer_slots[2] = {{{0}}};
    uint8_t before[sizeof(outer_slots)];
    uint8_t after[sizeof(outer_slots)];

    memset(outer_slots, 0xa5, sizeof(outer_slots));
    memcpy(before, outer_slots, sizeof(before));
    init_jit_context(&context, instance);
    context.simd_slots = outer_slots;
    context.simd_slot_count = 2u;

    assert(function != NULL);
    assert(backend->is_function_eligible(
        backend->context,
        &impl->validation,
        function_index,
        function));

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &interpreted, 1u,
               &interpreted_count,
               &interpreted_trap) == TURBOWASM_OK);
    assert(interpreted_count == 1u);
    assert(interpreted.kind == TURBOWASM_VALUE_I32);
    assert(interpreted.as.i32 == expected);
    assert(interpreted_trap == TURBOWASM_TRAP_NONE);

    assert(backend->compile_function(
               backend->context,
               &impl->validation,
               function_index,
               function,
               &compiled) == TURBOWASM_OK);
    assert(compiled.impl != NULL);

    assert(backend->invoke(
               &compiled,
               &context,
               NULL, 0u,
               &generated, 1u,
               &generated_count,
               &generated_trap) == TURBOWASM_OK);
    assert(generated_count == 1u);
    assert(generated.kind == TURBOWASM_VALUE_I32);
    assert(generated.as.i32 == expected);
    assert(generated.as.i32 == interpreted.as.i32);
    assert(generated_trap == TURBOWASM_TRAP_NONE);

    /* The compiled invocation must restore an outer SIMD frame exactly. */
    assert(context.simd_slots == outer_slots);
    assert(context.simd_slot_count == 2u);
    memcpy(after, outer_slots, sizeof(after));
    assert(memcmp(before, after, sizeof(before)) == 0);

    backend->destroy_function(backend->context, &compiled);
    assert(compiled.impl == NULL);
}

static void test_fuel_and_slot_restore(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    turbowasm_instance *instance) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    const turbowasm_validation_function *function =
        function_at(impl, 0u);
    turbowasm_compiled_function compiled = {0};
    turbowasm_value generated = {0};
    size_t generated_count = 99u;
    turbowasm_trap trap = TURBOWASM_TRAP_UNREACHABLE;
    turbowasm_jit_invocation_context context;
    turbowasm_jit_execution_control execution = {0};
    salts_v128 outer_slots[1] = {{{0}}};
    uint8_t before[sizeof(outer_slots)];

    memset(outer_slots, 0x5a, sizeof(outer_slots));
    memcpy(before, outer_slots, sizeof(before));

    assert(function != NULL);
    assert(backend->compile_function(
               backend->context,
               &impl->validation,
               0u,
               function,
               &compiled) == TURBOWASM_OK);

    init_jit_context(&context, instance);
    execution.fuel_limited = true;
    execution.fuel_remaining = 0u;
    context.execution = &execution;
    context.simd_slots = outer_slots;
    context.simd_slot_count = 1u;

    assert(backend->invoke(
               &compiled,
               &context,
               NULL, 0u,
               &generated, 1u,
               &generated_count,
               &trap) == TURBOWASM_FUEL_EXHAUSTED);
    assert(generated_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(context.simd_slots == outer_slots);
    assert(context.simd_slot_count == 1u);
    assert(memcmp(before, outer_slots, sizeof(before)) == 0);

    backend->destroy_function(backend->context, &compiled);
}

static void test_memory_trap_parity(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    turbowasm_instance *instance) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    const turbowasm_validation_function *function =
        function_at(impl, 6u);
    turbowasm_compiled_function compiled = {0};
    turbowasm_value interpreted = {0};
    turbowasm_value generated = {0};
    size_t interpreted_count = 0u;
    size_t generated_count = 0u;
    turbowasm_trap interpreted_trap = TURBOWASM_TRAP_NONE;
    turbowasm_trap generated_trap = TURBOWASM_TRAP_NONE;
    turbowasm_jit_invocation_context context;
    salts_v128 outer_slots[1] = {{{0}}};

    assert(function != NULL);
    assert(backend->is_function_eligible(
        backend->context,
        &impl->validation,
        6u,
        function));

    assert(turbowasm_instance_invoke(
               instance, 6u,
               NULL, 0u,
               &interpreted, 1u,
               &interpreted_count,
               &interpreted_trap) == TURBOWASM_TRAPPED);
    assert(interpreted_trap ==
           TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);

    assert(backend->compile_function(
               backend->context,
               &impl->validation,
               6u,
               function,
               &compiled) == TURBOWASM_OK);

    init_jit_context(&context, instance);
    context.simd_slots = outer_slots;
    context.simd_slot_count = 1u;

    assert(backend->invoke(
               &compiled,
               &context,
               NULL, 0u,
               &generated, 1u,
               &generated_count,
               &generated_trap) == TURBOWASM_TRAPPED);
    assert(generated_trap ==
           TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
    assert(context.simd_slots == outer_slots);
    assert(context.simd_slot_count == 1u);

    backend->destroy_function(backend->context, &compiled);
}

static void test_shuffle_stays_interpreter_fallback(
    turbowasm_jit_backend *backend,
    turbowasm_module *module,
    turbowasm_instance *instance) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    const turbowasm_validation_function *function =
        function_at(impl, 7u);
    turbowasm_compiled_function compiled = {0};
    turbowasm_value interpreted = {0};
    size_t interpreted_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(function != NULL);

    /* The interpreter supports shuffle and produces zero bitmask. */
    assert(turbowasm_instance_invoke(
               instance, 7u,
               NULL, 0u,
               &interpreted, 1u,
               &interpreted_count,
               &trap) == TURBOWASM_OK);
    assert(interpreted_count == 1u);
    assert(interpreted.kind == TURBOWASM_VALUE_I32);
    assert(interpreted.as.i32 == 0);
    assert(trap == TURBOWASM_TRAP_NONE);

    /* Immediate-special shuffle is intentionally outside this MIR slice. */
    assert(!backend->is_function_eligible(
        backend->context,
        &impl->validation,
        7u,
        function));
    assert(backend->compile_function(
               backend->context,
               &impl->validation,
               7u,
               function,
               &compiled) == TURBOWASM_UNSUPPORTED);
    assert(compiled.impl == NULL);
}

int main(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_jit_backend backend = {0};

    assert(turbowasm_module_load_borrowed(
               &module,
               simd_module,
               sizeof(simd_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_create(
               &backend) == TURBOWASM_OK);
    assert(backend.supports_execution_control);

    /* i32x4 add -> bitmask */
    compare_i32_success(
        &backend, &module, &instance, 0u, 15);

    /* i32x4 eq -> bitmask */
    compare_i32_success(
        &backend, &module, &instance, 1u, 15);

    /* f32x4 mul + compare -> bitmask */
    compare_i32_success(
        &backend, &module, &instance, 2u, 15);

    /* bitselect -> lane-sign bitmask 0b0101 */
    compare_i32_success(
        &backend, &module, &instance, 3u, 5);

    /* signed i32x4 shift -> all lanes negative */
    compare_i32_success(
        &backend, &module, &instance, 4u, 15);

    /* v128.store + v128.load -> same lane-sign mask */
    compare_i32_success(
        &backend, &module, &instance, 5u, 5);

    test_fuel_and_slot_restore(
        &backend, &module, &instance);
    test_memory_trap_parity(
        &backend, &module, &instance);
    test_shuffle_stays_interpreter_fallback(
        &backend, &module, &instance);

    backend.destroy_backend(backend.context);
    backend.context = NULL;
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return 0;
}
