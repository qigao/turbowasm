#include "wasi02_io_api.h"
#include "wasi02_poll.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

typedef struct probe {
    uint32_t refs, drops, reads, error_drops;
    bool ready;
} probe;
static turbowasm_status ready(void *context, bool *out) { *out = ((probe *)context)->ready; return TURBOWASM_OK; }
static void retain(void *context) { ++((probe *)context)->refs; }
static void release(void *context) { --((probe *)context)->refs; }
static turbowasm_wasi02_io_source_ops operations(probe *p) {
    return (turbowasm_wasi02_io_source_ops){p, ready, retain, release};
}
static turbowasm_wasi02_io io;
static turbowasm_wasi02_io_source sources[3];
static turbowasm_wasi02_stream_provider streams;
static turbowasm_wasi02_poll_provider poll;
static probe probes[3];

static const uint8_t module_bytes[] = {
    0,97,115,109,1,0,0,0, 1,5,1,96,0,1,127,
    2,13,1,4,'h','o','s','t',4,'p','o','l','l',0,0,
    3,2,1,0, 10,6,1,4,0,16,0,11
};
typedef struct host_probe { turbowasm_wasi02_poll *poll; uint32_t resources[3]; } host_probe;
static turbowasm_status wait_many(void *context, turbowasm_host_call *call,
    const turbowasm_value *arguments, size_t count, turbowasm_value *results, size_t capacity,
    size_t *result_count, turbowasm_trap *trap) {
    host_probe *host = context; turbowasm_component_value result = {0}; turbowasm_status status;
    (void)arguments; (void)count; (void)capacity;
    status = turbowasm_wasi02_poll_many(host->poll, host->resources, 3, call, &result, trap);
    if (status != TURBOWASM_OK) return status;
    results[0].kind = TURBOWASM_VALUE_I32; results[0].as.i32 = (int32_t)result.as.list.count;
    *result_count = 1; turbowasm_component_value_destroy(&result); return TURBOWASM_OK;
}
static turbowasm_name name(const char *s) { return (turbowasm_name){(const uint8_t *)s, (uint32_t)strlen(s)}; }
static void exercise_wait(bool cancel) {
    turbowasm_wasi02_poll bridge = {0}; host_probe host = {0}; turbowasm_value reps[2] = {{0}};
    turbowasm_module module = {0}; turbowasm_linker linker = {0}; turbowasm_instance instance = {0};
    turbowasm_execution execution = {0};
    const turbowasm_value_kind result_kind = TURBOWASM_VALUE_I32;
    const turbowasm_host_function_type type = {NULL, 0, &result_kind, 1};
    check_equal(turbowasm_wasi02_poll_init(&bridge, &poll, 4), TURBOWASM_OK);
    bridge.wait_done = turbowasm_wasi02_io_wait_done; bridge.wait_done_context = poll.context;
    host.poll = &bridge;
    for (unsigned i = 0; i < 2; ++i) {
        check_equal(turbowasm_wasi02_io_pollable_register(&io, sources[i], &reps[i]), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_pollable_new(&bridge, reps[i], &host.resources[i]), TURBOWASM_OK);
    }
    host.resources[2] = host.resources[1];
    check_equal(turbowasm_module_load_borrowed(&module, module_bytes, sizeof(module_bytes)), TURBOWASM_OK);
    check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
    check_equal(turbowasm_linker_define_host_function(&linker, name("host"), name("poll"), &type, wait_many, &host), TURBOWASM_OK);
    check_equal(turbowasm_instance_create_linked(&instance, &module, &linker), TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);
    check_equal(turbowasm_execution_create(&execution, &instance, 1, NULL, 0), TURBOWASM_OK);
    check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_YIELDED);
    if (cancel) turbowasm_execution_destroy(&execution);
    else {
        probes[1].ready = true;
        check_equal(turbowasm_wasi02_io_source_changed(&io, sources[1]), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_advance(&io), TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution, NULL), TURBOWASM_OK);
        check_equal(turbowasm_execution_result_at(&execution, 0)->as.i32, 2);
        turbowasm_execution_destroy(&execution);
    }
    for (unsigned i = 0; i < 2; ++i) check_equal(turbowasm_wasi02_pollable_drop(&bridge, host.resources[i]), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_poll_destroy(&bridge), TURBOWASM_OK);
    turbowasm_instance_destroy(&instance); turbowasm_module_destroy(&module);
    check_equal(turbowasm_wasi02_io_advance(&io), TURBOWASM_OK);
}
static turbowasm_status read_bytes(void *context, turbowasm_value rep, uint64_t max,
    const uint8_t **data, size_t *size, turbowasm_wasi02_stream_error *error) {
    (void)context; (void)rep; error->kind = TURBOWASM_WASI02_STREAM_ERROR_NONE;
    *data = (const uint8_t *)"abc"; *size = max < 3 ? (size_t)max : 3; return TURBOWASM_OK;
}
static void drop_bytes(void *context, turbowasm_value rep) { (void)rep; ++((probe *)context)->drops; }
static turbowasm_status read_failure(void *context, turbowasm_value rep, uint64_t max,
    const uint8_t **data, size_t *size, turbowasm_wasi02_stream_error *error) {
    (void)rep; (void)max; ++((probe *)context)->reads; *data = NULL; *size = 0;
    error->kind = TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED;
    error->error_rep = (turbowasm_value){.kind = TURBOWASM_VALUE_I32, .as.i32 = 7};
    return TURBOWASM_OK;
}
static turbowasm_status debug_failure(void *context, turbowasm_value rep, turbowasm_wasi02_string_view *out) {
    check_equal(((probe *)context)->refs, 1u); check_equal(rep.as.i32, 7);
    *out = (turbowasm_wasi02_string_view){(const uint8_t *)"failure", 7}; return TURBOWASM_OK;
}
static void drop_failure(void *context, turbowasm_value rep) {
    check_equal(rep.as.i32, 7); ++((probe *)context)->error_drops;
}
static turbowasm_wasi02_io_config small_config(void) {
    turbowasm_wasi02_io_config config;
    turbowasm_wasi02_io_config_init(&config);
    config.sources = config.streams = config.errors = config.subscriptions = config.routes = config.members_per_route = 1;
    return config;
}
typedef struct allocation_probe { turbowasm_wasi02_io *io; size_t budget, live; } allocation_probe;
static void *allocate_checked(void *context, size_t size) {
    allocation_probe *p = context; void *result;
    check_equal(turbowasm_wasi02_io_init(p->io, NULL, NULL), TURBOWASM_INVALID_ARGUMENT);
    check_equal(turbowasm_wasi02_io_destroy(p->io), TURBOWASM_INVALID_ARGUMENT);
    if (!p->budget) return NULL;
    if (p->budget != SIZE_MAX) --p->budget;
    result = malloc(size); if (result) ++p->live; return result;
}
static void free_checked(void *context, void *pointer) {
    allocation_probe *p = context; if (pointer) { --p->live; free(pointer); }
}
suite("shared WASI I/O readiness") {
    before_each() {
        memset(&io, 0, sizeof(io)); memset(sources, 0, sizeof(sources)); memset(probes, 0, sizeof(probes));
        check_equal(turbowasm_wasi02_io_init(&io, NULL, NULL), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_providers(&io, &streams, &poll), TURBOWASM_OK);
        for (unsigned i = 0; i < 3; ++i) {
            turbowasm_wasi02_io_source_ops ops = operations(&probes[i]);
            check_equal(turbowasm_wasi02_io_source_register(&io, &ops, &sources[i]), TURBOWASM_OK);
        }
    }
    after_each() {
        for (unsigned i = 0; i < 3; ++i) {
            if (sources[i].token) check_equal(turbowasm_wasi02_io_source_close(&io, &sources[i]), TURBOWASM_OK);
            check_equal(probes[i].refs, 0u);
        }
        check_equal(turbowasm_wasi02_io_destroy(&io), TURBOWASM_OK);
    }
    it("reuses the same subscription across multiple readiness cycles") {
        turbowasm_value rep = {0}; bool state;
        check_equal(turbowasm_wasi02_io_pollable_register(&io, sources[0], &rep), TURBOWASM_OK);
        for (unsigned i = 0; i < 6; ++i) {
            probes[0].ready = (i & 1u) != 0;
            check_equal(poll.ready(poll.context, rep, &state), TURBOWASM_OK);
            check_equal(state, probes[0].ready);
        }
        check_equal(poll.drop(poll.context, rep), TURBOWASM_OK);
        check_equal(poll.ready(poll.context, rep, &state), TURBOWASM_TRAPPED);
    }
    it("keeps closed sources until every child subscription retires") {
        turbowasm_value reps[2]; bool state;
        for (unsigned i = 0; i < 2; ++i) check_equal(turbowasm_wasi02_io_pollable_register(&io, sources[0], &reps[i]), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_source_close(&io, &sources[0]), TURBOWASM_OK);
        check_equal(probes[0].refs, 1u);
        check_equal(poll.ready(poll.context, reps[0], &state), TURBOWASM_OK); check_true(state);
        for (unsigned i = 0; i < 2; ++i) check_equal(poll.drop(poll.context, reps[i]), TURBOWASM_OK);
        check_equal(probes[0].refs, 0u);
    }
    it("rejects cross-domain source tokens without consuming ownership") {
        turbowasm_wasi02_io other = {0}; turbowasm_value rep = {0};
        check_equal(turbowasm_wasi02_io_init(&other, NULL, NULL), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_pollable_register(&other, sources[0], &rep), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_wasi02_io_destroy(&other), TURBOWASM_OK);
    }
    it("wraps provider streams and preserves their drop obligation") {
        turbowasm_wasi02_stream_provider ops = {0}; turbowasm_value rep = {0}, own = {0}, child = {0};
        turbowasm_wasi02_stream_error error = {0}; const uint8_t *data; size_t count;
        ops.context = &probes[0]; ops.input_read = read_bytes; ops.input_drop = drop_bytes;
        check_equal(turbowasm_wasi02_io_stream_register(&io, TURBOWASM_WASI02_IO_INPUT, &ops, own, sources[0], &rep), TURBOWASM_OK);
        check_equal(streams.input_read(streams.context, rep, 3, &data, &count, &error), TURBOWASM_OK);
        check_equal(count, (size_t)3); check_equal(data, "abc", 3);
        check_equal(streams.input_subscribe(streams.context, rep, &child), TURBOWASM_OK);
        streams.input_drop(streams.context, rep); check_equal(probes[0].drops, 1u);
        check_equal(poll.drop(poll.context, child), TURBOWASM_OK);
    }
    it("completes wait-any and retains duplicate input positions") { exercise_wait(false); }
    it("unregisters a cancelled execution without cancelling shared sources") { exercise_wait(true); }
    it("rejects exhausted quotas and wrong-kind reps without consuming ownership") {
        turbowasm_wasi02_io local = {0}; turbowasm_wasi02_io_config config = small_config();
        turbowasm_wasi02_io_source source = {0}, extra = {0}; probe p = {0}; bool state;
        turbowasm_wasi02_io_source_ops ops = operations(&p);
        turbowasm_wasi02_stream_provider delegate = {0}, wrapped;
        turbowasm_wasi02_poll_provider local_poll; turbowasm_value rep = {0}, alias = {0}, rejected = {0};
        delegate.context = &p; delegate.input_drop = drop_bytes;
        check_equal(turbowasm_wasi02_io_init(&local, &config, NULL), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_providers(&local, &wrapped, &local_poll), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_source_register(&local, &ops, &source), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_source_register(&local, &ops, &extra), TURBOWASM_OUT_OF_MEMORY);
        check_equal(extra.token, (uint64_t)0); check_equal(p.refs, 1u);
        check_equal(turbowasm_wasi02_io_stream_register(&local, TURBOWASM_WASI02_IO_INPUT, &delegate, rejected, source, &rep), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_stream_register(&local, TURBOWASM_WASI02_IO_INPUT, &delegate, rejected, source, &rejected), TURBOWASM_OUT_OF_MEMORY);
        check_equal(p.drops, 0u);
        check_equal(local_poll.ready(local_poll.context, rep, &state), TURBOWASM_TRAPPED);
        check_equal(turbowasm_wasi02_io_pollable_register(&local, source, &alias), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_pollable_register(&local, source, &rejected), TURBOWASM_OUT_OF_MEMORY);
        wrapped.input_drop(wrapped.context, rep); check_equal(p.drops, 1u);
        check_equal(local_poll.drop(local_poll.context, alias), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_source_close(&local, &source), TURBOWASM_OK);
        check_equal(p.refs, 0u); check_equal(turbowasm_wasi02_io_destroy(&local), TURBOWASM_OK);
    }
    it("reserves error capacity before reads and retains the owner through error drop") {
        turbowasm_wasi02_io local = {0}; turbowasm_wasi02_io_config config = small_config();
        turbowasm_wasi02_io_source source = {0}; probe p = {0};
        turbowasm_wasi02_io_source_ops ops = operations(&p);
        turbowasm_wasi02_stream_provider delegate = {0}, wrapped;
        turbowasm_wasi02_poll_provider local_poll; turbowasm_value rep = {0}, own = {0};
        turbowasm_wasi02_stream_error error = {0}, rejected = {0};
        turbowasm_wasi02_string_view debug; const uint8_t *data; size_t size;
        delegate.context = &p; delegate.input_read = read_failure; delegate.input_drop = drop_bytes;
        delegate.error_debug = debug_failure; delegate.error_drop = drop_failure;
        check_equal(turbowasm_wasi02_io_init(&local, &config, NULL), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_providers(&local, &wrapped, &local_poll), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_source_register(&local, &ops, &source), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_stream_register(&local, TURBOWASM_WASI02_IO_INPUT, &delegate, own, source, &rep), TURBOWASM_OK);
        check_equal(wrapped.input_read(wrapped.context, rep, 1, &data, &size, &error), TURBOWASM_OK);
        check_equal(wrapped.input_read(wrapped.context, rep, 1, &data, &size, &rejected), TURBOWASM_OUT_OF_MEMORY);
        check_equal(p.reads, 1u);
        wrapped.input_drop(wrapped.context, rep);
        check_equal(turbowasm_wasi02_io_source_close(&local, &source), TURBOWASM_OK);
        check_equal(p.refs, 1u); check_equal(turbowasm_wasi02_io_destroy(&local), TURBOWASM_INVALID_ARGUMENT);
        check_equal(wrapped.error_debug(wrapped.context, error.error_rep, &debug), TURBOWASM_OK);
        check_equal(debug.size, (size_t)7); check_equal(debug.data, "failure", 7);
        wrapped.error_drop(wrapped.context, error.error_rep);
        check_equal(p.error_drops, 1u); check_equal(p.refs, 0u);
        check_equal(turbowasm_wasi02_io_destroy(&local), TURBOWASM_OK);
    }
    it("rolls back every initialization allocation and rejects allocator reentry") {
        turbowasm_wasi02_io local = {0}; turbowasm_runtime_config config;
        allocation_probe p = {&local, 0, 0};
        turbowasm_runtime_config_init(&config);
        config.allocator = (turbowasm_allocator){&p, allocate_checked, NULL, free_checked};
        for (size_t budget = 0; budget < 7; ++budget) {
            p.budget = budget;
            check_equal(turbowasm_wasi02_io_init(&local, NULL, &config), TURBOWASM_OUT_OF_MEMORY);
            check_true(local.impl == NULL); check_equal(p.live, (size_t)0);
        }
        p.budget = SIZE_MAX;
        check_equal(turbowasm_wasi02_io_init(&local, NULL, &config), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_destroy(&local), TURBOWASM_OK); check_equal(p.live, (size_t)0);
    }
}
