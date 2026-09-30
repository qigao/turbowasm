#include "../src/wasi02_poll.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct poll_probe {
    bool ready;
    bool fail_next_drop;
    uint32_t ready_calls;
    uint32_t arm_calls;
    uint32_t drop_calls;
    turbowasm_value last_rep;
    uintptr_t operation_token;
} poll_probe;

static turbowasm_status probe_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    poll_probe *probe = (poll_probe *)context;

    assert(probe != NULL);
    assert(out_ready != NULL);
    ++probe->ready_calls;
    probe->last_rep = rep;
    *out_ready = probe->ready;
    return TURBOWASM_OK;
}

static turbowasm_status probe_arm(
    void *context,
    turbowasm_value rep,
    uintptr_t *out_operation_token) {
    poll_probe *probe = (poll_probe *)context;

    assert(probe != NULL);
    assert(out_operation_token != NULL);
    ++probe->arm_calls;
    probe->last_rep = rep;
    *out_operation_token = probe->operation_token;
    return TURBOWASM_OK;
}

static turbowasm_status probe_drop(
    void *context,
    turbowasm_value rep) {
    poll_probe *probe = (poll_probe *)context;

    assert(probe != NULL);
    ++probe->drop_calls;
    probe->last_rep = rep;
    if (probe->fail_next_drop) {
        probe->fail_next_drop = false;
        return TURBOWASM_TRAPPED;
    }
    return TURBOWASM_OK;
}

static void test_pollable_lifecycle_and_fail_fast(void) {
    turbowasm_wasi02_poll poll = {0};
    turbowasm_wasi02_poll_provider provider = {0};
    turbowasm_value rep = {0};
    poll_probe probe = {0};
    uint32_t resource = 0u;
    bool ready = false;

    probe.operation_token = (uintptr_t)0x1234u;
    provider.context = &probe;
    provider.ready = probe_ready;
    provider.arm = probe_arm;
    provider.drop = probe_drop;

    assert(turbowasm_wasi02_poll_init(
               &poll, &provider, 2u) == TURBOWASM_OK);

    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = INT64_C(77);
    assert(turbowasm_wasi02_pollable_new(
               &poll, rep, &resource) == TURBOWASM_OK);
    assert(resource != 0u);

    assert(turbowasm_wasi02_pollable_ready(
               &poll, resource, &ready) == TURBOWASM_OK);
    assert(!ready);
    assert(probe.ready_calls == 1u);
    assert(probe.last_rep.kind == TURBOWASM_VALUE_I64);
    assert(probe.last_rep.as.i64 == INT64_C(77));

    /*
     * One-shot/no-call execution cannot wait. Crucially, the provider arm
     * callback is not touched before Runtime admits suspension.
     */
    assert(turbowasm_wasi02_pollable_block(
               &poll, resource, NULL) == TURBOWASM_UNSUPPORTED);
    assert(probe.arm_calls == 0u);

    /* An already-ready pollable never needs a Runtime wait frame. */
    probe.ready = true;
    assert(turbowasm_wasi02_pollable_block(
               &poll, resource, NULL) == TURBOWASM_OK);
    assert(probe.arm_calls == 0u);

    /* Live resources make bridge destruction fail closed. */
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_INVALID_ARGUMENT);

    /*
     * Provider drop runs before logical handle consumption so a transient
     * provider failure leaves the generation-safe resource live for retry.
     */
    probe.fail_next_drop = true;
    assert(turbowasm_wasi02_pollable_drop(
               &poll, resource) == TURBOWASM_TRAPPED);
    assert(probe.drop_calls == 1u);
    assert(turbowasm_wasi02_pollable_ready(
               &poll, resource, &ready) == TURBOWASM_OK);

    assert(turbowasm_wasi02_pollable_drop(
               &poll, resource) == TURBOWASM_OK);
    assert(probe.drop_calls == 2u);
    assert(turbowasm_wasi02_pollable_ready(
               &poll, resource, &ready) == TURBOWASM_TRAPPED);

    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
}

int main(void) {
    test_pollable_lifecycle_and_fail_fast();
    return 0;
}
