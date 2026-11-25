#include "../src/wasi02_exec.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct clock_probe {
    uint32_t calls;
    uint64_t value;
} clock_probe;

static turbowasm_status monotonic_now(
    void *context,
    uint64_t *out_value) {
    clock_probe *probe = (clock_probe *)context;

    if (probe == NULL || out_value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++probe->calls;
    *out_value = probe->value;
    return TURBOWASM_OK;
}

static turbowasm_status poll_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    (void)context;
    (void)rep;
    if (out_ready == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_ready = true;
    return TURBOWASM_OK;
}

static const uint8_t monotonic_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
    0x01,0x23,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x05,0x01,0x60,0x00,0x01,0x7e,
      0x02,0x09,0x01,
        0x01,'p',
        0x03,'n','o','w',
        0x00,0x00,
      0x07,0x07,0x01,
        0x03,'r','u','n',
        0x00,0x00,
    0x07,0x14,0x02,
      0x42,0x02,
        0x01,0x40,0x00,0x00,0x77,
        0x04,0x00,0x03,'n','o','w',0x01,0x00,
      0x40,0x00,0x00,0x77,
    0x0a,0x26,0x01,
      0x00,0x21,
      'w','a','s','i',':','c','l','o','c','k','s','/',
      'm','o','n','o','t','o','n','i','c','-',
      'c','l','o','c','k','@','0','.','2','.','8',
      0x05,0x00,
    0x06,0x08,0x01,
      0x01,0x00,0x00,0x03,'n','o','w',
    0x08,0x05,0x01,
      0x01,0x00,0x00,0x00,
    0x02,0x10,0x02,
      0x01,0x01,
        0x03,'n','o','w',0x00,0x00,
      0x00,0x00,0x01,
        0x01,'p',0x12,0x00,
    0x06,0x09,0x01,
      0x00,0x00,0x01,0x01,0x03,'r','u','n',
    0x08,0x06,0x01,
      0x00,0x00,0x01,0x00,0x01,
    0x0b,0x09,0x01,
      0x00,0x03,'r','u','n',
      0x01,0x01,0x00
};

static void test_provider_and_poll_share_one_exec(void) {
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_wasi02_provider provider = {0};
    turbowasm_wasi02_provider_config provider_config = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_wasi02_poll_provider poll_provider = {0};
    turbowasm_wasi02_exec_capabilities capabilities = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    clock_probe probe = {0};

    probe.value = UINT64_C(123456);
    provider_config.context = &probe;
    provider_config.monotonic_clock_now = monotonic_now;
    poll_provider.ready = poll_ready;

    assert(turbowasm_wasi02_provider_init(
               &provider,
               &provider_config,
               NULL) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_init(
               &poll,
               &poll_provider,
               4u) == TURBOWASM_OK);
    assert(turbowasm_component_binary_load(
               &binary,
               monotonic_component,
               sizeof(monotonic_component)) == TURBOWASM_OK);

    capabilities.provider = &provider;
    capabilities.poll = &poll;
    assert(turbowasm_wasi02_exec_init(
               &exec,
               &binary,
               &capabilities) == TURBOWASM_OK);
    assert(exec.import_set_count == 2u);

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"run",
               3u,
               NULL,
               0u,
               &result,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U64);
    assert(result.as.u64 == probe.value);
    assert(probe.calls == 1u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&binary);
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
    turbowasm_wasi02_provider_destroy(&provider);
}

static void test_streams_requires_same_poll_owner(void) {
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_wasi02_poll poll_a = {0};
    turbowasm_wasi02_poll poll_b = {0};
    turbowasm_wasi02_poll_provider provider = {0};
    turbowasm_wasi02_streams streams = {0};
    turbowasm_wasi02_stream_provider stream_provider = {0};
    turbowasm_wasi02_exec_capabilities capabilities = {0};

    provider.ready = poll_ready;
    assert(turbowasm_wasi02_poll_init(
               &poll_a, &provider, 4u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_init(
               &poll_b, &provider, 4u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_init(
               &streams,
               &stream_provider,
               4u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_streams_attach_poll(
               &streams, &poll_a) == TURBOWASM_OK);
    assert(turbowasm_component_binary_load(
               &binary,
               monotonic_component,
               sizeof(monotonic_component)) == TURBOWASM_OK);

    capabilities.poll = &poll_b;
    capabilities.streams = &streams;
    assert(turbowasm_wasi02_exec_init(
               &exec,
               &binary,
               &capabilities) == TURBOWASM_INVALID_ARGUMENT);
    assert(exec.binary == NULL);

    turbowasm_component_binary_destroy(&binary);
    assert(turbowasm_wasi02_streams_destroy(
               &streams) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll_b) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll_a) == TURBOWASM_OK);
}

int main(void) {
    test_provider_and_poll_share_one_exec();
    test_streams_requires_same_poll_owner();
    return 0;
}
