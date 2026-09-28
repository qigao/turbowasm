#include "../src/instance_internal.h"
#include "../src/jit/mir_backend.h"

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

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value result = {0};
    result.kind = TURBOWASM_VALUE_I32;
    result.as.i32 = value;
    return result;
}

int main(void) {
    static const uint8_t bytes[] = {
        WASM_HEADER,

        /* type0: (i32) -> i32 */
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,

        0x03, 0x02,
        0x01, 0x00,

        0x0a, 0x19,
        0x01,
        0x17,
        /* one non-parameter i32 local */
        0x01, 0x01, 0x7f,

        /*
         * if (n) {
         *   local1 = 9;
         *   return_call self(n - 1);
         * } else {
         *   return local1;
         * }
         *
         * Correct frame replacement re-zeroes local1 on every logical
         * invocation, so the base case returns 0 rather than 9.
         */
        0x20, 0x00,
        0x04, 0x7f,
          0x41, 0x09,
          0x21, 0x01,
          0x20, 0x00,
          0x41, 0x01,
          0x6b,
          0x12, 0x00,
        0x05,
          0x20, 0x01,
        0x0b,
        0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_instance_impl *impl;
    turbowasm_jit_backend backend = {0};
    turbowasm_value argument = i32_value(4096);
    turbowasm_value result = {0};
    turbowasm_execution_options options = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module, bytes, sizeof(bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &instance, &module) == TURBOWASM_OK);

    impl = (turbowasm_instance_impl *)instance.impl;
    assert(impl != NULL);

    assert(turbowasm_mir_backend_create(&backend) == TURBOWASM_OK);
    assert(backend.supports_execution_control);
    assert(turbowasm_jit_instance_attach_backend(
               impl, &backend, 1u) == TURBOWASM_OK);

    /*
     * The first call reaches the hot threshold and must compile.  4096
     * self-tail iterations are intentionally well above the interpreter's
     * ordinary 256 nested-call guard.
     */
    assert(turbowasm_instance_invoke(
               &instance, 0u,
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    assert(result.as.i32 == 0);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(impl->jit_functions[0].state == TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[0].compiled.impl != NULL);

    /*
     * The native loop keeps the same opcode checkpoint contract as the
     * interpreter.  A bounded invocation must stop without deoptimizing.
     */
    options.has_fuel_limit = true;
    options.fuel = 32u;
    result = (turbowasm_value){0};
    result_count = 0u;
    trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke_with_options(
               &instance, 0u,
               &argument, 1u,
               &result, 1u,
               &result_count,
               &trap,
               &options) == TURBOWASM_FUEL_EXHAUSTED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(impl->jit_functions[0].state == TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[0].compiled.impl != NULL);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    return 0;
}
