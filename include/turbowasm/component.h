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
 * Public Component Model façade, with separate synchronous and async admission.
 *
 * Handles are zero-initialized opaque owners. A loaded component borrows
 * immutable source bytes. Those bytes and allocator contexts must remain alive
 * until all derived instances, calls, tasks, transfers and returned values have
 * been released. Owners are unique, zero-initialized, and used on one owner
 * thread; copying a carrier does not retain it. API calls on the same instance
 * must not reenter from its allocator or guest callback.
 * A canon-lift post-return runs after copying/lifting results and before host
 * publication. Failure discards unpublished results. Resumable cleanup shares
 * the call's fuel/interruption budget and is unwound by call destruction.
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

/* Unique endpoint carrier. A future/stream host value owns a readable end;
 * copying the carrier does not retain it. All operations use the owner thread. */
typedef struct turbowasm_component_async_endpoint { void *impl; } turbowasm_component_async_endpoint;

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
    TURBOWASM_COMPONENT_HOST_BORROW = 25,
    TURBOWASM_COMPONENT_HOST_FUTURE = 27,
    TURBOWASM_COMPONENT_HOST_STREAM = 28
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
        turbowasm_component_async_endpoint future;
        turbowasm_component_async_endpoint stream;
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

/* Explicit async admission keeps the original synchronous loader/constructor
 * gate intact. Empty outputs are required; failure leaves them unchanged.
 * Borrowed bytes and allocator context outlive every derived owner. NULL config
 * or options selects defaults. Unsupported binary features remain UNSUPPORTED;
 * unresolved imports return LINK_ERROR. Allocation/quota failure is
 * OUT_OF_MEMORY, invalid ownership/options is INVALID_ARGUMENT. */
typedef struct turbowasm_component_async_task { void *impl; }
    turbowasm_component_async_task;
typedef struct turbowasm_component_async_transfer { void *impl; }
    turbowasm_component_async_transfer;

typedef struct turbowasm_component_async_options {
    uint32_t tasks;
    uint32_t handles;
    uint32_t transfers;
    size_t host_bytes;
} turbowasm_component_async_options;

/* Defaults: 64 tasks, 4096 handles, 64 transfers, 16 MiB logical host storage.
 * Limits are finite/nonzero, copied before callbacks. Charges survive terminal
 * notification and value delivery until actual owner/allocation destruction;
 * non-null empty strings cost one byte. NULL is a no-op. */
void turbowasm_component_async_options_init(
    turbowasm_component_async_options *options);

turbowasm_status turbowasm_component_load_async_borrowed(
    turbowasm_component *component, const uint8_t *bytes, size_t size);
turbowasm_status turbowasm_component_load_async_borrowed_with_config(
    turbowasm_component *component, const uint8_t *bytes, size_t size,
    const turbowasm_runtime_config *config);

turbowasm_status turbowasm_component_instance_create_async_with_options(
    turbowasm_component_instance *instance,
    const turbowasm_component *component,
    const turbowasm_component_async_options *options);

/* Copyable borrowed identity from this live instance. Fields are opaque; a
 * token neither owns nor extends the instance lifetime. Queries validate scope,
 * leave outputs unchanged on failure, and return TYPE_MISMATCH for the wrong
 * type/edge or absent payload, INVALID_ARGUMENT for an out-of-range index,
 * LINK_ERROR for a missing export. FIELD/CASE use index; other edges require 0. */
typedef struct turbowasm_component_type_token {
    const void *scope;
    uint32_t id;
    bool inline_type;
} turbowasm_component_type_token;

typedef enum turbowasm_component_type_edge {
    TURBOWASM_COMPONENT_TYPE_EDGE_ELEMENT,
    TURBOWASM_COMPONENT_TYPE_EDGE_FIELD,
    TURBOWASM_COMPONENT_TYPE_EDGE_CASE,
    TURBOWASM_COMPONENT_TYPE_EDGE_SOME,
    TURBOWASM_COMPONENT_TYPE_EDGE_OK,
    TURBOWASM_COMPONENT_TYPE_EDGE_ERROR,
    TURBOWASM_COMPONENT_TYPE_EDGE_PAYLOAD
} turbowasm_component_type_edge;

turbowasm_status turbowasm_component_instance_parameter_type(
    const turbowasm_component_instance *instance, turbowasm_name export_name,
    size_t parameter_index, turbowasm_component_type_token *out);
turbowasm_status turbowasm_component_instance_result_type(
    const turbowasm_component_instance *instance, turbowasm_name export_name,
    turbowasm_component_type_token *out);
turbowasm_status turbowasm_component_instance_type_child(
    const turbowasm_component_instance *instance,
    turbowasm_component_type_token parent, turbowasm_component_type_edge edge,
    size_t index, turbowasm_component_type_token *out);

typedef struct turbowasm_component_async_endpoint_type {
    bool future;
    bool has_payload;
    turbowasm_component_type_token payload;
} turbowasm_component_async_endpoint_type;

turbowasm_status turbowasm_component_instance_endpoint_type_get(
    const turbowasm_component_instance *instance,
    turbowasm_component_type_token type,
    turbowasm_component_async_endpoint_type *out);

typedef enum turbowasm_component_async_wait_reason {
    TURBOWASM_COMPONENT_ASYNC_WAIT_NONE,
    TURBOWASM_COMPONENT_ASYNC_WAIT_FUEL,
    TURBOWASM_COMPONENT_ASYNC_WAIT_INTERRUPTION,
    TURBOWASM_COMPONENT_ASYNC_WAIT_HOST_IO,
    TURBOWASM_COMPONENT_ASYNC_WAIT_COMPONENT_EVENT,
    TURBOWASM_COMPONENT_ASYNC_WAIT_BACKPRESSURE,
    TURBOWASM_COMPONENT_ASYNC_WAIT_COOPERATIVE
} turbowasm_component_async_wait_reason;

typedef struct turbowasm_component_async_task_state {
    turbowasm_execution_state execution;
    turbowasm_component_async_wait_reason wait_reason;
    bool terminal;
    bool cancellation_requested;
    bool cancelled;
    bool result_taken;
    size_t result_count;
    turbowasm_status status;
    turbowasm_trap trap;
} turbowasm_component_async_task_state;

/* Create an async-typed export task; synchronous exports use existing calls.
 * Ordinary arguments are copied. Const create rejects nested own/endpoints;
 * create_move consumes all own/readable endpoint leaves together after complete
 * validation and staging. Before commit, failure preserves the whole input;
 * after successful create, execution/trap/cancel never restores moved leaves.
 * Caller containers remain caller-owned. Borrow sources stay pinned through
 * terminal execution and must outlive their loan. A task retains its instance. */
turbowasm_status turbowasm_component_async_task_create(
    turbowasm_component_async_task *task,
    turbowasm_component_instance *instance, turbowasm_name export_name,
    const turbowasm_component_host_value *arguments, size_t argument_count);
turbowasm_status turbowasm_component_async_task_create_move(
    turbowasm_component_async_task *task,
    turbowasm_component_instance *instance, turbowasm_name export_name,
    turbowasm_component_host_value *arguments, size_t argument_count);
/* Drive one continuation using Runtime fuel/interruption options. YIELDED means
 * inspect state/wait; OK means successful completion; cancellation acknowledged
 * before return is INTERRUPTED. Execution failures propagate their status/trap.
 * State reads are snapshots and do not advance or acknowledge completion. */
turbowasm_status turbowasm_component_async_task_resume(
    turbowasm_component_async_task *task,
    const turbowasm_execution_options *options);
turbowasm_status turbowasm_component_async_task_state_get(
    const turbowasm_component_async_task *task,
    turbowasm_component_async_task_state *out);
/* Idempotent request; a pending host I/O must still complete and the task must
 * be driven to a terminal state. An already-returned result wins late cancel. */
turbowasm_status turbowasm_component_async_task_request_cancel(
    turbowasm_component_async_task *task);
/* Once-only delivery: out is empty (NULL allowed for no result); count is
 * required and set to 0/1 only on success. Pending returns YIELDED. OOM leaves
 * output/count untouched and can be retried. Returned storage remains charged
 * and alive independently of the task/public instance; destroy it explicitly. */
turbowasm_status turbowasm_component_async_task_take_result(
    turbowasm_component_async_task *task,
    turbowasm_component_host_value *out, size_t *out_result_count);
/* Empty owner succeeds; nonterminal/busy returns INVALID_ARGUMENT unchanged.
 * Terminal destruction consumes the owner and reports its first failure. */
turbowasm_status turbowasm_component_async_task_destroy(
    turbowasm_component_async_task *task);

typedef struct turbowasm_component_async_wait {
    const void *owner;
    uint64_t generation;
    uint64_t continuation;
    turbowasm_host_wait wait;
} turbowasm_component_async_wait;

/* Tickets authenticate owner, admission, continuation and Runtime wait. Query
 * false preserves out. Completion is allocation-free and resumes no guest code;
 * subsequently call resume/poll. Stale/foreign/duplicate tickets are invalid.
 * completion_status is the imported I/O provider's result, interpreted there. */
bool turbowasm_component_async_task_pending_host_wait(
    const turbowasm_component_async_task *task,
    turbowasm_component_async_wait *out);
turbowasm_status turbowasm_component_async_task_complete_host_wait(
    turbowasm_component_async_task *task,
    turbowasm_component_async_wait wait, int completion_status);

typedef struct turbowasm_component_async_endpoint_state {
    bool future;
    bool readable;
    bool has_payload;
    bool peer_dropped;
    bool finished;
} turbowasm_component_async_endpoint_state;

/* Create both ends atomically for an instance-derived future/stream type.
 * Readers/writers are distinct empty owners, each retaining its origin instance.
 * Only idle readable ends can move into/out of a host value; failure preserves
 * both carriers. Endpoint state reports facts without exposing handles/buffers.
 * Empty destroy succeeds; busy destroy returns INVALID_ARGUMENT unchanged. */
turbowasm_status turbowasm_component_async_endpoint_pair_create(
    turbowasm_component_async_endpoint *reader,
    turbowasm_component_async_endpoint *writer,
    turbowasm_component_instance *instance,
    turbowasm_component_type_token type);
turbowasm_status turbowasm_component_async_endpoint_state_get(
    const turbowasm_component_async_endpoint *endpoint,
    turbowasm_component_async_endpoint_state *out);
turbowasm_status turbowasm_component_async_endpoint_into_value(
    turbowasm_component_async_endpoint *reader,
    turbowasm_component_host_value *out);
turbowasm_status turbowasm_component_async_endpoint_from_value(
    turbowasm_component_async_endpoint *reader,
    turbowasm_component_host_value *source);
turbowasm_status turbowasm_component_async_endpoint_destroy(
    turbowasm_component_async_endpoint *endpoint);

typedef enum turbowasm_component_async_transfer_outcome {
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_PENDING,
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_COMPLETED,
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_PEER_DROPPED,
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_CANCELLED,
    TURBOWASM_COMPONENT_ASYNC_TRANSFER_FAILED
} turbowasm_component_async_transfer_outcome;

typedef struct turbowasm_component_async_transfer_state {
    uint32_t length;
    uint32_t progress;
    bool readable;
    bool terminal;
    bool result_taken;
    turbowasm_component_async_transfer_outcome outcome;
    turbowasm_status status;
} turbowasm_component_async_transfer_state;

typedef struct turbowasm_component_async_transfer_result {
    turbowasm_component_async_endpoint endpoint;
    turbowasm_component_host_value values;
    uint32_t first_index;
    uint32_t logical_count;
} turbowasm_component_async_transfer_result;

/* Successful admission consumes the endpoint. Read reserves count cells and
 * payload_bytes for nested canonical payload storage (numeric/unit can use 0).
 * Write copies ordinary values; write_move also consumes all own/readable ends
 * after whole-tree staging. Payload borrow is TYPE_MISMATCH; const owned leaves
 * are INVALID_ARGUMENT. Unit endpoints use count with values == NULL. Future
 * count must be 1, stream count may be 0. Capacity/byte overflow fails explicitly.
 * Precommit failure preserves inputs. A later copy failure belongs to an
 * accepted transfer and is reported by poll/state, with prefix/tail reclaimable. */
turbowasm_status turbowasm_component_async_transfer_read(
    turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_endpoint *reader,
    uint32_t count, size_t payload_bytes);
turbowasm_status turbowasm_component_async_transfer_write(
    turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_endpoint *writer,
    const turbowasm_component_host_value *values, uint32_t count);
turbowasm_status turbowasm_component_async_transfer_write_move(
    turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_endpoint *writer,
    turbowasm_component_host_value *values, uint32_t count);
/* Poll acknowledges the actual copy event once and thereafter returns recorded
 * terminal status. YIELDED retains all owners. Partial progress may complete a
 * stream operation. Cancel is an idempotent request; notified completion wins. */
turbowasm_status turbowasm_component_async_transfer_poll(
    turbowasm_component_async_transfer *transfer);
turbowasm_status turbowasm_component_async_transfer_state_get(
    const turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_transfer_state *out);
turbowasm_status turbowasm_component_async_transfer_request_cancel(
    turbowasm_component_async_transfer *transfer);
/* Once-only atomic result: empty out required, pending returns YIELDED. Read
 * returns [0, progress); write returns [progress, length), including unsent own
 * leaves. Values is an owned outer HOST_LIST batch for payload types, zero for
 * unit; logical_count still counts units. endpoint preserves its original role.
 * OOM leaves transfer and out unchanged for retry, including after copy failure.
 * Result storage retains its instance and charge until actual destruction. */
turbowasm_status turbowasm_component_async_transfer_take_result(
    turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_transfer_result *out);
/* Destroys endpoint + complete value tree together. Busy leaves reject without
 * mutation. Otherwise consumes/clears all fields, returning first cleanup error. */
turbowasm_status turbowasm_component_async_transfer_result_destroy(
    turbowasm_component_async_transfer_result *result);
/* Nonterminal/busy rejects unchanged; empty succeeds. Terminal destruction
 * discards undelivered prefix/tail, consumes the owner, reports first failure. */
turbowasm_status turbowasm_component_async_transfer_destroy(
    turbowasm_component_async_transfer *transfer);

typedef struct turbowasm_component_async_shutdown_state {
    bool requested;
    bool complete;
    turbowasm_component_async_wait_reason wait_reason;
    turbowasm_status status;
} turbowasm_component_async_shutdown_state;

/* Close admission and request cancellation of registered owners. Poll shares
 * Runtime fuel/interruption control with resumable guest destructors. YIELDED
 * requires driving/releasing remaining owners and completing authenticated I/O.
 * External task/transfer/value carriers are never silently destroyed. Terminal
 * status preserves first cleanup failure and repeated polls return it. Once
 * explicitly requested, instance_destroy preserves its carrier until shutdown
 * completes; then it releases it. NULL/invalid/busy requests reject unchanged. */
turbowasm_status turbowasm_component_instance_request_shutdown(
    turbowasm_component_instance *instance);
turbowasm_status turbowasm_component_instance_poll_shutdown(
    turbowasm_component_instance *instance,
    const turbowasm_execution_options *options);
turbowasm_status turbowasm_component_instance_shutdown_state_get(
    const turbowasm_component_instance *instance,
    turbowasm_component_async_shutdown_state *out);
bool turbowasm_component_instance_shutdown_pending_host_wait(
    const turbowasm_component_instance *instance,
    turbowasm_component_async_wait *out);
turbowasm_status turbowasm_component_instance_shutdown_complete_host_wait(
    turbowasm_component_instance *instance,
    turbowasm_component_async_wait wait, int completion_status);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_COMPONENT_H */
