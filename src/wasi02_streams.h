#ifndef TURBOWASM_WASI02_STREAMS_H
#define TURBOWASM_WASI02_STREAMS_H

#include "component_resource.h"
#include "wasi02_provider.h"
#include "wasi02_poll.h"

#include <turbowasm/status.h>
#include <turbowasm/value.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum turbowasm_wasi02_stream_error_kind {
    TURBOWASM_WASI02_STREAM_ERROR_NONE = 0,
    TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED,
    TURBOWASM_WASI02_STREAM_ERROR_CLOSED
} turbowasm_wasi02_stream_error_kind;

typedef struct turbowasm_wasi02_stream_error {
    turbowasm_wasi02_stream_error_kind kind;
    turbowasm_value error_rep;
} turbowasm_wasi02_stream_error;

typedef turbowasm_status (*turbowasm_wasi02_input_read_fn)(
    void *context,
    turbowasm_value stream_rep,
    uint64_t max_bytes,
    const uint8_t **out_data,
    size_t *out_size,
    turbowasm_wasi02_stream_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_input_skip_fn)(
    void *context,
    turbowasm_value stream_rep,
    uint64_t max_bytes,
    uint64_t *out_skipped,
    turbowasm_wasi02_stream_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_output_check_write_fn)(
    void *context,
    turbowasm_value stream_rep,
    uint64_t *out_permit,
    turbowasm_wasi02_stream_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_output_write_fn)(
    void *context,
    turbowasm_value stream_rep,
    const uint8_t *data,
    size_t size,
    turbowasm_wasi02_stream_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_output_flush_fn)(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_wasi02_stream_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_output_write_zeroes_fn)(
    void *context,
    turbowasm_value stream_rep,
    uint64_t size,
    turbowasm_wasi02_stream_error *out_error);

typedef turbowasm_status (*turbowasm_wasi02_stream_subscribe_fn)(
    void *context,
    turbowasm_value stream_rep,
    turbowasm_value *out_pollable_rep);

typedef turbowasm_status (*turbowasm_wasi02_error_debug_fn)(
    void *context,
    turbowasm_value error_rep,
    turbowasm_wasi02_string_view *out_debug);

typedef void (*turbowasm_wasi02_stream_drop_fn)(
    void *context,
    turbowasm_value rep);

typedef struct turbowasm_wasi02_stream_provider {
    void *context;

    turbowasm_wasi02_input_read_fn input_read;
    turbowasm_wasi02_input_skip_fn input_skip;
    turbowasm_wasi02_stream_subscribe_fn input_subscribe;

    turbowasm_wasi02_output_check_write_fn output_check_write;
    turbowasm_wasi02_output_write_fn output_write;
    turbowasm_wasi02_output_flush_fn output_flush;
    turbowasm_wasi02_output_write_zeroes_fn output_write_zeroes;
    turbowasm_wasi02_stream_subscribe_fn output_subscribe;

    turbowasm_wasi02_error_debug_fn error_debug;

    turbowasm_wasi02_stream_drop_fn input_drop;
    turbowasm_wasi02_stream_drop_fn output_drop;
    turbowasm_wasi02_stream_drop_fn error_drop;
} turbowasm_wasi02_stream_provider;

typedef enum turbowasm_wasi02_stream_slot_kind {
    TURBOWASM_WASI02_STREAM_SLOT_NONE = 0,
    TURBOWASM_WASI02_STREAM_SLOT_INPUT,
    TURBOWASM_WASI02_STREAM_SLOT_OUTPUT,
    TURBOWASM_WASI02_STREAM_SLOT_ERROR
} turbowasm_wasi02_stream_slot_kind;

typedef struct turbowasm_wasi02_stream_slot {
    bool active;
    turbowasm_wasi02_stream_slot_kind kind;
    turbowasm_value provider_rep;

    bool write_permit_valid;
    uint64_t write_permit;

    bool transient_pollable_valid;
    uint32_t transient_pollable;
} turbowasm_wasi02_stream_slot;

typedef struct turbowasm_wasi02_streams {
    turbowasm_wasi02_stream_provider provider;

    turbowasm_component_resource_table resources;
    turbowasm_wasi02_stream_slot *slots;
    uint32_t *free_indices;
    uint32_t capacity;
    uint32_t free_count;

    turbowasm_wasi02_poll *poll;
    bool initialized;
} turbowasm_wasi02_streams;

turbowasm_status turbowasm_wasi02_streams_init(
    turbowasm_wasi02_streams *streams,
    const turbowasm_wasi02_stream_provider *provider,
    uint32_t max_resources);

turbowasm_status turbowasm_wasi02_streams_destroy(
    turbowasm_wasi02_streams *streams);

turbowasm_status turbowasm_wasi02_streams_attach_poll(
    turbowasm_wasi02_streams *streams,
    turbowasm_wasi02_poll *poll);

turbowasm_status turbowasm_wasi02_input_stream_new(
    turbowasm_wasi02_streams *streams,
    turbowasm_value provider_rep,
    uint32_t *out_resource);

turbowasm_status turbowasm_wasi02_output_stream_new(
    turbowasm_wasi02_streams *streams,
    turbowasm_value provider_rep,
    uint32_t *out_resource);

/*
 * Explicit direct-drop qualification entrypoint. Real Component resource drop
 * is wired in the execution slice.
 */
turbowasm_status turbowasm_wasi02_stream_resource_drop(
    turbowasm_wasi02_streams *streams,
    uint32_t resource);

/*
 * Execute the W4c2a nonblocking subset:
 *   input read/skip
 *   output check-write/write/flush/write-zeroes
 *   error.to-debug-string
 *
 * subscribe is available when a poll bridge is attached and the provider
 * supplies the corresponding subscribe callback.
 *
 * blocking methods and splice remain unsupported here.
 */
turbowasm_status turbowasm_wasi02_streams_call_with_host(
    turbowasm_wasi02_streams *streams,
    turbowasm_host_call *call,
    const char *interface_name,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result);

turbowasm_status turbowasm_wasi02_streams_call(
    turbowasm_wasi02_streams *streams,
    const char *interface_name,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result);

#endif /* TURBOWASM_WASI02_STREAMS_H */
