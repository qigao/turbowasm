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

turbowasm_instance *turbowasm_host_call_instance(
    turbowasm_host_call *call);

/*
 * Copy through the canonical guest-memory bounds path for the current host
 * invocation. These APIs never return a long-lived guest pointer.
 */
turbowasm_status turbowasm_host_call_memory_read(
    turbowasm_host_call *call,
    uint32_t memory_index,
    uint64_t address,
    void *destination,
    size_t length);

turbowasm_status turbowasm_host_call_memory_write(
    turbowasm_host_call *call,
    uint32_t memory_index,
    uint64_t address,
    const void *source,
    size_t length);

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
