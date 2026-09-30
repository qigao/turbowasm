#ifndef TURBOWASM_WASI02_PROVIDER_H
#define TURBOWASM_WASI02_PROVIDER_H

#include "wasi02_descriptor.h"

#include <turbowasm/runtime.h>
#include <turbowasm/status.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum turbowasm_wasi02_value_kind {
    TURBOWASM_WASI02_VALUE_UNIT = 0,
    TURBOWASM_WASI02_VALUE_BOOL,
    TURBOWASM_WASI02_VALUE_U8,
    TURBOWASM_WASI02_VALUE_U32,
    TURBOWASM_WASI02_VALUE_U64,
    TURBOWASM_WASI02_VALUE_STRING,
    TURBOWASM_WASI02_VALUE_LIST,
    TURBOWASM_WASI02_VALUE_TUPLE,
    TURBOWASM_WASI02_VALUE_RECORD,
    TURBOWASM_WASI02_VALUE_OPTION,
    TURBOWASM_WASI02_VALUE_RESULT,
    TURBOWASM_WASI02_VALUE_RESOURCE
} turbowasm_wasi02_value_kind;

typedef struct turbowasm_wasi02_value turbowasm_wasi02_value;

typedef struct turbowasm_wasi02_value_sequence {
    turbowasm_wasi02_value *items;
    size_t count;
} turbowasm_wasi02_value_sequence;

struct turbowasm_wasi02_value {
    turbowasm_wasi02_value_kind kind;
    union {
        bool boolean;
        uint8_t u8;
        uint32_t u32;
        uint64_t u64;
        struct {
            uint8_t *data;
            size_t size;
        } string;
        turbowasm_wasi02_value_sequence list;
        turbowasm_wasi02_value_sequence tuple;
        turbowasm_wasi02_value_sequence record;
        struct {
            bool has_value;
            turbowasm_wasi02_value *value;
        } option;
        struct {
            bool is_error;
            turbowasm_wasi02_value *value;
        } result;
        uint32_t resource;
    } as;
};

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

typedef struct turbowasm_wasi02_provider {
    turbowasm_wasi02_provider_config config;
    turbowasm_runtime_config runtime_config;
    bool initialized;
} turbowasm_wasi02_provider;

turbowasm_status turbowasm_wasi02_provider_init(
    turbowasm_wasi02_provider *provider,
    const turbowasm_wasi02_provider_config *config,
    const turbowasm_runtime_config *runtime_config);

void turbowasm_wasi02_provider_destroy(
    turbowasm_wasi02_provider *provider);

bool turbowasm_wasi02_value_matches_type(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_wasi02_value *value);

void turbowasm_wasi02_value_destroy(
    turbowasm_wasi02_value *value);

/*
 * Invoke one registered W1 descriptor through caller-owned W2 providers.
 *
 * Arguments are borrowed. Successful results are TurboWasm-owned and must be
 * released with turbowasm_wasi02_value_destroy().
 *
 * wasi:cli/exit is intentionally non-returning at the typed boundary: after a
 * successful host callback this function returns TURBOWASM_INTERRUPTED.
 *
 * Monotonic subscribe-* remains W4 poll/resource work and returns
 * TURBOWASM_UNSUPPORTED here.
 */
turbowasm_status turbowasm_wasi02_provider_call(
    turbowasm_wasi02_provider *provider,
    const char *package_name,
    const char *interface_name,
    const char *function_name,
    const turbowasm_wasi02_value *arguments,
    size_t argument_count,
    turbowasm_wasi02_value *out_result);

#endif /* TURBOWASM_WASI02_PROVIDER_H */
