#include "../src/wasi02_provider.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct provider_state {
    unsigned wall_now_calls;
    unsigned wall_resolution_calls;
    unsigned monotonic_now_calls;
    unsigned monotonic_resolution_calls;
    unsigned random_bytes_calls;
    unsigned random_u64_calls;
    unsigned insecure_bytes_calls;
    unsigned insecure_u64_calls;
    unsigned seed_calls;
    unsigned environment_calls;
    unsigned arguments_calls;
    unsigned cwd_calls;
    unsigned exit_calls;
    bool exit_success;

    uint8_t env_name[4];
    uint8_t env_value[3];
    uint8_t arg0[5];
    uint8_t arg1[4];
    uint8_t cwd[5];

    turbowasm_wasi02_environment_entry_view env[1];
    turbowasm_wasi02_string_view args[2];
} provider_state;

static turbowasm_status wall_now(
    void *context,
    uint64_t *seconds,
    uint32_t *nanoseconds) {
    provider_state *state = (provider_state *)context;
    ++state->wall_now_calls;
    *seconds = UINT64_C(1234);
    *nanoseconds = UINT32_C(567);
    return TURBOWASM_OK;
}

static turbowasm_status wall_resolution(
    void *context,
    uint64_t *seconds,
    uint32_t *nanoseconds) {
    provider_state *state = (provider_state *)context;
    ++state->wall_resolution_calls;
    *seconds = 0u;
    *nanoseconds = UINT32_C(1000);
    return TURBOWASM_OK;
}

static turbowasm_status monotonic_now(
    void *context,
    uint64_t *value) {
    provider_state *state = (provider_state *)context;
    ++state->monotonic_now_calls;
    *value = UINT64_C(9001);
    return TURBOWASM_OK;
}

static turbowasm_status monotonic_resolution(
    void *context,
    uint64_t *value) {
    provider_state *state = (provider_state *)context;
    ++state->monotonic_resolution_calls;
    *value = UINT64_C(10);
    return TURBOWASM_OK;
}

static turbowasm_status random_bytes(
    void *context,
    uint8_t *bytes,
    size_t size) {
    provider_state *state = (provider_state *)context;
    size_t i;
    ++state->random_bytes_calls;
    for (i = 0u; i < size; ++i)
        bytes[i] = (uint8_t)(i + 1u);
    return TURBOWASM_OK;
}

static turbowasm_status insecure_bytes(
    void *context,
    uint8_t *bytes,
    size_t size) {
    provider_state *state = (provider_state *)context;
    size_t i;
    ++state->insecure_bytes_calls;
    for (i = 0u; i < size; ++i)
        bytes[i] = (uint8_t)(UINT8_C(0xa0) + (uint8_t)i);
    return TURBOWASM_OK;
}

static turbowasm_status random_u64(
    void *context,
    uint64_t *value) {
    provider_state *state = (provider_state *)context;
    ++state->random_u64_calls;
    *value = UINT64_C(0x1122334455667788);
    return TURBOWASM_OK;
}

static turbowasm_status insecure_u64(
    void *context,
    uint64_t *value) {
    provider_state *state = (provider_state *)context;
    ++state->insecure_u64_calls;
    *value = UINT64_C(0x8877665544332211);
    return TURBOWASM_OK;
}

static turbowasm_status insecure_seed(
    void *context,
    uint64_t *first,
    uint64_t *second) {
    provider_state *state = (provider_state *)context;
    ++state->seed_calls;
    *first = UINT64_C(11);
    *second = UINT64_C(22);
    return TURBOWASM_OK;
}

static turbowasm_status get_environment(
    void *context,
    const turbowasm_wasi02_environment_entry_view **entries,
    size_t *count) {
    provider_state *state = (provider_state *)context;
    ++state->environment_calls;
    *entries = state->env;
    *count = 1u;
    return TURBOWASM_OK;
}

static turbowasm_status get_arguments(
    void *context,
    const turbowasm_wasi02_string_view **arguments,
    size_t *count) {
    provider_state *state = (provider_state *)context;
    ++state->arguments_calls;
    *arguments = state->args;
    *count = 2u;
    return TURBOWASM_OK;
}

static turbowasm_status get_cwd(
    void *context,
    bool *has_value,
    turbowasm_wasi02_string_view *value) {
    provider_state *state = (provider_state *)context;
    ++state->cwd_calls;
    *has_value = true;
    value->data = state->cwd;
    value->size = sizeof(state->cwd);
    return TURBOWASM_OK;
}

static turbowasm_status do_exit(
    void *context,
    bool success) {
    provider_state *state = (provider_state *)context;
    ++state->exit_calls;
    state->exit_success = success;
    return TURBOWASM_OK;
}

static void init_state(provider_state *state) {
    memset(state, 0, sizeof(*state));

    memcpy(state->env_name, "KEY", 4u);
    memcpy(state->env_value, "ok", 3u);
    memcpy(state->arg0, "prog", 5u);
    memcpy(state->arg1, "arg", 4u);
    memcpy(state->cwd, "/tmp", 5u);

    state->env[0].name.data = state->env_name;
    state->env[0].name.size = 3u;
    state->env[0].value.data = state->env_value;
    state->env[0].value.size = 2u;

    state->args[0].data = state->arg0;
    state->args[0].size = 4u;
    state->args[1].data = state->arg1;
    state->args[1].size = 3u;
}

static turbowasm_wasi02_provider_config provider_config(
    provider_state *state) {
    turbowasm_wasi02_provider_config config;
    memset(&config, 0, sizeof(config));

    config.context = state;
    config.wall_clock_now = wall_now;
    config.wall_clock_resolution = wall_resolution;
    config.monotonic_clock_now = monotonic_now;
    config.monotonic_clock_resolution = monotonic_resolution;
    config.random_bytes = random_bytes;
    config.random_u64 = random_u64;
    config.insecure_random_bytes = insecure_bytes;
    config.insecure_random_u64 = insecure_u64;
    config.insecure_seed = insecure_seed;
    config.environment = get_environment;
    config.arguments = get_arguments;
    config.initial_cwd = get_cwd;
    config.exit = do_exit;
    return config;
}

static void test_clocks(
    turbowasm_wasi02_provider *provider,
    provider_state *state) {
    turbowasm_wasi02_value result = {0};
    turbowasm_wasi02_value arg = {0};

    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:clocks", "wall-clock", "now",
               NULL, 0u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_RECORD);
    assert(result.as.record.count == 2u);
    assert(result.as.record.items[0].kind ==
           TURBOWASM_WASI02_VALUE_U64);
    assert(result.as.record.items[0].as.u64 == UINT64_C(1234));
    assert(result.as.record.items[1].kind ==
           TURBOWASM_WASI02_VALUE_U32);
    assert(result.as.record.items[1].as.u32 == UINT32_C(567));
    assert(state->wall_now_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:clocks", "monotonic-clock", "resolution",
               NULL, 0u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_U64);
    assert(result.as.u64 == UINT64_C(10));
    assert(state->monotonic_resolution_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);

    arg.kind = TURBOWASM_WASI02_VALUE_U64;
    arg.as.u64 = UINT64_C(1);
    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:clocks", "monotonic-clock",
               "subscribe-duration",
               &arg, 1u, &result) == TURBOWASM_UNSUPPORTED);
}

static void test_random(
    turbowasm_wasi02_provider *provider,
    provider_state *state) {
    turbowasm_wasi02_value length = {0};
    turbowasm_wasi02_value result = {0};

    length.kind = TURBOWASM_WASI02_VALUE_U64;
    length.as.u64 = 4u;
    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:random", "random", "get-random-bytes",
               &length, 1u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_LIST);
    assert(result.as.list.count == 4u);
    assert(result.as.list.items[0].kind ==
           TURBOWASM_WASI02_VALUE_U8);
    assert(result.as.list.items[0].as.u8 == 1u);
    assert(result.as.list.items[3].as.u8 == 4u);
    assert(state->random_bytes_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:random", "random", "get-random-u64",
               NULL, 0u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_U64);
    assert(result.as.u64 == UINT64_C(0x1122334455667788));
    assert(state->random_u64_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);

    length.as.u64 = 2u;
    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:random", "insecure",
               "get-insecure-random-bytes",
               &length, 1u, &result) == TURBOWASM_OK);
    assert(result.as.list.items[0].as.u8 == UINT8_C(0xa0));
    assert(result.as.list.items[1].as.u8 == UINT8_C(0xa1));
    assert(state->insecure_bytes_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:random", "insecure-seed", "insecure-seed",
               NULL, 0u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_TUPLE);
    assert(result.as.tuple.count == 2u);
    assert(result.as.tuple.items[0].as.u64 == UINT64_C(11));
    assert(result.as.tuple.items[1].as.u64 == UINT64_C(22));
    assert(state->seed_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);
}

static void test_cli(
    turbowasm_wasi02_provider *provider,
    provider_state *state) {
    turbowasm_wasi02_value result = {0};
    turbowasm_wasi02_value exit_status = {0};

    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:cli", "environment", "get-environment",
               NULL, 0u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_LIST);
    assert(result.as.list.count == 1u);
    assert(result.as.list.items[0].kind ==
           TURBOWASM_WASI02_VALUE_TUPLE);
    assert(result.as.list.items[0].as.tuple.count == 2u);
    assert(memcmp(
               result.as.list.items[0].as.tuple.items[0]
                   .as.string.data,
               "KEY", 3u) == 0);
    assert(state->environment_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:cli", "environment", "get-arguments",
               NULL, 0u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_LIST);
    assert(result.as.list.count == 2u);
    assert(memcmp(
               result.as.list.items[0].as.string.data,
               "prog", 4u) == 0);
    state->arg0[0] = (uint8_t)'X';
    assert(result.as.list.items[0].as.string.data[0] ==
           (uint8_t)'p');
    assert(state->arguments_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);

    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:cli", "environment", "initial-cwd",
               NULL, 0u, &result) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_WASI02_VALUE_OPTION);
    assert(result.as.option.has_value);
    assert(result.as.option.value != NULL);
    assert(result.as.option.value->kind ==
           TURBOWASM_WASI02_VALUE_STRING);
    assert(memcmp(
               result.as.option.value->as.string.data,
               "/tmp", 4u) == 0);
    assert(state->cwd_calls == 1u);
    turbowasm_wasi02_value_destroy(&result);

    exit_status.kind = TURBOWASM_WASI02_VALUE_RESULT;
    exit_status.as.result.is_error = false;
    exit_status.as.result.value = NULL;
    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:cli", "exit", "exit",
               &exit_status, 1u, NULL) == TURBOWASM_INTERRUPTED);
    assert(state->exit_calls == 1u);
    assert(state->exit_success);

    exit_status.as.result.is_error = true;
    assert(turbowasm_wasi02_provider_call(
               provider,
               "wasi:cli", "exit", "exit",
               &exit_status, 1u, NULL) == TURBOWASM_INTERRUPTED);
    assert(state->exit_calls == 2u);
    assert(!state->exit_success);
}

static void test_validation_and_limits(
    provider_state *state) {
    turbowasm_wasi02_provider provider = {0};
    turbowasm_wasi02_provider_config config =
        provider_config(state);
    turbowasm_runtime_config runtime_config;
    turbowasm_wasi02_value argument = {0};
    turbowasm_wasi02_value result = {0};

    assert(turbowasm_wasi02_provider_init(
               &provider, &config, NULL) == TURBOWASM_OK);

    argument.kind = TURBOWASM_WASI02_VALUE_U32;
    argument.as.u32 = 3u;
    assert(turbowasm_wasi02_provider_call(
               &provider,
               "wasi:random", "random", "get-random-bytes",
               &argument, 1u, &result) ==
           TURBOWASM_TYPE_MISMATCH);
    assert(turbowasm_wasi02_provider_call(
               &provider,
               "wasi:random", "random", "missing",
               NULL, 0u, &result) ==
           TURBOWASM_UNSUPPORTED);
    turbowasm_wasi02_provider_destroy(&provider);

    turbowasm_runtime_config_init(&runtime_config);
    runtime_config.limits.max_allocation_bytes = 8u;
    assert(turbowasm_wasi02_provider_init(
               &provider, &config, &runtime_config) ==
           TURBOWASM_OK);
    argument.kind = TURBOWASM_WASI02_VALUE_U64;
    argument.as.u64 = 2u;
    assert(turbowasm_wasi02_provider_call(
               &provider,
               "wasi:random", "random", "get-random-bytes",
               &argument, 1u, &result) ==
           TURBOWASM_OUT_OF_MEMORY);
    turbowasm_wasi02_provider_destroy(&provider);
}

int main(void) {
    provider_state state;
    turbowasm_wasi02_provider provider = {0};
    turbowasm_wasi02_provider_config config;

    init_state(&state);
    config = provider_config(&state);

    assert(turbowasm_wasi02_provider_init(
               &provider, &config, NULL) == TURBOWASM_OK);

    test_clocks(&provider, &state);
    test_random(&provider, &state);
    test_cli(&provider, &state);

    turbowasm_wasi02_provider_destroy(&provider);
    test_validation_and_limits(&state);
    return 0;
}
