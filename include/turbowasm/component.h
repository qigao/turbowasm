#ifndef TURBOWASM_COMPONENT_H
#define TURBOWASM_COMPONENT_H

#include <turbowasm/execution.h>
#include <turbowasm/instance.h>
#include <turbowasm/module.h>
#include <turbowasm/runtime.h>
#include <turbowasm/status.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Public synchronous Component Model façade.
 *
 * Both handles are zero-initialized opaque owners. A loaded component borrows
 * immutable source bytes. Those bytes must remain alive until the component
 * handle and every instance derived from it have been destroyed.
 */
typedef struct turbowasm_component {
    void *impl;
} turbowasm_component;

typedef struct turbowasm_component_instance {
    void *impl;
} turbowasm_component_instance;

typedef struct turbowasm_component_call {
    void *impl;
} turbowasm_component_call;

typedef enum turbowasm_component_host_value_kind {
    TURBOWASM_COMPONENT_HOST_BOOL = 1,
    TURBOWASM_COMPONENT_HOST_S8,
    TURBOWASM_COMPONENT_HOST_U8,
    TURBOWASM_COMPONENT_HOST_S16,
    TURBOWASM_COMPONENT_HOST_U16,
    TURBOWASM_COMPONENT_HOST_S32,
    TURBOWASM_COMPONENT_HOST_U32,
    TURBOWASM_COMPONENT_HOST_S64,
    TURBOWASM_COMPONENT_HOST_U64,
    TURBOWASM_COMPONENT_HOST_F32,
    TURBOWASM_COMPONENT_HOST_F64,
    TURBOWASM_COMPONENT_HOST_CHAR,
    TURBOWASM_COMPONENT_HOST_STRING,
    TURBOWASM_COMPONENT_HOST_LIST
} turbowasm_component_host_value_kind;

typedef struct turbowasm_component_host_value
    turbowasm_component_host_value;

typedef struct turbowasm_component_host_bytes {
    uint8_t *data;
    size_t size;
} turbowasm_component_host_bytes;

typedef struct turbowasm_component_host_list {
    turbowasm_component_host_value *items;
    size_t count;
} turbowasm_component_host_list;

struct turbowasm_component_host_value {
    turbowasm_component_host_value_kind kind;
    union {
        bool boolean;
        int8_t s8;
        uint8_t u8;
        int16_t s16;
        uint16_t u16;
        int32_t s32;
        uint32_t u32;
        int64_t s64;
        uint64_t u64;
        float f32;
        double f64;
        uint32_t character;
        turbowasm_component_host_bytes string;
        turbowasm_component_host_list list;
    } as;
};

turbowasm_status turbowasm_component_load_borrowed(
    turbowasm_component *component,
    const uint8_t *bytes,
    size_t size);

turbowasm_status turbowasm_component_load_borrowed_with_config(
    turbowasm_component *component,
    const uint8_t *bytes,
    size_t size,
    const turbowasm_runtime_config *config);

/*
 * Releasing the public component handle does not invalidate already-created
 * component instances: instances retain the private decoded component state.
 * The borrowed source bytes, however, must still outlive those instances.
 */
void turbowasm_component_destroy(
    turbowasm_component *component);

turbowasm_status turbowasm_component_instance_create(
    turbowasm_component_instance *instance,
    const turbowasm_component *component);

void turbowasm_component_instance_destroy(
    turbowasm_component_instance *instance);

/*
 * Invoke one synchronous Component function export.
 *
 * The currently installed façade supports the stable scalar/string/list host
 * value subset. Resource own/borrow values remain private until their public
 * ownership contract is frozen.
 *
 * A synchronous Component MVP function has at most one result. result_capacity
 * may therefore be 0 or 1. out_result_count is always written on successful
 * invocation.
 *
 * Input values are borrowed and never consumed. Returned string/list storage is
 * owned by TurboWasm and must be released with
 * turbowasm_component_host_value_destroy().
 */
turbowasm_status turbowasm_component_instance_invoke(
    turbowasm_component_instance *instance,
    turbowasm_name export_name,
    const turbowasm_component_host_value *arguments,
    size_t argument_count,
    turbowasm_component_host_value *result,
    size_t result_capacity,
    size_t *out_result_count,
    turbowasm_trap *trap);

/*
 * Create a restartable invocation of one Component function export.
 *
 * The call borrows the Component instance, which must outlive the call.
 * Arguments are canonically lowered during create and need not outlive this
 * function. The retained Runtime execution may then yield for fuel,
 * interruption, or host-wait without replaying canonical lowering or the
 * imported callback frame.
 */
turbowasm_status turbowasm_component_call_create(
    turbowasm_component_call *call,
    turbowasm_component_instance *instance,
    turbowasm_name export_name,
    const turbowasm_component_host_value *arguments,
    size_t argument_count);

void turbowasm_component_call_destroy(
    turbowasm_component_call *call);

turbowasm_status turbowasm_component_call_resume(
    turbowasm_component_call *call,
    const turbowasm_execution_options *options);

turbowasm_execution_state turbowasm_component_call_state_get(
    const turbowasm_component_call *call);

turbowasm_yield_reason turbowasm_component_call_yield_reason_get(
    const turbowasm_component_call *call);

bool turbowasm_component_call_pending_host_wait(
    const turbowasm_component_call *call,
    turbowasm_host_wait *out_wait);

turbowasm_status turbowasm_component_call_complete_host_wait(
    turbowasm_component_call *call,
    turbowasm_host_wait wait,
    int status);

turbowasm_status turbowasm_component_call_terminal_status(
    const turbowasm_component_call *call);

turbowasm_trap turbowasm_component_call_trap(
    const turbowasm_component_call *call);

size_t turbowasm_component_call_result_count(
    const turbowasm_component_call *call);

/*
 * Move the completed Component result to caller-owned public storage exactly
 * once. Calls with no result report result_count == 0 and reject take_result.
 */
turbowasm_status turbowasm_component_call_take_result(
    turbowasm_component_call *call,
    turbowasm_component_host_value *out_result);

/*
 * Destroy a value returned by turbowasm_component_instance_invoke().
 * Do not call this on caller-owned input values.
 */
void turbowasm_component_host_value_destroy(
    turbowasm_component_host_value *value);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_COMPONENT_H */
