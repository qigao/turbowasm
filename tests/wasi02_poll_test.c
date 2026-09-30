#include "../src/wasi02_poll.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

typedef struct poll_state {
    uint32_t ready_calls;
    uint32_t block_calls;
    uint32_t poll_calls;
    uint32_t drop_calls;
    bool fail_next_drop;
} poll_state;

static turbowasm_status fake_ready(
    void *context,
    uint64_t token,
    bool *out_ready) {
    poll_state *state = (poll_state *)context;

    assert(state != NULL);
    assert(out_ready != NULL);
    ++state->ready_calls;
    *out_ready = (token & UINT64_C(1)) == 0u;
    return TURBOWASM_OK;
}

static turbowasm_status fake_block(
    void *context,
    turbowasm_host_call *call,
    uint64_t token) {
    poll_state *state = (poll_state *)context;

    assert(state != NULL);
    assert(call != NULL);
    assert(token == UINT64_C(11));
    ++state->block_calls;
    return TURBOWASM_OK;
}

static turbowasm_status fake_poll(
    void *context,
    turbowasm_host_call *call,
    const uint64_t *tokens,
    size_t token_count,
    uint32_t *out_indices,
    size_t result_capacity,
    size_t *out_count) {
    poll_state *state = (poll_state *)context;
    size_t i;
    size_t count = 0u;

    assert(state != NULL);
    assert(call != NULL);
    assert(tokens != NULL);
    assert(out_indices != NULL);
    assert(out_count != NULL);
    ++state->poll_calls;

    for (i = 0u; i < token_count; ++i) {
        if ((tokens[i] & UINT64_C(1)) == 0u) {
            assert(count < result_capacity);
            out_indices[count++] = (uint32_t)i;
        }
    }
    *out_count = count;
    return TURBOWASM_OK;
}

static turbowasm_status fake_drop(
    void *context,
    uint64_t token) {
    poll_state *state = (poll_state *)context;

    assert(state != NULL);
    (void)token;
    ++state->drop_calls;
    if (state->fail_next_drop) {
        state->fail_next_drop = false;
        return TURBOWASM_TRAPPED;
    }
    return TURBOWASM_OK;
}

static void test_pollable_provider_lifecycle(void) {
    poll_state state = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_wasi02_poll_config config = {0};
    turbowasm_host_call fake_call = {0};
    uint32_t h1 = 0u;
    uint32_t h2 = 0u;
    uint32_t h3 = 0u;
    uint32_t handles[2];
    uint32_t indices[2] = {UINT32_MAX, UINT32_MAX};
    size_t count = 0u;
    bool ready = false;

    config.context = &state;
    config.ready = fake_ready;
    config.block = fake_block;
    config.poll_many = fake_poll;
    config.drop = fake_drop;

    assert(turbowasm_wasi02_poll_init(
               &poll, &config, 2u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_new(
               &poll, UINT64_C(11), &h1) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_new(
               &poll, UINT64_C(22), &h2) == TURBOWASM_OK);
    assert(h1 != 0u && h2 != 0u && h1 != h2);

    assert(turbowasm_wasi02_pollable_new(
               &poll, UINT64_C(33), &h3) ==
           TURBOWASM_OUT_OF_MEMORY);

    assert(turbowasm_wasi02_pollable_ready(
               &poll, h1, &ready) == TURBOWASM_OK);
    assert(!ready);
    assert(turbowasm_wasi02_pollable_ready(
               &poll, h2, &ready) == TURBOWASM_OK);
    assert(ready);
    assert(state.ready_calls == 2u);

    assert(turbowasm_wasi02_pollable_block(
               &poll, &fake_call, h1) == TURBOWASM_OK);
    assert(state.block_calls == 1u);

    handles[0] = h1;
    handles[1] = h2;
    assert(turbowasm_wasi02_poll_many(
               &poll, &fake_call,
               handles, 2u,
               indices, 2u, &count) == TURBOWASM_OK);
    assert(state.poll_calls == 1u);
    assert(count == 1u);
    assert(indices[0] == 1u);

    assert(turbowasm_wasi02_poll_many(
               &poll, &fake_call,
               handles, 0u,
               indices, 2u, &count) ==
           TURBOWASM_INVALID_ARGUMENT);

    /* Provider drop failure leaves the handle live for retry. */
    state.fail_next_drop = true;
    assert(turbowasm_wasi02_pollable_drop(
               &poll, h1) == TURBOWASM_TRAPPED);
    assert(turbowasm_wasi02_pollable_ready(
               &poll, h1, &ready) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_drop(
               &poll, h1) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_ready(
               &poll, h1, &ready) == TURBOWASM_TRAPPED);

    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_wasi02_pollable_drop(
               &poll, h2) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);

    assert(state.drop_calls == 3u);
}

static turbowasm_status bad_poll(
    void *context,
    turbowasm_host_call *call,
    const uint64_t *tokens,
    size_t token_count,
    uint32_t *out_indices,
    size_t result_capacity,
    size_t *out_count) {
    (void)context;
    (void)call;
    (void)tokens;
    assert(token_count == 1u);
    assert(result_capacity >= 1u);
    out_indices[0] = 1u;
    *out_count = 1u;
    return TURBOWASM_OK;
}

static void test_invalid_provider_result_traps(void) {
    poll_state state = {0};
    turbowasm_wasi02_poll poll = {0};
    turbowasm_wasi02_poll_config config = {0};
    turbowasm_host_call fake_call = {0};
    uint32_t handle = 0u;
    uint32_t index = 0u;
    size_t count = 0u;

    config.context = &state;
    config.ready = fake_ready;
    config.block = fake_block;
    config.poll_many = bad_poll;
    config.drop = fake_drop;

    assert(turbowasm_wasi02_poll_init(
               &poll, &config, 1u) == TURBOWASM_OK);
    assert(turbowasm_wasi02_pollable_new(
               &poll, UINT64_C(22), &handle) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_many(
               &poll, &fake_call,
               &handle, 1u,
               &index, 1u, &count) == TURBOWASM_TRAPPED);
    assert(turbowasm_wasi02_pollable_drop(
               &poll, handle) == TURBOWASM_OK);
    assert(turbowasm_wasi02_poll_destroy(
               &poll) == TURBOWASM_OK);
}

int main(void) {
    test_pollable_provider_lifecycle();
    test_invalid_provider_result_traps();
    return 0;
}
