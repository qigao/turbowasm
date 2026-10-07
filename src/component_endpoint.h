#ifndef TURBOWASM_COMPONENT_ENDPOINT_H
#define TURBOWASM_COMPONENT_ENDPOINT_H

#include "component_canonical.h"
#include "component_waitable.h"

/* Transfer-owned host storage, exclusively borrowed from submit through event
 * delivery. Source values are unique owned canonical values; destinations start
 * empty. Successful moves clear source cells. Untransferred cells remain owned
 * by the caller, including cancellation and peer closure. Unit buffers may use
 * NULL values regardless of length. Do not mutate a leased buffer or its cells. */
typedef struct turbowasm_component_host_buffer {
    turbowasm_component_value *values;
    uint32_t length;
    uint32_t progress;
    bool leased;
} turbowasm_component_host_buffer;

typedef struct turbowasm_component_endpoint {
    turbowasm_component_waitable waitable;
    struct turbowasm_component_endpoint *peer;
    const turbowasm_component_type_graph *graph;
    turbowasm_component_type_ref payload;
    turbowasm_component_host_buffer *available;
    turbowasm_component_host_buffer *operation;
    bool has_payload;
    bool readable;
    bool initialized;
    bool closed;
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
 * entire host buffer is checked before admission. No allocation or guest calls
 * occur. Completion may be partial; an older operation may accumulate further
 * progress until its pending event is consumed. */
turbowasm_status turbowasm_component_endpoint_submit(
    turbowasm_component_endpoint *endpoint, turbowasm_component_host_buffer *buffer);
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
/* Host value-array copies acknowledge cancellation immediately, retaining the
 * borrow until event delivery. This does not cancel an external I/O request. */
turbowasm_status turbowasm_component_endpoint_cancel(turbowasm_component_endpoint *endpoint);
turbowasm_status turbowasm_component_endpoint_close(turbowasm_component_endpoint *endpoint);

#endif
