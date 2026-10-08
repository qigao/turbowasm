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

enum {
    TURBOWASM_JIT_ARTIFACT_FINGERPRINT_SIZE = 32
};

typedef struct turbowasm_jit_artifact_key {
    uint8_t source_sha256[TURBOWASM_JIT_ARTIFACT_FINGERPRINT_SIZE];
    uint64_t validation_feature_fingerprint;
    uint32_t function_index;
    uint8_t backend_fingerprint[TURBOWASM_JIT_ARTIFACT_FINGERPRINT_SIZE];
} turbowasm_jit_artifact_key;

typedef struct turbowasm_jit_artifact_view {
    const uint8_t *bytes;
    size_t size;
} turbowasm_jit_artifact_view;

/*
 * Runtime-owned cache routing contract.
 *
 * A lookup hit returns a borrowed view that remains valid until release().
 * max_blob_bytes is mandatory and bounds both restore input and store output.
 * Cache I/O failure is advisory: execution must fall back to ordinary compile
 * or interpreter behavior. store() consumes/copies its input synchronously;
 * the Runtime releases the temporary buffer when store() returns.
 */
typedef struct turbowasm_jit_artifact_cache {
    void *context;
    size_t max_blob_bytes;

    bool (*lookup)(
        void *context,
        const turbowasm_jit_artifact_key *key,
        turbowasm_jit_artifact_view *out);

    void (*release)(
        void *context,
        turbowasm_jit_artifact_view *view);

    turbowasm_status (*store)(
        void *context,
        const turbowasm_jit_artifact_key *key,
        const uint8_t *bytes,
        size_t size);
} turbowasm_jit_artifact_cache;

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

    /* Requires execution control and retention of native frames, rooted value
     * cells and invocation-local scratch across owner-thread coroutine yields.
     * Code and backend storage must outlive every suspended invocation. */
    bool supports_resumable_execution;

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

    /*
     * Optional persistent-artifact surface.
     *
     * artifact_fingerprint identifies the exact backend/compiler/target ABI
     * contract. Backends that cannot safely restore generated code leave these
     * callbacks NULL and retain ordinary lazy compilation.
     *
     * restore_function_artifact must fully validate backend-private bytes and
     * enforce backend executable-memory/resource policy before publishing a
     * compiled handle.
     */
    bool (*artifact_fingerprint)(
        void *context,
        uint8_t out[TURBOWASM_JIT_ARTIFACT_FINGERPRINT_SIZE]);

    turbowasm_status (*restore_function_artifact)(
        void *context,
        const struct turbowasm_validation_context *validation,
        uint32_t function_index,
        const struct turbowasm_validation_function *function,
        const uint8_t *bytes,
        size_t size,
        turbowasm_compiled_function *out);

    turbowasm_status (*measure_function_artifact)(
        void *context,
        const turbowasm_compiled_function *compiled,
        size_t *out_size);

    turbowasm_status (*write_function_artifact)(
        void *context,
        const turbowasm_compiled_function *compiled,
        uint8_t *output,
        size_t capacity,
        size_t *out_size);

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
 * - supports_resumable_execution separately admits suspend/resume callbacks;
 *   backends without it keep interpreter execution for resumable calls;
 * - SIMD may call Salts::SIMD helpers instead of native-lowering V128;
 * - backend implementation types must never enter public TurboWasm headers;
 * - persistent blobs are optional and backend-private;
 * - a cached blob never substitutes for Wasm validation identity;
 * - restore failure/corruption must fall back without changing Wasm semantics;
 * - backend restore must enforce executable-memory/resource limits before
 *   publishing code.
 */

#endif /* TURBOWASM_JIT_BACKEND_H */
