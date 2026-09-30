#include "../src/wasi02_component.h"

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

int main(void) {
    test_monotonic_import_executes_through_provider();
    test_missing_provider_is_link_error();
    return 0;
}
