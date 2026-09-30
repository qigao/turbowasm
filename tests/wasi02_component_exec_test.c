#include "../src/wasi02_component.h"
#include "../src/instance_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct clock_state {
    uint32_t calls;
    uint64_t value;
} clock_state;

static turbowasm_status monotonic_now(
    void *context,
    uint64_t *out_value) {
    clock_state *state = (clock_state *)context;

    if (state == NULL || out_value == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++state->calls;
    *out_value = state->value;
    return TURBOWASM_OK;
}

/*
 * Real flat imported-interface execution chain:
 *
 *   import "wasi:clocks/monotonic-clock@0.2.8"
 *     : instance { now: func() -> u64 }
 *       |
 *       +-- alias export "now" -> component func0
 *       +-- canon lower func0 -> core func0
 *       +-- inline core provider exports core func0 as p.now
 *       +-- Core module imports p.now and re-exports it as run
 *       +-- canon lift run -> component func1
 *       '-- export "run"
 */
static const uint8_t monotonic_component[] = {
    /* Component preamble. */
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,

    /* Embedded Core module0:
     *   (import "p" "now" (func () -> i64))
     *   (export "run" (func 0))
     */
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

    /* type0 = instance { now: func() -> u64 }
     * type1 = func() -> u64
     */
    0x07,0x14,0x02,
      0x42,0x02,
        0x01,0x40,0x00,0x00,0x77,
        0x04,0x00,0x03,'n','o','w',0x01,0x00,
      0x40,0x00,0x00,0x77,

    /* import instance0 = wasi:clocks/monotonic-clock@0.2.8 : type0 */
    0x0a,0x26,0x01,
      0x00,0x21,
      'w','a','s','i',':','c','l','o','c','k','s','/',
      'm','o','n','o','t','o','n','i','c','-',
      'c','l','o','c','k','@','0','.','2','.','8',
      0x05,0x00,

    /* component func0 = alias export instance0 "now" */
    0x06,0x08,0x01,
      0x01,0x00,0x00,0x03,'n','o','w',

    /* core func0 = canon lower component func0, no options */
    0x08,0x05,0x01,
      0x01,0x00,0x00,0x00,

    /* core instance0 = inline { "now" -> core func0 }
     * core instance1 = instantiate module0 with "p" -> instance0
     */
    0x02,0x10,0x02,
      0x01,0x01,
        0x03,'n','o','w',0x00,0x00,
      0x00,0x00,0x01,
        0x01,'p',0x12,0x00,

    /* core func1 = alias core export instance1 "run" */
    0x06,0x09,0x01,
      0x00,0x00,0x01,0x01,0x03,'r','u','n',

    /* component func1 = canon lift core func1, no options, type1 */
    0x08,0x06,0x01,
      0x00,0x00,0x01,0x00,0x01,

    /* export "run" -> component func1 */
    0x0b,0x09,0x01,
      0x00,0x03,'r','u','n',
      0x01,0x01,0x00
};


typedef struct memory_provider_state {
    uint32_t wall_calls;
    uint32_t random_calls;
} memory_provider_state;

static turbowasm_status wall_now_value(
    void *context,
    uint64_t *seconds,
    uint32_t *nanoseconds) {
    memory_provider_state *state = (memory_provider_state *)context;
    if (state == NULL || seconds == NULL || nanoseconds == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    ++state->wall_calls;
    *seconds = UINT64_C(1234);
    *nanoseconds = UINT32_C(567);
    return TURBOWASM_OK;
}

static turbowasm_status random_bytes_value(
    void *context,
    uint8_t *bytes,
    size_t size) {
    memory_provider_state *state = (memory_provider_state *)context;
    size_t i;
    if (state == NULL || (size != 0u && bytes == NULL))
        return TURBOWASM_INVALID_ARGUMENT;
    ++state->random_calls;
    for (i = 0u; i < size; ++i)
        bytes[i] = (uint8_t)(i + 1u);
    return TURBOWASM_OK;
}

static const uint8_t wall_clock_memory_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,0x01,0x38,0x00,0x61,
    0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x09,0x01,0x60,0x04,0x7f,
    0x7f,0x7f,0x7f,0x01,0x7f,0x03,0x02,0x01,0x00,0x05,0x03,0x01,
    0x00,0x01,0x07,0x11,0x02,0x03,0x6d,0x65,0x6d,0x02,0x00,0x07,
    0x72,0x65,0x61,0x6c,0x6c,0x6f,0x63,0x00,0x00,0x0a,0x07,0x01,
    0x05,0x00,0x41,0x80,0x02,0x0b,0x01,0x43,0x00,0x61,0x73,0x6d,
    0x01,0x00,0x00,0x00,0x01,0x09,0x02,0x60,0x01,0x7f,0x00,0x60,
    0x00,0x01,0x7e,0x02,0x12,0x02,0x01,0x6d,0x03,0x6d,0x65,0x6d,
    0x02,0x00,0x01,0x01,0x70,0x03,0x6e,0x6f,0x77,0x00,0x00,0x03,
    0x02,0x01,0x01,0x07,0x07,0x01,0x03,0x72,0x75,0x6e,0x00,0x01,
    0x0a,0x0d,0x01,0x0b,0x00,0x41,0x00,0x10,0x00,0x41,0x00,0x29,
    0x03,0x00,0x0b,0x02,0x04,0x01,0x00,0x00,0x00,0x06,0x15,0x02,
    0x00,0x02,0x01,0x00,0x03,0x6d,0x65,0x6d,0x00,0x00,0x01,0x00,
    0x07,0x72,0x65,0x61,0x6c,0x6c,0x6f,0x63,0x07,0x3b,0x02,0x42,
    0x04,0x01,0x72,0x02,0x07,0x73,0x65,0x63,0x6f,0x6e,0x64,0x73,
    0x77,0x0b,0x6e,0x61,0x6e,0x6f,0x73,0x65,0x63,0x6f,0x6e,0x64,
    0x73,0x79,0x04,0x00,0x08,0x64,0x61,0x74,0x65,0x74,0x69,0x6d,
    0x65,0x03,0x00,0x00,0x01,0x40,0x00,0x00,0x01,0x04,0x00,0x03,
    0x6e,0x6f,0x77,0x01,0x02,0x40,0x00,0x00,0x77,0x0a,0x21,0x01,
    0x00,0x1c,0x77,0x61,0x73,0x69,0x3a,0x63,0x6c,0x6f,0x63,0x6b,
    0x73,0x2f,0x77,0x61,0x6c,0x6c,0x2d,0x63,0x6c,0x6f,0x63,0x6b,
    0x40,0x30,0x2e,0x32,0x2e,0x38,0x05,0x00,0x06,0x08,0x01,0x01,
    0x00,0x00,0x03,0x6e,0x6f,0x77,0x08,0x07,0x01,0x01,0x00,0x00,
    0x01,0x03,0x00,0x02,0x14,0x02,0x01,0x01,0x03,0x6e,0x6f,0x77,
    0x00,0x01,0x00,0x01,0x02,0x01,0x6d,0x12,0x00,0x01,0x70,0x12,
    0x01,0x06,0x09,0x01,0x00,0x00,0x01,0x02,0x03,0x72,0x75,0x6e,
    0x08,0x06,0x01,0x00,0x00,0x02,0x00,0x01,0x0b,0x09,0x01,0x00,
    0x03,0x72,0x75,0x6e,0x01,0x01,0x00,
};

static const uint8_t random_bytes_memory_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,0x01,0x38,0x00,0x61,
    0x73,0x6d,0x01,0x00,0x00,0x00,0x01,0x09,0x01,0x60,0x04,0x7f,
    0x7f,0x7f,0x7f,0x01,0x7f,0x03,0x02,0x01,0x00,0x05,0x03,0x01,
    0x00,0x01,0x07,0x11,0x02,0x03,0x6d,0x65,0x6d,0x02,0x00,0x07,
    0x72,0x65,0x61,0x6c,0x6c,0x6f,0x63,0x00,0x00,0x0a,0x07,0x01,
    0x05,0x00,0x41,0x80,0x02,0x0b,0x01,0x4a,0x00,0x61,0x73,0x6d,
    0x01,0x00,0x00,0x00,0x01,0x0a,0x02,0x60,0x02,0x7e,0x7f,0x00,
    0x60,0x00,0x01,0x7f,0x02,0x13,0x02,0x01,0x6d,0x03,0x6d,0x65,
    0x6d,0x02,0x00,0x01,0x01,0x70,0x04,0x72,0x61,0x6e,0x64,0x00,
    0x00,0x03,0x02,0x01,0x01,0x07,0x07,0x01,0x03,0x72,0x75,0x6e,
    0x00,0x01,0x0a,0x12,0x01,0x10,0x00,0x42,0x04,0x41,0x00,0x10,
    0x00,0x41,0x00,0x28,0x02,0x00,0x2d,0x00,0x00,0x0b,0x02,0x04,
    0x01,0x00,0x00,0x00,0x06,0x15,0x02,0x00,0x02,0x01,0x00,0x03,
    0x6d,0x65,0x6d,0x00,0x00,0x01,0x00,0x07,0x72,0x65,0x61,0x6c,
    0x6c,0x6f,0x63,0x07,0x29,0x02,0x42,0x03,0x01,0x70,0x7d,0x01,
    0x40,0x01,0x03,0x6c,0x65,0x6e,0x77,0x00,0x00,0x04,0x00,0x10,
    0x67,0x65,0x74,0x2d,0x72,0x61,0x6e,0x64,0x6f,0x6d,0x2d,0x62,
    0x79,0x74,0x65,0x73,0x01,0x01,0x40,0x00,0x00,0x79,0x0a,0x1d,
    0x01,0x00,0x18,0x77,0x61,0x73,0x69,0x3a,0x72,0x61,0x6e,0x64,
    0x6f,0x6d,0x2f,0x72,0x61,0x6e,0x64,0x6f,0x6d,0x40,0x30,0x2e,
    0x32,0x2e,0x38,0x05,0x00,0x06,0x15,0x01,0x01,0x00,0x00,0x10,
    0x67,0x65,0x74,0x2d,0x72,0x61,0x6e,0x64,0x6f,0x6d,0x2d,0x62,
    0x79,0x74,0x65,0x73,0x08,0x09,0x01,0x01,0x00,0x00,0x02,0x03,
    0x00,0x04,0x00,0x02,0x15,0x02,0x01,0x01,0x04,0x72,0x61,0x6e,
    0x64,0x00,0x01,0x00,0x01,0x02,0x01,0x6d,0x12,0x00,0x01,0x70,
    0x12,0x01,0x06,0x09,0x01,0x00,0x00,0x01,0x02,0x03,0x72,0x75,
    0x6e,0x08,0x06,0x01,0x00,0x00,0x02,0x00,0x01,0x0b,0x09,0x01,
    0x00,0x03,0x72,0x75,0x6e,0x01,0x01,0x00,
};


static void test_monotonic_import_executes_through_provider(void) {
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_wasi02_provider provider = {0};
    turbowasm_wasi02_provider_config config = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    clock_state state = {0};

    state.value = UINT64_C(424242);
    config.context = &state;
    config.monotonic_clock_now = monotonic_now;

    assert(turbowasm_component_binary_load(
               &binary,
               monotonic_component,
               sizeof(monotonic_component)) == TURBOWASM_OK);
    assert(binary.import_count == 1u);
    assert(binary.canon_lower_count == 1u);
    assert(binary.canon_lift_count == 1u);
    assert(binary.core_instance_count == 2u);

    /* Closed Component execution still rejects the import. */
    assert(turbowasm_component_exec_init(
               &exec, &binary) == TURBOWASM_UNSUPPORTED);
    memset(&exec, 0, sizeof(exec));

    assert(turbowasm_wasi02_provider_init(
               &provider, &config, NULL) == TURBOWASM_OK);
    assert(turbowasm_wasi02_component_exec_init(
               &exec, &binary, &provider) == TURBOWASM_OK);

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"run", 3u,
               NULL, 0u,
               &result, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U64);
    assert(result.as.u64 == UINT64_C(424242));
    assert(state.calls == 1u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_wasi02_provider_destroy(&provider);
    turbowasm_component_binary_destroy(&binary);
}

static void test_missing_provider_is_link_error(void) {
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_wasi02_provider provider = {0};
    turbowasm_wasi02_provider_config config = {0};

    assert(turbowasm_component_binary_load(
               &binary,
               monotonic_component,
               sizeof(monotonic_component)) == TURBOWASM_OK);
    assert(turbowasm_wasi02_provider_init(
               &provider, &config, NULL) == TURBOWASM_OK);

    /*
     * Type/name matching succeeds, but the W2a provider itself has no
     * monotonic-clock callback. The import boundary therefore stays explicit:
     * instantiation succeeds and the call reports unsupported capability.
     */
    assert(turbowasm_wasi02_component_exec_init(
               &exec, &binary, &provider) == TURBOWASM_OK);

    {
        turbowasm_component_value result = {0};
        turbowasm_trap trap = TURBOWASM_TRAP_NONE;
        assert(turbowasm_component_exec_invoke_export(
                   &exec,
                   (const uint8_t *)"run", 3u,
                   NULL, 0u,
                   &result, &trap) == TURBOWASM_UNSUPPORTED);
        assert(trap == TURBOWASM_TRAP_NONE);
        turbowasm_component_value_destroy(&result);
    }

    turbowasm_component_exec_destroy(&exec);
    turbowasm_wasi02_provider_destroy(&provider);
    turbowasm_component_binary_destroy(&binary);
}


static void test_wall_clock_record_uses_indirect_result_memory(void) {
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_wasi02_provider provider = {0};
    turbowasm_wasi02_provider_config config = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    memory_provider_state state = {0};
    turbowasm_instance_impl *memory_instance;
    uint8_t nanos[4] = {0};
    const turbowasm_component_canon_lower *lower;

    config.context = &state;
    config.wall_clock_now = wall_now_value;

    assert(turbowasm_component_binary_load(
               &binary,
               wall_clock_memory_component,
               sizeof(wall_clock_memory_component)) == TURBOWASM_OK);
    assert(binary.canon_lower_count == 1u);
    lower = turbowasm_component_binary_canon_lower_at(&binary, 0u);
    assert(lower != NULL);
    assert(lower->has_memory);
    assert(lower->memory_index == 0u);
    assert(!lower->has_realloc);

    assert(turbowasm_wasi02_provider_init(
               &provider, &config, NULL) == TURBOWASM_OK);
    assert(turbowasm_wasi02_component_exec_init(
               &exec, &binary, &provider) == TURBOWASM_OK);

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"run", 3u,
               NULL, 0u,
               &result, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U64);
    assert(result.as.u64 == UINT64_C(1234));
    assert(state.wall_calls == 1u);

    memory_instance = (turbowasm_instance_impl *)exec.core_instances[0].impl;
    assert(memory_instance != NULL);
    assert(turbowasm_instance_memory_read_bytes(
               memory_instance, 0u, 8u, 0u,
               nanos, sizeof(nanos)) == TURBOWASM_OK);
    assert(nanos[0] == 0x37u);
    assert(nanos[1] == 0x02u);
    assert(nanos[2] == 0u);
    assert(nanos[3] == 0u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_wasi02_provider_destroy(&provider);
    turbowasm_component_binary_destroy(&binary);
}

static void test_random_bytes_uses_realloc_and_guest_payload(void) {
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_wasi02_provider provider = {0};
    turbowasm_wasi02_provider_config config = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    memory_provider_state state = {0};
    turbowasm_instance_impl *memory_instance;
    uint8_t pair[8] = {0};
    uint8_t payload[4] = {0};
    const turbowasm_component_canon_lower *lower;

    config.context = &state;
    config.random_bytes = random_bytes_value;

    assert(turbowasm_component_binary_load(
               &binary,
               random_bytes_memory_component,
               sizeof(random_bytes_memory_component)) == TURBOWASM_OK);
    assert(binary.canon_lower_count == 1u);
    lower = turbowasm_component_binary_canon_lower_at(&binary, 0u);
    assert(lower != NULL);
    assert(lower->has_memory);
    assert(lower->memory_index == 0u);
    assert(lower->has_realloc);
    assert(lower->realloc_function_index == 0u);

    assert(turbowasm_wasi02_provider_init(
               &provider, &config, NULL) == TURBOWASM_OK);
    assert(turbowasm_wasi02_component_exec_init(
               &exec, &binary, &provider) == TURBOWASM_OK);

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"run", 3u,
               NULL, 0u,
               &result, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == 1u);
    assert(state.random_calls == 1u);

    memory_instance = (turbowasm_instance_impl *)exec.core_instances[0].impl;
    assert(memory_instance != NULL);
    assert(turbowasm_instance_memory_read_bytes(
               memory_instance, 0u, 0u, 0u,
               pair, sizeof(pair)) == TURBOWASM_OK);
    assert(pair[0] == 0x00u);
    assert(pair[1] == 0x01u);
    assert(pair[2] == 0u);
    assert(pair[3] == 0u);
    assert(pair[4] == 4u);
    assert(pair[5] == 0u);
    assert(pair[6] == 0u);
    assert(pair[7] == 0u);

    assert(turbowasm_instance_memory_read_bytes(
               memory_instance, 0u, 256u, 0u,
               payload, sizeof(payload)) == TURBOWASM_OK);
    assert(payload[0] == 1u);
    assert(payload[1] == 2u);
    assert(payload[2] == 3u);
    assert(payload[3] == 4u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_wasi02_provider_destroy(&provider);
    turbowasm_component_binary_destroy(&binary);
}

int main(void) {
    test_monotonic_import_executes_through_provider();
    test_missing_provider_is_link_error();
    test_wall_clock_record_uses_indirect_result_memory();
    test_random_bytes_uses_realloc_and_guest_payload();
    return 0;
}
