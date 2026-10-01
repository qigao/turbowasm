#ifndef TURBOWASM_WASI02_H
#define TURBOWASM_WASI02_H

#include <turbowasm/component.h>
#include <turbowasm/link.h>
#include <turbowasm/runtime.h>
#include <turbowasm/status.h>
#include <turbowasm/value.h>
#include <turbowasm/wasi_fs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Installed WASI 0.2 Component capability façade.
 *
 * The handle owns only TurboWasm adapter state. All callback contexts,
 * filesystem providers and provider resource identities are caller-owned and
 * borrowed. No native fd/HANDLE/socket identity enters this ABI.
 */
typedef struct turbowasm_wasi02 {
    void *impl;
} turbowasm_wasi02;

typedef struct turbowasm_wasi02_string_view {
    const uint8_t *data;
    size_t size;
} turbowasm_wasi02_string_view;

typedef struct turbowasm_wasi02_environment_entry_view {
    turbowasm_wasi02_string_view name;
    turbowasm_wasi02_string_view value;
} turbowasm_wasi02_environment_entry_view;

typedef turbowasm_status (*turbowasm_wasi02_wall_clock_fn)(
    void *context,
    uint64_t *out_seconds,
    uint32_t *out_nanoseconds);

typedef turbowasm_status (*turbowasm_wasi02_monotonic_clock_fn)(
    void *context,
    uint64_t *out_value);

typedef turbowasm_status (*turbowasm_wasi02_random_bytes_fn)(
    void *context,
    uint8_t *bytes,
    size_t size);

typedef turbowasm_status (*turbowasm_wasi02_random_u64_fn)(
    void *context,
    uint64_t *out_value);

typedef turbowasm_status (*turbowasm_wasi02_insecure_seed_fn)(
    void *context,
    uint64_t *out_first,
    uint64_t *out_second);

typedef turbowasm_status (*turbowasm_wasi02_environment_fn)(
    void *context,
    const turbowasm_wasi02_environment_entry_view **out_entries,
    size_t *out_count);

typedef turbowasm_status (*turbowasm_wasi02_arguments_fn)(
    void *context,
    const turbowasm_wasi02_string_view **out_arguments,
    size_t *out_count);

typedef turbowasm_status (*turbowasm_wasi02_initial_cwd_fn)(
    void *context,
    bool *out_has_value,
    turbowasm_wasi02_string_view *out_value);

typedef turbowasm_status (*turbowasm_wasi02_exit_fn)(
    void *context,
    bool success);

typedef struct turbowasm_wasi02_provider_config {
    void *context;

    turbowasm_wasi02_wall_clock_fn wall_clock_now;
    turbowasm_wasi02_wall_clock_fn wall_clock_resolution;
    turbowasm_wasi02_monotonic_clock_fn monotonic_clock_now;
    turbowasm_wasi02_monotonic_clock_fn monotonic_clock_resolution;

    turbowasm_wasi02_random_bytes_fn random_bytes;
    turbowasm_wasi02_random_u64_fn random_u64;
    turbowasm_wasi02_random_bytes_fn insecure_random_bytes;
    turbowasm_wasi02_random_u64_fn insecure_random_u64;
    turbowasm_wasi02_insecure_seed_fn insecure_seed;

    turbowasm_wasi02_environment_fn environment;
    turbowasm_wasi02_arguments_fn arguments;
    turbowasm_wasi02_initial_cwd_fn initial_cwd;
    turbowasm_wasi02_exit_fn exit;
} turbowasm_wasi02_provider_config;

typedef turbowasm_status (*turbowasm_wasi02_poll_ready_fn)(
    void *context,
    turbowasm_value rep,
    bool *out_ready);

typedef turbowasm_status (*turbowasm_wasi02_poll_arm_fn)(
    void *context,
    turbowasm_value rep,
    uintptr_t *out_operation_token);

typedef turbowasm_status (*turbowasm_wasi02_poll_arm_routed_fn)(
    void *context,
    turbowasm_value rep,
    turbowasm_host_call *call,
    uintptr_t *out_operation_token,
    turbowasm_host_wait **out_wait_storage);

typedef turbowasm_status (*turbowasm_wasi02_poll_arm_many_fn)(
    void *context,
    const turbowasm_value *reps,
    size_t rep_count,
    uintptr_t *out_operation_token);

typedef turbowasm_status (*turbowasm_wasi02_poll_arm_many_routed_fn)(
    void *context,
    const turbowasm_value *reps,
    size_t rep_count,
    turbowasm_host_call *call,
    uintptr_t *out_operation_token,
    turbowasm_host_wait **out_wait_storage);

typedef turbowasm_status (*turbowasm_wasi02_poll_drop_fn)(
    void *context,
    turbowasm_value rep);

typedef struct turbowasm_wasi02_poll_provider {
    void *context;
    turbowasm_wasi02_poll_ready_fn ready;
    turbowasm_wasi02_poll_arm_fn arm;
    turbowasm_wasi02_poll_arm_routed_fn arm_routed;
    turbowasm_wasi02_poll_arm_many_fn arm_many;
    turbowasm_wasi02_poll_arm_many_routed_fn arm_many_routed;
    turbowasm_wasi02_poll_drop_fn drop;
} turbowasm_wasi02_poll_provider;

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

/*
 * Produce one caller-owned opaque stream representation. On success ownership
 * transfers to TurboWasm's generation-safe stream resource table and is later
 * released through input_drop/output_drop.
 */
typedef turbowasm_status (*turbowasm_wasi02_stream_factory_fn)(
    void *context,
    turbowasm_value *out_stream_rep);

typedef turbowasm_status (*turbowasm_wasi02_error_debug_fn)(
    void *context,
    turbowasm_value error_rep,
    turbowasm_wasi02_string_view *out_debug);

typedef void (*turbowasm_wasi02_stream_drop_fn)(
    void *context,
    turbowasm_value rep);

typedef struct turbowasm_wasi02_stream_provider {
    void *context;

    /* wasi:cli/stdin, stdout and stderr stream producers. */
    turbowasm_wasi02_stream_factory_fn get_stdin;
    turbowasm_wasi02_stream_factory_fn get_stdout;
    turbowasm_wasi02_stream_factory_fn get_stderr;

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

/*
 * Zero capacities disable the optional filesystem/poll/streams slices.
 *
 * filesystem_resource_capacity requires filesystem != NULL.
 * stream_resource_capacity requires pollable_capacity != 0 and one shared poll
 * provider; streams and pollables therefore use one ownership domain.
 */
typedef struct turbowasm_wasi02_config {
    turbowasm_wasi02_provider_config provider;

    turbowasm_wasi_fs *filesystem;
    uint32_t filesystem_resource_capacity;

    turbowasm_wasi02_poll_provider poll;
    uint32_t pollable_capacity;

    turbowasm_wasi02_stream_provider streams;
    uint32_t stream_resource_capacity;
} turbowasm_wasi02_config;

turbowasm_status turbowasm_wasi02_init(
    turbowasm_wasi02 *wasi02,
    const turbowasm_wasi02_config *config,
    const turbowasm_runtime_config *runtime_config);

/*
 * Ownership-strict destroy. Returns TURBOWASM_INVALID_ARGUMENT while a
 * WASI02-backed Component instance or a provider resource remains live.
 */
turbowasm_status turbowasm_wasi02_destroy(
    turbowasm_wasi02 *wasi02);

/*
 * Instantiate an already-loaded public Component against this WASI 0.2
 * capability context. The resulting instance retains both the Component and
 * WASI02 context; destroy it with turbowasm_component_instance_destroy().
 */
turbowasm_status turbowasm_wasi02_component_instance_create(
    turbowasm_component_instance *instance,
    const turbowasm_component *component,
    turbowasm_wasi02 *wasi02);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_WASI02_H */
