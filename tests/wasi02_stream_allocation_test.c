#include "wasi02_streams.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static turbowasm_wasi02_streams streams;
static turbowasm_runtime_config runtime;
static turbowasm_runtime_scope scope;
static size_t budget, live, reads, writes, skips;
static bool fail_after_read, reenter;
static uint32_t input, output;
static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (reenter) {
        check_equal(turbowasm_wasi02_stream_resource_drop(&streams, input), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_wasi02_streams_destroy(&streams), TURBOWASM_INVALID_ARGUMENT);
    }
    if (!budget) return NULL;
    if (budget != SIZE_MAX) --budget;
    p = malloc(size); if (p) ++live; return p;
}
static void deallocate(void *context, void *p) { (void)context; if (p) { --live; free(p); } }
static turbowasm_status read_bytes(void *context, turbowasm_value rep, uint64_t max,
    const uint8_t **data, size_t *size, turbowasm_wasi02_stream_error *error) {
    static const uint8_t bytes[] = "read";
    (void)context; (void)rep; (void)error; ++reads;
    *data = bytes; *size = max < 4 ? (size_t)max : 4;
    if (fail_after_read) budget = 0;
    return TURBOWASM_OK;
}
static turbowasm_status skip_bytes(void *context, turbowasm_value rep, uint64_t max,
    uint64_t *size, turbowasm_wasi02_stream_error *error) {
    (void)context; (void)rep; (void)error; ++skips; *size = max;
    if (fail_after_read) budget = 0;
    return TURBOWASM_OK;
}
static turbowasm_status check_write(void *context, turbowasm_value rep, uint64_t *permit,
    turbowasm_wasi02_stream_error *error) {
    (void)context; (void)rep; (void)error; *permit = 4; return TURBOWASM_OK;
}
static turbowasm_status write_bytes(void *context, turbowasm_value rep, const uint8_t *data,
    size_t size, turbowasm_wasi02_stream_error *error) {
    (void)context; (void)rep; (void)error; ++writes;
    check_equal(size, (size_t)4); check_equal(data, "read", 4); return TURBOWASM_OK;
}
static void drop(void *context, turbowasm_value rep) { (void)context; (void)rep; }

static turbowasm_status call_read(bool skip, turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value args[2] = {{0}};
    args[0].kind = TURBOWASM_WASI02_VALUE_RESOURCE; args[0].as.resource = input;
    args[1].kind = TURBOWASM_WASI02_VALUE_U64; args[1].as.u64 = 4;
    return turbowasm_wasi02_streams_call(&streams, "streams", skip ?
        "[method]input-stream.skip" : "[method]input-stream.read", args, 2, out);
}
static turbowasm_status call_splice(turbowasm_wasi02_value *out) {
    turbowasm_wasi02_value args[3] = {{0}};
    args[0].kind = args[1].kind = TURBOWASM_WASI02_VALUE_RESOURCE;
    args[0].as.resource = output; args[1].as.resource = input;
    args[2].kind = TURBOWASM_WASI02_VALUE_U64; args[2].as.u64 = 4;
    return turbowasm_wasi02_streams_call(&streams, "streams", "[method]output-stream.splice", args, 3, out);
}
suite("stream publication allocation") {
    before_each() {
        turbowasm_wasi02_stream_provider provider = {0}; turbowasm_value rep = {0};
        budget = SIZE_MAX; live = reads = writes = skips = 0; fail_after_read = reenter = false;
        memset(&streams, 0, sizeof(streams)); turbowasm_runtime_config_init(&runtime);
        runtime.allocator.allocate = allocate; runtime.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&runtime);
        provider.input_read = read_bytes; provider.input_skip = skip_bytes;
        provider.output_check_write = check_write; provider.output_write = write_bytes;
        provider.input_drop = provider.output_drop = drop;
        check_equal(turbowasm_wasi02_streams_init(&streams, &provider, 8), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_input_stream_new(&streams, rep, &input), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_output_stream_new(&streams, rep, &output), TURBOWASM_OK);
    }
    after_each() {
        budget = SIZE_MAX; reenter = false;
        check_equal(turbowasm_wasi02_stream_resource_drop(&streams, input), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_stream_resource_drop(&streams, output), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_streams_destroy(&streams), TURBOWASM_OK);
        turbowasm_runtime_scope_leave(scope); check_equal(live, (size_t)0);
    }
    it("preflights every read allocation before consuming provider bytes") {
        size_t baseline = live;
        for (size_t limit = 0; limit < 3; ++limit) {
            turbowasm_wasi02_value result = {0}; budget = limit;
            check_equal(call_read(false, &result), TURBOWASM_OUT_OF_MEMORY);
            turbowasm_wasi02_value_destroy(&result); check_equal(reads, (size_t)0); check_equal(live, baseline);
        }
        budget = SIZE_MAX; fail_after_read = true;
        turbowasm_wasi02_value result = {0}; check_equal(call_read(false, &result), TURBOWASM_OK);
        check_equal(result.as.result.value->as.list.count, (size_t)4);
        turbowasm_wasi02_value_destroy(&result); check_equal(reads, (size_t)1); check_equal(live, baseline);
    }
    it("publishes skip without allocating after provider consumption") {
        turbowasm_wasi02_value result = {0}; budget = 1;
        check_equal(call_read(true, &result), TURBOWASM_OUT_OF_MEMORY); check_equal(skips, (size_t)0);
        budget = SIZE_MAX; fail_after_read = true;
        check_equal(call_read(true, &result), TURBOWASM_OK); check_equal(result.as.result.value->as.u64, 4u);
        turbowasm_wasi02_value_destroy(&result);
    }
    it("preflights splice and allocates nothing after reading or sending") {
        size_t baseline = live;
        for (size_t limit = 0; limit < 5; ++limit) {
            turbowasm_wasi02_value result = {0}; budget = limit;
            check_equal(call_splice(&result), TURBOWASM_OUT_OF_MEMORY);
            turbowasm_wasi02_value_destroy(&result); check_equal(reads, (size_t)0);
            check_equal(writes, (size_t)0); check_equal(live, baseline);
        }
        turbowasm_wasi02_value result = {0}; budget = SIZE_MAX; fail_after_read = true;
        check_equal(call_splice(&result), TURBOWASM_OK); check_equal(result.as.result.value->as.u64, 4u);
        check_equal(writes, (size_t)1); turbowasm_wasi02_value_destroy(&result); check_equal(live, baseline);
    }
    it("rejects allocator reentry without retiring a borrowed input") {
        turbowasm_wasi02_value result = {0}; reenter = true;
        check_equal(call_read(false, &result), TURBOWASM_OK);
        reenter = false; turbowasm_wasi02_value_destroy(&result); check_equal(reads, (size_t)1);
    }
}
