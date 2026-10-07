#include <turbowasm/wasi02.h>
#include <tinytest.h>
#include "fixtures/component_wasi_resources.h"
#include <stdlib.h>
#include <string.h>

typedef turbowasm_component_host_value host_value;
enum { STREAM_REP = 11, POLL_REP = 77, RESOURCE_CAPACITY = 8 };
static turbowasm_component component;
static turbowasm_component_instance instance;
static turbowasm_component_call call;
static turbowasm_wasi02 wasi;
static host_value stream, pollable;
static struct {
    size_t live, attempts, fail_at;
    unsigned input_drops, poll_drops, armed, inputs;
    bool ready, fail_drop;
    host_value *reentrant;
} probe;

static void *allocate(void *context, size_t size) {
    void *pointer;
    (void)context;
    if (++probe.attempts == probe.fail_at)
        return NULL;
    pointer = malloc(size);
    if (pointer != NULL)
        ++probe.live;
    return pointer;
}
static void deallocate(void *context, void *pointer) {
    (void)context;
    if (pointer != NULL) {
        check_true(probe.live != 0u);
        --probe.live;
    }
    free(pointer);
}
static turbowasm_status get_stdin(void *context, turbowasm_value *out) {
    (void)context;
    ++probe.inputs;
    *out = (turbowasm_value){.kind = TURBOWASM_VALUE_I64, .as.i64 = STREAM_REP};
    return TURBOWASM_OK;
}
static turbowasm_status subscribe(void *context, turbowasm_value input, turbowasm_value *out) {
    (void)context;
    check_equal(input.kind, TURBOWASM_VALUE_I64);
    check_equal(input.as.i64, (int64_t)STREAM_REP);
    *out = (turbowasm_value){.kind = TURBOWASM_VALUE_I64, .as.i64 = POLL_REP};
    return TURBOWASM_OK;
}
static void drop_input(void *context, turbowasm_value input) {
    (void)context;
    check_equal(input.as.i64, (int64_t)STREAM_REP);
    ++probe.input_drops;
}
static turbowasm_status ready(void *context, turbowasm_value value, bool *out) {
    (void)context;
    check_equal(value.as.i64, (int64_t)POLL_REP);
    *out = probe.ready;
    return TURBOWASM_OK;
}
static turbowasm_status arm(void *context, turbowasm_value value, uintptr_t *out) {
    (void)context;
    check_equal(value.as.i64, (int64_t)POLL_REP);
    *out = ++probe.armed;
    return TURBOWASM_OK;
}
static turbowasm_status drop_poll(void *context, turbowasm_value value) {
    (void)context;
    check_equal(value.as.i64, (int64_t)POLL_REP);
    ++probe.poll_drops;
    if (probe.reentrant != NULL) {
        host_value borrowed = {0};
        check_equal(turbowasm_component_host_value_borrow(probe.reentrant, &borrowed), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_host_value_destroy(probe.reentrant), TURBOWASM_INVALID_ARGUMENT);
    }
    return probe.fail_drop ? TURBOWASM_TRAPPED : TURBOWASM_OK;
}
static turbowasm_name name_span(const char *name) {
    return (turbowasm_name){(const uint8_t *)name, (uint32_t)strlen(name)};
}
static turbowasm_status invoke(const char *name, host_value *argument, host_value *out) {
    size_t count;
    turbowasm_trap trap;
    return turbowasm_component_instance_invoke(&instance, name_span(name), argument,
        argument != NULL ? 1u : 0u, out, out != NULL ? 1u : 0u, &count, &trap);
}
static void acquire_pollable(void) {
    host_value borrowed = {0};
    check_equal(invoke("get", NULL, &stream), TURBOWASM_OK);
    check_equal(turbowasm_component_host_value_borrow(&stream, &borrowed), TURBOWASM_OK);
    check_equal(invoke("subscribe", &borrowed, &pollable), TURBOWASM_OK);
    check_equal(turbowasm_component_host_value_destroy(&borrowed), TURBOWASM_OK);
    check_equal(probe.input_drops, 0u);
}

spec("Component capability owners and host-wait loans") {
    before_each() {
        turbowasm_wasi02_config config = {0};
        turbowasm_runtime_config runtime;
        memset(&probe, 0, sizeof(probe));
        turbowasm_runtime_config_init(&runtime);
        runtime.allocator.allocate = allocate;
        runtime.allocator.deallocate = deallocate;
        config.poll.ready = ready;
        config.poll.arm = arm;
        config.poll.drop = drop_poll;
        config.pollable_capacity = RESOURCE_CAPACITY;
        config.streams.get_stdin = get_stdin;
        config.streams.input_subscribe = subscribe;
        config.streams.input_drop = drop_input;
        config.stream_resource_capacity = RESOURCE_CAPACITY;
        check_equal(turbowasm_wasi02_init(&wasi, &config, &runtime), TURBOWASM_OK);
        check_equal(turbowasm_component_load_borrowed_with_config(&component,
            component_wasi_resources_bytes, sizeof(component_wasi_resources_bytes), &runtime), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_component_instance_create(&instance, &component, &wasi), TURBOWASM_OK);
    }
    after_each() {
        probe.fail_at = 0u;
        probe.reentrant = NULL;
        turbowasm_component_call_destroy(&call);
        check_equal(turbowasm_component_host_value_destroy(&stream), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&pollable), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance);
        turbowasm_component_destroy(&component);
        check_equal(turbowasm_wasi02_destroy(&wasi), TURBOWASM_OK);
        check_equal(probe.live, (size_t)0);
    }
    it("pins the capability owner and resource across host-wait completion") {
        host_value borrowed = {0};
        turbowasm_host_wait wait;
        acquire_pollable();
        check_equal(turbowasm_component_host_value_borrow(&pollable, &borrowed), TURBOWASM_OK);
        check_equal(turbowasm_component_call_create(&call, &instance, name_span("block"), &borrowed, 1u), TURBOWASM_OK);
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_call_yield_reason_get(&call), TURBOWASM_YIELD_HOST_WAIT);
        check_equal(turbowasm_component_host_value_destroy(&pollable), TURBOWASM_INVALID_ARGUMENT);
        turbowasm_component_instance_destroy(&instance);
        turbowasm_component_destroy(&component);
        check_equal(turbowasm_wasi02_destroy(&wasi), TURBOWASM_INVALID_ARGUMENT);
        check_true(turbowasm_component_call_pending_host_wait(&call, &wait));
        probe.ready = true;
        check_equal(turbowasm_component_call_complete_host_wait(&call, wait, 0), TURBOWASM_OK);
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OK);
        check_equal(probe.poll_drops, 0u);
        check_equal(turbowasm_component_host_value_destroy(&borrowed), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&pollable), TURBOWASM_OK);
        check_equal(probe.poll_drops, 1u);
    }
    it("unwinds nested borrow loans when a host-wait call is cancelled") {
        host_value borrowed = {0};
        acquire_pollable();
        check_equal(turbowasm_component_host_value_borrow(&pollable, &borrowed), TURBOWASM_OK);
        check_equal(turbowasm_component_call_create(&call, &instance, name_span("block"), &borrowed, 1u), TURBOWASM_OK);
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_YIELDED);
        turbowasm_component_call_destroy(&call);
        check_equal(probe.poll_drops, 0u);
        check_equal(turbowasm_component_host_value_destroy(&pollable), TURBOWASM_OK);
        check_equal(probe.poll_drops, 1u);
        check_equal(turbowasm_component_host_value_destroy(&borrowed), TURBOWASM_OK);
    }
    it("invalidates the owner before invoking a reentrant provider destructor") {
        host_value shadow;
        acquire_pollable();
        shadow = pollable;
        probe.reentrant = &shadow;
        check_equal(turbowasm_component_host_value_destroy(&pollable), TURBOWASM_OK);
        probe.reentrant = NULL;
        check_equal((int)pollable.kind, 0);
        check_equal(probe.poll_drops, 1u);
    }
    it("moves provider ownership once and drops unstarted transferred calls") {
        host_value output = {0};
        size_t count;
        turbowasm_trap trap;
        acquire_pollable();
        check_equal(turbowasm_component_instance_invoke_move(&instance, name_span("echo"),
            &pollable, 1u, &output, 1u, &count, &trap), TURBOWASM_OK);
        check_equal((int)pollable.kind, 0);
        check_equal(turbowasm_component_call_create_move(&call, &instance,
            name_span("consume"), &output, 1u), TURBOWASM_OK);
        check_equal((int)output.kind, 0);
        turbowasm_component_call_destroy(&call);
        check_equal(probe.poll_drops, 1u);
    }
    it("consumes a failed provider destructor without leaving an unreachable capability") {
        acquire_pollable();
        probe.fail_drop = true;
        check_equal(turbowasm_component_host_value_destroy(&pollable), TURBOWASM_TRAPPED);
        check_equal((int)pollable.kind, 0);
        check_equal(turbowasm_component_host_value_destroy(&pollable), TURBOWASM_OK);
        check_equal(probe.poll_drops, 1u);
        probe.fail_drop = false;
    }
    it("uses the configured allocator for lifted owners and releases reps on failure") {
        host_value output = {0};
        size_t owner_allocation, live;
        unsigned before;
        /* Warm the canonical table; the final resume allocation creates the host
         * owner, after Core execution and canonical handle removal have completed. */
        check_equal(invoke("get", NULL, &stream), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&stream), TURBOWASM_OK);
        check_equal(turbowasm_component_call_create(&call, &instance,
            name_span("get"), NULL, 0u), TURBOWASM_OK);
        probe.attempts = 0u;
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OK);
        owner_allocation = probe.attempts;
        check_true(owner_allocation != 0u);
        check_equal(turbowasm_component_call_take_result(&call, &output), TURBOWASM_OK);
        turbowasm_component_call_destroy(&call);
        check_equal(turbowasm_component_host_value_destroy(&output), TURBOWASM_OK);
        live = probe.live;
        before = probe.input_drops;
        check_equal(turbowasm_component_call_create(&call, &instance,
            name_span("get"), NULL, 0u), TURBOWASM_OK);
        probe.attempts = 0u;
        probe.fail_at = owner_allocation;
        check_equal(turbowasm_component_call_resume(&call, NULL), TURBOWASM_OUT_OF_MEMORY);
        probe.fail_at = 0u;
        check_equal(probe.input_drops, before + 1u);
        check_equal(probe.inputs, probe.input_drops);
        turbowasm_component_call_destroy(&call);
        check_equal(probe.live, live);
    }
}
