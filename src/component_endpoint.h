#ifndef TURBOWASM_COMPONENT_ENDPOINT_H
#define TURBOWASM_COMPONENT_ENDPOINT_H

#include "component_buffer.h"
#include "component_waitable.h"

typedef struct turbowasm_component_endpoint_codec turbowasm_component_endpoint_codec;

typedef struct turbowasm_component_endpoint {
    turbowasm_component_waitable waitable;
    struct turbowasm_component_endpoint *peer;
    const turbowasm_component_type_graph *graph;
    turbowasm_component_type_id type;
    turbowasm_component_type_ref payload;
    turbowasm_component_buffer *available;
    turbowasm_component_buffer *operation;
    turbowasm_component_buffer guest_buffer;
    turbowasm_status failure;
    bool has_payload;
    bool readable;
    bool initialized;
    bool closed;
    turbowasm_component_endpoint_value_owner *value_owner;
    turbowasm_component_endpoint_codec *lower_scope;
    struct turbowasm_component_endpoint *lower_next;
    turbowasm_component_resource_handle lower_handle;
    /* Stable pair owner notification after close/forward commits. It may release
     * the creation instance and invalidate both closed ends. No endpoint access
     * follows notification; caller-owned pairs leave this NULL. */
    void (*closed_notify)(void *context);
    void *closed_context;
} turbowasm_component_endpoint;

/* Private pair owners are zero-initialized, caller-owned and stable until both
 * are closed and no peer references remain. The graph outlives both. NULL tables
 * represent host ownership. Registration failure leaves both endpoints empty;
 * any earlier handle is removed. This API does not enable guest async decoding. */
turbowasm_status turbowasm_component_endpoint_pair_open(
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    turbowasm_component_resource_table *reader_table,
    turbowasm_component_resource_table *writer_table,
    turbowasm_component_endpoint *reader, turbowasm_component_endpoint *writer);

/* Submit a read/write according to the endpoint direction. OK admits a pending
 * operation; use take or the registered waitable set to obtain its event. The
 * entire range is checked before admission. Guest copies may allocate, convert
 * values and call guest realloc. Completion may be partial; an older operation
 * may accumulate progress before delivery. OK means admitted, including a copy
 * trap subsequently returned by take/poll on both ends. Those error deliveries
 * release buffer borrows; the failed pair can close but cannot copy or move. */
turbowasm_status turbowasm_component_endpoint_submit(
    turbowasm_component_endpoint *endpoint, turbowasm_component_buffer *buffer);
/* Task-driven host submission; driver is borrowed only during this call. */
turbowasm_status turbowasm_component_endpoint_submit_from_task(
    turbowasm_component_endpoint *endpoint, turbowasm_component_buffer *buffer,
    struct turbowasm_component_task *driver);
/* Own the descriptor until event delivery; guest memory stays borrowed. A
 * supplied guest.release transfers context cleanup on successful admission. A sync
 * submission pins before conversion; the caller must wait_end/cancel its pin.
 * Failure before admission preserves the endpoint and consumes no buffer. */
turbowasm_status turbowasm_component_endpoint_submit_guest(
    turbowasm_component_endpoint *endpoint, const turbowasm_component_buffer *buffer, bool synchronous,
    struct turbowasm_component_task *driver);
/* Validate the registered object's provenance before interpreting it as an end. */
turbowasm_component_endpoint *turbowasm_component_endpoint_get(
    turbowasm_component_resource_table *table, turbowasm_component_resource_handle handle,
    turbowasm_component_handle_kind kind);
turbowasm_status turbowasm_component_endpoint_take(
    turbowasm_component_endpoint *endpoint, turbowasm_component_event *out_event);
/* Internal owner transitions for readable ends. Only IDLE, unjoined ends with
 * no operation/waiter may move. Detach removes the old handle; attach publishes
 * the new owner only after successful slot allocation. Peer and pending events
 * are preserved. The canonical boundary must check expected payload type first.
 * Caller-owned endpoint storage and its original graph remain alive throughout. */
turbowasm_status turbowasm_component_endpoint_detach_readable(
    turbowasm_component_endpoint *endpoint);
turbowasm_status turbowasm_component_endpoint_attach_readable(
    turbowasm_component_endpoint *endpoint,
    turbowasm_component_resource_table *table);
/* Move a host-owned readable end into an empty canonical value, freezing direct
 * endpoint operations. Its peer remains usable. Taking the value clears it and
 * restores direct ownership; destroying it closes the end. Neither transition
 * frees caller-owned stable endpoint storage. Entering allocates a Runtime owner
 * record, taking frees it. Graph/storage must outlive the value. Failures preserve
 * the value and endpoint ownership; values reserved for lower cannot be taken. */
turbowasm_status turbowasm_component_endpoint_into_value(
    turbowasm_component_endpoint *endpoint, turbowasm_component_value *out);
turbowasm_status turbowasm_component_endpoint_take_value(
    turbowasm_component_value *value, turbowasm_component_endpoint **out);
/* Host value-array copies acknowledge cancellation immediately, retaining the
 * borrow until event delivery. This does not cancel an external I/O request. */
turbowasm_status turbowasm_component_endpoint_cancel(turbowasm_component_endpoint *endpoint);
/* Successful close can invalidate closed domain-owned endpoints when it returns
 * the final creation-instance keepalive. Retain the instance for further access. */
turbowasm_status turbowasm_component_endpoint_close(turbowasm_component_endpoint *endpoint);

/* Consume idle, unjoined source-readable and destination-writable ends of the
 * same type, joining their peers and copying any available buffers. Admission
 * failure preserves ownership. A conversion failure after linking consumes the
 * intermediate ends, returns the trap, and requires error delivery on both
 * surviving operations before releasing their borrowed storage. */
turbowasm_status turbowasm_component_endpoint_forward(
    turbowasm_component_endpoint *source, turbowasm_component_endpoint *destination);
/* The forwarding task drives any immediate copy between surviving peers. */
turbowasm_status turbowasm_component_endpoint_forward_from_task(
    turbowasm_component_endpoint *source, turbowasm_component_endpoint *destination,
    struct turbowasm_component_task *driver);

/* Zero-initialize then set table to the destination/source canonical table.
 * Source values, table and codec remain alive and exclusively borrowed until
 * commit/rollback. Handles reserved by lower are unusable until commit. Finish
 * before publishing guest output or destroying values; partial guest writes do
 * not roll back. The table quota bounds the intrusive reservation chain. */
struct turbowasm_component_endpoint_codec {
    turbowasm_component_resource_table *table;
    turbowasm_component_endpoint *lower_head;
};

/* Signatures match the private canonical memory endpoint callbacks. Graphs must
 * be validated and immutable. Failed lift admission preserves its handle/output;
 * failed lower admission preserves its value/output and earlier reservations. */
turbowasm_status turbowasm_component_endpoint_codec_lift(void *context,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref type,
    uint32_t handle, turbowasm_component_value *out);
turbowasm_status turbowasm_component_endpoint_codec_lower(void *context,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_ref type,
    const turbowasm_component_value *value, uint32_t *out_handle);
turbowasm_status turbowasm_component_endpoint_codec_commit(turbowasm_component_endpoint_codec *codec);
turbowasm_status turbowasm_component_endpoint_codec_rollback(turbowasm_component_endpoint_codec *codec);

#endif
