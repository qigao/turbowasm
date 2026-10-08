#ifndef TURBOWASM_COMPONENT_ASYNC_API_H
#define TURBOWASM_COMPONENT_ASYNC_API_H

/* Private staging of the approved public interface. Install only after the
 * value, transfer and lifecycle adapters are complete and qualified. */
#include <turbowasm/component.h>

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

turbowasm_status turbowasm_component_async_task_create(
    turbowasm_component_async_task *task,
    turbowasm_component_instance *instance, turbowasm_name export_name,
    const turbowasm_component_host_value *arguments, size_t argument_count);
turbowasm_status turbowasm_component_async_task_create_move(
    turbowasm_component_async_task *task,
    turbowasm_component_instance *instance, turbowasm_name export_name,
    turbowasm_component_host_value *arguments, size_t argument_count);
turbowasm_status turbowasm_component_async_task_resume(
    turbowasm_component_async_task *task,
    const turbowasm_execution_options *options);
turbowasm_status turbowasm_component_async_task_state_get(
    const turbowasm_component_async_task *task,
    turbowasm_component_async_task_state *out);
turbowasm_status turbowasm_component_async_task_request_cancel(
    turbowasm_component_async_task *task);
turbowasm_status turbowasm_component_async_task_take_result(
    turbowasm_component_async_task *task,
    turbowasm_component_host_value *out, size_t *out_result_count);
turbowasm_status turbowasm_component_async_task_destroy(
    turbowasm_component_async_task *task);

typedef struct turbowasm_component_async_wait {
    const void *owner;
    uint64_t generation;
    uint64_t continuation;
    turbowasm_host_wait wait;
} turbowasm_component_async_wait;

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
turbowasm_status turbowasm_component_async_transfer_poll(
    turbowasm_component_async_transfer *transfer);
turbowasm_status turbowasm_component_async_transfer_state_get(
    const turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_transfer_state *out);
turbowasm_status turbowasm_component_async_transfer_request_cancel(
    turbowasm_component_async_transfer *transfer);
turbowasm_status turbowasm_component_async_transfer_take_result(
    turbowasm_component_async_transfer *transfer,
    turbowasm_component_async_transfer_result *out);
turbowasm_status turbowasm_component_async_transfer_result_destroy(
    turbowasm_component_async_transfer_result *result);
turbowasm_status turbowasm_component_async_transfer_destroy(
    turbowasm_component_async_transfer *transfer);

typedef struct turbowasm_component_async_shutdown_state {
    bool requested;
    bool complete;
    turbowasm_component_async_wait_reason wait_reason;
    turbowasm_status status;
} turbowasm_component_async_shutdown_state;

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

#endif
