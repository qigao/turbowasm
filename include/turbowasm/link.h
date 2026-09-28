#ifndef TURBOWASM_LINK_H
#define TURBOWASM_LINK_H

#include <turbowasm/instance.h>

#ifdef __cplusplus
extern "C" {
#endif

struct turbowasm_instance;

/*
 * Explicit capability scope for module linking.
 *
 * Namespace names are copied into the linker. Provider instances are borrowed;
 * they and their modules must outlive every consumer instance created from the
 * linker. The linker itself may be destroyed after linked instance creation.
 */
typedef struct turbowasm_linker {
    void *impl;
} turbowasm_linker;

/*
 * Synchronous host-function ABI.
 *
 * Parameter/result kinds describe the exact public TurboWasm carrier signature
 * admitted at link time. The callback/context are borrowed by consumer
 * instances and must remain valid while those instances may invoke the host
 * function. The linker itself may be destroyed after instance creation.
 *
 * turbowasm_host_call is an opaque per-invocation Runtime context. This first
 * slice only exposes the caller instance accessor. Future host-wait support can
 * suspend through the same call context without replaying the Wasm import.
 *
 * This first host ABI deliberately exposes no guest-memory pointer and no
 * scheduler/NativeIO handle. WASI capability adapters layer those contracts on
 * top rather than bypassing Runtime validation.
 */
typedef struct turbowasm_host_call {
    void *impl;
} turbowasm_host_call;

/*
 * Generation-checked pending host wait identity. operation_token is copied from
 * the host adapter and remains opaque to TurboWasm; generation is Runtime-owned
 * and prevents stale/duplicate completion from waking a later wait.
 */
typedef struct turbowasm_host_wait {
    uint64_t generation;
    uintptr_t operation_token;
} turbowasm_host_wait;

turbowasm_instance *turbowasm_host_call_instance(
    turbowasm_host_call *call);

/*
 * Suspend the current async-capable host callback after an external operation
 * has been accepted. This only succeeds inside a resumable execution. The same
 * callback frame continues after the matching wait is completed and the
 * execution is resumed. out_status receives the adapter-provided completion
 * status.
 */
turbowasm_status turbowasm_host_call_wait(
    turbowasm_host_call *call,
    uintptr_t operation_token,
    int *out_status);

typedef struct turbowasm_host_function_type {
    const turbowasm_value_kind *params;
    size_t param_count;
    const turbowasm_value_kind *results;
    size_t result_count;
} turbowasm_host_function_type;

typedef turbowasm_status (*turbowasm_host_function_fn)(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap);

turbowasm_status turbowasm_linker_init(turbowasm_linker *linker);
void turbowasm_linker_destroy(turbowasm_linker *linker);

turbowasm_status turbowasm_linker_define_instance(
    turbowasm_linker *linker,
    turbowasm_name module_name,
    struct turbowasm_instance *instance);

turbowasm_status turbowasm_linker_define_host_function(
    turbowasm_linker *linker,
    turbowasm_name module_name,
    turbowasm_name name,
    const turbowasm_host_function_type *type,
    turbowasm_host_function_fn function,
    void *context);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_LINK_H */
