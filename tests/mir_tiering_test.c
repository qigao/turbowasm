#include "../src/instance_internal.h"
#include "../src/jit/mir_backend.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static int32_t invoke_i32_at(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(trap == TURBOWASM_TRAP_NONE);
    return result.as.i32;
}

static int32_t invoke_i32(turbowasm_instance *instance) {
    return invoke_i32_at(instance, 0u);
}

int main(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,
        0x01, 0x05,
        0x01, 0x60, 0x00, 0x01, 0x7f,
        0x03, 0x02,
        0x01, 0x00,
        0x0a, 0x06,
        0x01,
        0x04, 0x00, 0x41, 0x2a, 0x0b
    };

    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    turbowasm_jit_backend backend = {0};
    turbowasm_execution_options options = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    void *compiled_impl;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);
    impl = (turbowasm_instance_impl *)instance.impl;
    assert(impl != NULL);

    assert(turbowasm_mir_backend_create(&backend) == TURBOWASM_OK);
    assert(backend.supports_execution_control);
    assert(turbowasm_jit_instance_attach_backend(
               impl, &backend, 2u) == TURBOWASM_OK);
    assert(backend.context == NULL);
    assert(impl->jit_function_count == 1u);
    assert(impl->jit_functions[0].state == TURBOWASM_JIT_INTERPRET);
    assert(impl->jit_functions[0].call_count == 0u);

    /* Cold call remains interpreted. */
    assert(invoke_i32(&instance) == 42);
    assert(impl->jit_functions[0].state == TURBOWASM_JIT_INTERPRET);
    assert(impl->jit_functions[0].call_count == 1u);
    assert(impl->jit_functions[0].compiled.impl == NULL);

    /* Threshold call compiles and immediately uses native code. */
    assert(invoke_i32(&instance) == 42);
    assert(impl->jit_functions[0].state == TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[0].call_count == 2u);
    assert(impl->jit_functions[0].compiled.impl != NULL);
    compiled_impl = impl->jit_functions[0].compiled.impl;

    /* Compiled handle is reused rather than regenerated. */
    assert(invoke_i32(&instance) == 42);
    assert(impl->jit_functions[0].compiled.impl == compiled_impl);
    assert(impl->jit_functions[0].call_count == 2u);

    /* Execution-control calls use native safe points and preserve the cache. */
    options.has_fuel_limit = true;
    options.fuel = 2u;
    assert(turbowasm_instance_invoke_with_options(
               &instance, 0u,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap,
               &options) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 42);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(impl->jit_functions[0].state == TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[0].compiled.impl == compiled_impl);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);

    /*
     * Tail-call functions are not MIR-eligible yet. They must become
     * interpreter-only per function rather than being miscompiled as an
     * ordinary call followed by return.
     */
    {
        static const uint8_t tail_bytes[] = {
            WASM_HEADER,
            0x01, 0x05,
            0x01, 0x60, 0x00, 0x01, 0x7f,
            0x03, 0x03,
            0x02, 0x00, 0x00,
            0x0a, 0x0b,
            0x02,
            0x04, 0x00, 0x41, 0x07, 0x0b,
            0x04, 0x00, 0x12, 0x00, 0x0b
        };
        turbowasm_module tail_module = {0};
        turbowasm_instance tail_instance = {0};
        turbowasm_instance_impl *tail_impl;
        turbowasm_jit_backend tail_backend = {0};

        assert(turbowasm_module_load_borrowed(
                   &tail_module,
                   tail_bytes,
                   sizeof(tail_bytes)) == TURBOWASM_OK);
        assert(turbowasm_instance_create(
                   &tail_instance,
                   &tail_module) == TURBOWASM_OK);
        tail_impl = (turbowasm_instance_impl *)tail_instance.impl;
        assert(tail_impl != NULL);

        assert(turbowasm_mir_backend_create(
                   &tail_backend) == TURBOWASM_OK);
        assert(turbowasm_jit_instance_attach_backend(
                   tail_impl, &tail_backend, 1u) == TURBOWASM_OK);

        assert(invoke_i32_at(&tail_instance, 1u) == 7);
        assert(tail_impl->jit_functions[1].state ==
               TURBOWASM_JIT_INTERPRET_ONLY);
        assert(tail_impl->jit_functions[1].compiled.impl == NULL);
        assert(tail_impl->jit_functions[0].state ==
               TURBOWASM_JIT_INTERPRET);

        assert(invoke_i32_at(&tail_instance, 1u) == 7);
        assert(tail_impl->jit_functions[1].state ==
               TURBOWASM_JIT_INTERPRET_ONLY);
        assert(tail_impl->jit_functions[1].compiled.impl == NULL);

        turbowasm_instance_destroy(&tail_instance);
        turbowasm_module_destroy(&tail_module);
    }

    return 0;
}
