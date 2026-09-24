#ifndef TURBOWASM_JIT_BACKEND_H
#define TURBOWASM_JIT_BACKEND_H

#include <turbowasm/instance.h>
#include <turbowasm/status.h>
#include <turbowasm/value.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Private backend-neutral JIT contract.
 *
 * Public TurboWasm headers must not include this file. Backends such as MIR
 * implement this interface behind the interpreter/runtime boundary.
 */

struct turbowasm_instance_impl;
struct turbowasm_validation_context;
struct turbowasm_validation_function;

/*
 * Execution control is intentionally opaque to the backend contract.
 * Backends receive a stable handle owned by the invocation and must preserve
 * the same fuel/interruption semantics as the interpreter.
 */
typedef struct turbowasm_jit_execution_control
    turbowasm_jit_execution_control;
typedef struct turbowasm_jit_invocation_context
    turbowasm_jit_invocation_context;

typedef struct turbowasm_compiled_function {
    void *impl;
} turbowasm_compiled_function;

typedef turbowasm_status (*turbowasm_compiled_invoke_fn)(
    const turbowasm_compiled_function *compiled,
    turbowasm_jit_invocation_context *context,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap);

typedef struct turbowasm_jit_backend {
    void *context;

    /* Backends opt in only after proving that compiled code consumes the
     * TurboWasm execution-control contract at bounded safe points. */
    bool supports_execution_control;

    bool (*is_function_eligible)(
        void *context,
        const struct turbowasm_validation_context *validation,
        uint32_t function_index,
        const struct turbowasm_validation_function *function);

    turbowasm_status (*compile_function)(
        void *context,
        const struct turbowasm_validation_context *validation,
        uint32_t function_index,
        const struct turbowasm_validation_function *function,
        turbowasm_compiled_function *out);

    turbowasm_compiled_invoke_fn invoke;

    void (*destroy_function)(
        void *context,
        turbowasm_compiled_function *compiled);

    void (*destroy_backend)(void *context);
} turbowasm_jit_backend;

/*
 * Contract rules:
 *
 * - eligibility is per function, never per module;
 * - compile failure must not invalidate the interpreter fallback path;
 * - a backend may reject any validated function it cannot lower safely;
 * - compiled invocation receives one private hidden invocation context
 *   carrying instance/execution/depth state;
 * - compiled invocation must preserve TurboWasm trap/status semantics;
 * - fuel/interruption is owned by TurboWasm, not delegated implicitly to
 *   backend-specific runtime state;
 * - supports_execution_control defaults false; Runtime interprets policy-
 *   controlled calls unless the backend explicitly opts in;
 * - SIMD may call Salts::SIMD helpers instead of native-lowering V128;
 * - backend implementation types must never enter public TurboWasm headers.
 */

#endif /* TURBOWASM_JIT_BACKEND_H */
