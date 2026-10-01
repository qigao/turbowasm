#include <turbowasm/wasi02.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TURBOWASM_WASI02_FIXTURE_DIR
#error "TURBOWASM_WASI02_FIXTURE_DIR must be defined"
#endif

typedef struct fixture_probe {
    uint64_t clock_value;
    uint64_t random_value;
    uint32_t clock_calls;
    uint32_t random_calls;
    uint32_t exit_calls;
    bool exit_success;
} fixture_probe;

static turbowasm_status monotonic_now(
    void *context,
    uint64_t *out_value) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL || out_value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->clock_calls;
    *out_value = probe->clock_value;
    return TURBOWASM_OK;
}

static turbowasm_status random_u64(
    void *context,
    uint64_t *out_value) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL || out_value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->random_calls;
    *out_value = probe->random_value;
    return TURBOWASM_OK;
}

static turbowasm_status exit_status(
    void *context,
    bool success) {
    fixture_probe *probe = (fixture_probe *)context;
    if (probe == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->exit_calls;
    probe->exit_success = success;
    return TURBOWASM_OK;
}

static uint8_t *read_fixture(
    const char *name,
    size_t *out_size) {
    char path[1024];
    FILE *file;
    long length;
    uint8_t *bytes;

    assert(name != NULL);
    assert(out_size != NULL);
    assert(snprintf(
               path, sizeof(path),
               "%s/%s",
               TURBOWASM_WASI02_FIXTURE_DIR,
               name) > 0);

    file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    assert(length > 8);
    assert(fseek(file, 0, SEEK_SET) == 0);

    bytes = (uint8_t *)malloc((size_t)length);
    assert(bytes != NULL);
    assert(fread(bytes, 1u, (size_t)length, file) ==
           (size_t)length);
    assert(fclose(file) == 0);

    assert(bytes[0] == 0x00u);
    assert(bytes[1] == 0x61u);
    assert(bytes[2] == 0x73u);
    assert(bytes[3] == 0x6du);
    assert(bytes[4] == 0x0du);
    assert(bytes[5] == 0x00u);
    assert(bytes[6] == 0x01u);
    assert(bytes[7] == 0x00u);

    *out_size = (size_t)length;
    return bytes;
}

static turbowasm_name run_name(void) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)"run";
    name.size = 3u;
    return name;
}

static void run_u64_fixture(
    turbowasm_wasi02 *wasi02,
    const char *filename,
    uint64_t expected) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_component_host_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t result_count = 0u;
    size_t size = 0u;
    uint8_t *bytes = read_fixture(filename, &size);

    assert(turbowasm_component_load_borrowed(
               &component, bytes, size) == TURBOWASM_OK);
    assert(turbowasm_wasi02_component_instance_create(
               &instance, &component, wasi02) == TURBOWASM_OK);
    assert(turbowasm_component_instance_invoke(
               &instance,
               run_name(),
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_COMPONENT_HOST_U64);
    assert(result.as.u64 == expected);

    turbowasm_component_host_value_destroy(&result);
    turbowasm_component_instance_destroy(&instance);
    turbowasm_component_destroy(&component);
    free(bytes);
}

static void run_exit_fixture(
    turbowasm_wasi02 *wasi02) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    size_t result_count = 99u;
    size_t size = 0u;
    uint8_t *bytes = read_fixture(
        "cli-exit.wasm", &size);

    assert(turbowasm_component_load_borrowed(
               &component, bytes, size) == TURBOWASM_OK);
    assert(turbowasm_wasi02_component_instance_create(
               &instance, &component, wasi02) == TURBOWASM_OK);
    assert(turbowasm_component_instance_invoke(
               &instance,
               run_name(),
               NULL, 0u,
               NULL, 0u,
               &result_count,
               &trap) == TURBOWASM_INTERRUPTED);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 0u);

    turbowasm_component_instance_destroy(&instance);
    turbowasm_component_destroy(&component);
    free(bytes);
}

int main(void) {
    turbowasm_wasi02 wasi02 = {0};
    turbowasm_wasi02_config config = {0};
    fixture_probe probe = {0};

    probe.clock_value = UINT64_C(0x1122334455667788);
    probe.random_value = UINT64_C(0x8877665544332211);

    config.provider.context = &probe;
    config.provider.monotonic_clock_now = monotonic_now;
    config.provider.random_u64 = random_u64;
    config.provider.exit = exit_status;

    assert(turbowasm_wasi02_init(
               &wasi02, &config, NULL) == TURBOWASM_OK);

    run_u64_fixture(
        &wasi02,
        "monotonic-clock.wasm",
        probe.clock_value);
    run_u64_fixture(
        &wasi02,
        "random-u64.wasm",
        probe.random_value);
    run_exit_fixture(&wasi02);

    assert(probe.clock_calls == 1u);
    assert(probe.random_calls == 1u);
    assert(probe.exit_calls == 1u);
    assert(probe.exit_success);

    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_OK);
    return 0;
}
