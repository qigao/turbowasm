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
 * Handles are zero-initialized opaque owners. A loaded component borrows
 * immutable source bytes. Those bytes and allocator contexts must remain alive
 * until all derived instances, calls and returned own values have been released.
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
    TURBOWASM_COMPONENT_HOST_LIST,
    TURBOWASM_COMPONENT_HOST_RECORD,
    TURBOWASM_COMPONENT_HOST_TUPLE,
    TURBOWASM_COMPONENT_HOST_VARIANT,
    TURBOWASM_COMPONENT_HOST_OPTION,
    TURBOWASM_COMPONENT_HOST_RESULT,
    TURBOWASM_COMPONENT_HOST_ENUM,
    TURBOWASM_COMPONENT_HOST_FLAGS,
    TURBOWASM_COMPONENT_HOST_OWN = 24,
    TURBOWASM_COMPONENT_HOST_BORROW = 25
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

/* Ordered fields/elements use declaration order, not names. */
typedef turbowasm_component_host_list turbowasm_component_host_sequence;

/* Variant case index; option none/some = 0/1; result ok/error = 0/1.
 * Payload is NULL exactly when the selected case has no payload. */
typedef struct turbowasm_component_host_variant {
    uint32_t case_index;
    turbowasm_component_host_value *payload;
} turbowasm_component_host_variant;

/* Little-endian words of flag bits. The synchronous MVP allows 1..32 flags,
 * hence word_count must be 1. Bits beyond the declared flags must be zero. */
typedef struct turbowasm_component_host_flags {
    uint32_t *words;
    size_t word_count;
} turbowasm_component_host_flags;

/* Opaque unique resource owner. Copying own values is not an ownership transfer.
 * Borrow views are non-owning and require their source to be live at admission. */
typedef struct turbowasm_component_host_resource turbowasm_component_host_resource;

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
        turbowasm_component_host_sequence record;
        turbowasm_component_host_sequence tuple;
        turbowasm_component_host_variant variant;
        turbowasm_component_host_variant option;
        turbowasm_component_host_variant result;
        uint32_t enum_index;
        turbowasm_component_host_flags flags;
        turbowasm_component_host_resource *own;
        turbowasm_component_host_resource *borrow;
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
 * Supports synchronous value types, including own results and borrow arguments.
 * Const invocation rejects own arguments, including nested own leaves, before
 * canonical lowering. Use invoke_move to transfer ownership.
 *
 * A synchronous Component MVP function has at most one result. result_capacity
 * may therefore be 0 or 1. out_result_count is always written on successful
 * invocation.
 *
 * Input values are borrowed and never consumed. Result storage must be empty
 * or contain only an unowned scalar before invocation. Returned composite/string
 * storage is owned by TurboWasm and must be released with
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

/* Same invocation contract, with explicit own transfer. Full type/identity
 * validation and canonical preparation precede commit. Pre-commit failure keeps
 * own leaves unchanged; after commit they are zeroed even if execution traps.
 * Non-resource argument storage stays caller-owned. Result storage may alias a
 * top-level own argument: failure before commit preserves it, success replaces
 * the consumed value. A guest realloc failure may leave guest memory changed. */
turbowasm_status turbowasm_component_instance_invoke_move(
    turbowasm_component_instance *instance,
    turbowasm_name export_name,
    turbowasm_component_host_value *arguments,
    size_t argument_count,
    turbowasm_component_host_value *result,
    size_t result_capacity,
    size_t *out_result_count,
    turbowasm_trap *trap);

/*
 * Create a restartable invocation of one Component function export.
 *
 * The call retains the Component instance and its capability owner. Releasing
 * the public instance/component handles does not invalidate an admitted call.
 * The borrowed component source bytes must still outlive the call.
 * Arguments are canonically lowered during create and their carrier storage need
 * not outlive this function. A borrow's source own remains pinned until terminal
 * completion or call destruction. The retained Runtime execution may yield for fuel,
 * interruption, or host-wait without replaying canonical lowering or the
 * imported callback frame.
 */
turbowasm_status turbowasm_component_call_create(
    turbowasm_component_call *call,
    turbowasm_component_instance *instance,
    turbowasm_name export_name,
    const turbowasm_component_host_value *arguments,
    size_t argument_count);

/* Successful creation consumes own leaves exactly once. Destroying an unstarted
 * call releases its transferred resources. Borrow loans last until terminal
 * completion or cancellation, and block moving/destroying the source own. */
turbowasm_status turbowasm_component_call_create_move(
    turbowasm_component_call *call,
    turbowasm_component_instance *instance,
    turbowasm_name export_name,
    turbowasm_component_host_value *arguments,
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

/* Create a borrow view without transferring or duplicating the source own.
 * out_borrow must be empty and distinct from source. Returns INVALID_ARGUMENT
 * for a non-own, moved, busy or invalid source. The source own must remain live
 * until the view is admitted to a call; never admit a stale view after moving or
 * destroying its source. The view itself has no cleanup cost. */
turbowasm_status turbowasm_component_host_value_borrow(
    const turbowasm_component_host_value *source,
    turbowasm_component_host_value *out_borrow);

/*
 * Destroy a value returned by turbowasm_component_instance_invoke().
 * Do not call this on caller-owned input values. Recursively releases storage
 * and clears the value. NULL and already-cleared values succeed.
 * An active loan or busy owner returns INVALID_ARGUMENT without mutation.
 * Otherwise cleanup continues after destructor failures and returns the first
 * error; the cleared value cannot be retried. Instances are single-threaded;
 * retained owners/calls do not permit concurrent execution on an instance.
 */
turbowasm_status turbowasm_component_host_value_destroy(
    turbowasm_component_host_value *value);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_COMPONENT_H */
