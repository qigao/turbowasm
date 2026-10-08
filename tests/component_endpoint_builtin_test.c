#include "component_endpoint_builtin.h"
#include "component_endpoint.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_endpoint_builtin.h"
#ifdef TURBOWASM_TEST_MIR
#include "instance_internal.h"
#include "jit/mir_backend.h"
#endif

static const struct {
    const char *name;
    turbowasm_component_async_builtin_kind kind;
    uint32_t type;
    bool wide, async, memory;
} definitions[] = {
    {"snew", TURBOWASM_COMPONENT_STREAM_NEW, 0, false, false, false},
    {"fnew", TURBOWASM_COMPONENT_FUTURE_NEW, 1, false, false, false},
    {"sread", TURBOWASM_COMPONENT_STREAM_READ, 0, false, true, true},
    {"swrite", TURBOWASM_COMPONENT_STREAM_WRITE, 0, false, true, true},
    {"sreadsync", TURBOWASM_COMPONENT_STREAM_READ, 0, false, false, true},
    {"swritesync", TURBOWASM_COMPONENT_STREAM_WRITE, 0, false, false, true},
    {"sread64", TURBOWASM_COMPONENT_STREAM_READ, 0, true, true, true},
    {"swrite64", TURBOWASM_COMPONENT_STREAM_WRITE, 0, true, true, true},
    {"fread", TURBOWASM_COMPONENT_FUTURE_READ, 1, false, true, true},
    {"fwrite", TURBOWASM_COMPONENT_FUTURE_WRITE, 1, false, true, true},
    {"freadsync", TURBOWASM_COMPONENT_FUTURE_READ, 1, false, false, true},
    {"fwritesync", TURBOWASM_COMPONENT_FUTURE_WRITE, 1, false, false, true},
    {"scancelr", TURBOWASM_COMPONENT_STREAM_CANCEL_READ, 0, false, false, false},
    {"scancelw", TURBOWASM_COMPONENT_STREAM_CANCEL_WRITE, 0, false, true, false},
    {"fcancelr", TURBOWASM_COMPONENT_FUTURE_CANCEL_READ, 1, false, false, false},
    {"fcancelw", TURBOWASM_COMPONENT_FUTURE_CANCEL_WRITE, 1, false, true, false},
    {"sdropr", TURBOWASM_COMPONENT_STREAM_DROP_READABLE, 0, false, false, false},
    {"sdropw", TURBOWASM_COMPONENT_STREAM_DROP_WRITABLE, 0, false, false, false},
    {"fdropr", TURBOWASM_COMPONENT_FUTURE_DROP_READABLE, 1, false, false, false},
    {"fdropw", TURBOWASM_COMPONENT_FUTURE_DROP_WRITABLE, 1, false, false, false},
    {"sforward", TURBOWASM_COMPONENT_STREAM_FORWARD, 0, false, false, false},
    {"fforward", TURBOWASM_COMPONENT_FUTURE_FORWARD, 1, false, false, false},
    {"fread64", TURBOWASM_COMPONENT_FUTURE_READ, 1, true, true, true},
    {"fwrite64", TURBOWASM_COMPONENT_FUTURE_WRITE, 1, true, true, true},
    {"textread", TURBOWASM_COMPONENT_STREAM_READ, 2, false, true, true},
    {"textwrite", TURBOWASM_COMPONENT_STREAM_WRITE, 2, false, true, true},
    {"unitread", TURBOWASM_COMPONENT_STREAM_READ, 4, false, true, false},
    {"unitwrite", TURBOWASM_COMPONENT_STREAM_WRITE, 4, false, true, false},
};
enum { BUILTIN_COUNT = sizeof(definitions) / sizeof(definitions[0]) };
static turbowasm_component_task_builtin builtins[BUILTIN_COUNT];
static turbowasm_component_type_graph graph;
static turbowasm_component_resource_table table;
static turbowasm_component_task_domain domain;
static turbowasm_component_task tasks[3];
static turbowasm_component_waitable_set set;
static turbowasm_module module;
static turbowasm_instance instance;
static turbowasm_linker linker;
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static turbowasm_component_canonical_memory memories[2];
static uint64_t inputs[3];
static bool may_leave;
static size_t live, allowance;
static unsigned returns;
static turbowasm_component_endpoint external_reader, external_writer;
static bool probe_copy_reentry;
static bool fail_realloc;
static unsigned reallocations;
enum { HOST_PAIR_LIMIT = 16 };
static turbowasm_component_endpoint *host_readers[HOST_PAIR_LIMIT], *host_writers[HOST_PAIR_LIMIT];
static turbowasm_component_endpoint_codec host_codec;
static turbowasm_component_value host_value, host_number;
static turbowasm_component_buffer host_buffer;

static void close_host_pair(unsigned index) {
    turbowasm_component_endpoint *ends[2] = {host_readers[index], host_writers[index]};
    unsigned i;
    host_readers[index] = host_writers[index] = NULL;
    for (i = 0u; i < 2u; ++i) if (ends[i] != NULL && !ends[i]->closed) {
        if (ends[i]->operation != NULL) {
            turbowasm_component_event event;
            if (ends[i]->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
                check_equal(turbowasm_component_endpoint_cancel(ends[i]), TURBOWASM_OK);
            (void)turbowasm_component_endpoint_take(ends[i], &event);
        }
        check_equal(turbowasm_component_endpoint_close(ends[i]), TURBOWASM_OK);
    }
    turbowasm_component_endpoint_domain_collect(&domain);
}

static turbowasm_status realloc_text(void *context, uint64_t old_pointer, uint64_t old_size,
    uint64_t alignment, uint64_t size, uint64_t *out) {
    (void)context; (void)alignment; check_equal(old_pointer, 0u); check_equal(old_size, 0u); check_less_equal(size, 16u);
    ++reallocations;
    if (fail_realloc) return TURBOWASM_OUT_OF_MEMORY;
    if (probe_copy_reentry) {
        turbowasm_component_event event;
        check_true(external_reader.waitable.sync_waiter); check_true(external_reader.waitable.delivering);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_take(&table, external_reader.waitable.handle, &event), TURBOWASM_TRAPPED);
    }
    *out = 256; return TURBOWASM_OK;
}

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live; return p;
}
static void deallocate(void *context, void *p) { (void)context; if (p != NULL) --live; free(p); }
static turbowasm_name name(const char *s) {
    turbowasm_name n = {(const uint8_t *)s, (uint32_t)strlen(s)}; return n;
}
static uint32_t function_index(const char *s) {
    size_t i;
    for (i = 0; i < turbowasm_module_export_count(&module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(&module, i);
        if (e->name.size == strlen(s) && memcmp(e->name.bytes, s, e->name.size) == 0) return e->item_index;
    }
    return UINT32_MAX;
}
static void compiled(const char *entry) {
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)instance.impl)->jit_functions[function_index(entry)].state, TURBOWASM_JIT_COMPILED);
#else
    (void)entry;
#endif
}
static turbowasm_status host(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t count, turbowasm_value *out, size_t capacity,
    size_t *out_count, turbowasm_trap *trap) {
    (void)call; check_equal(count, 1u); *out_count = 0; *trap = TURBOWASM_TRAP_NONE;
    if (context == NULL) {
        check_less((uint32_t)args[0].as.i32, 3u); check_greater_equal(capacity, 1u);
        out[0].kind = TURBOWASM_VALUE_I64; out[0].as.i64 = (int64_t)inputs[(uint32_t)args[0].as.i32];
        *out_count = 1; return TURBOWASM_OK;
    }
    ++returns;
    return turbowasm_component_task_return_flat(&domain, &graph, true,
        turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U64), NULL, args, count);
}
static turbowasm_status start(unsigned index, const char *entry, uint64_t handle, uint64_t pointer, uint64_t length) {
    turbowasm_component_task_binding b = {0}; turbowasm_status status;
    inputs[0] = handle; inputs[1] = pointer; inputs[2] = length;
    b.graph = &graph; b.function_type = 3; b.instance = &instance; b.function_index = function_index(entry);
    status = turbowasm_component_task_create(&tasks[index], &domain, &b);
    if (status != TURBOWASM_OK) return status;
    return turbowasm_component_task_resume(&tasks[index], NULL);
}
static uint64_t result(unsigned index) {
    turbowasm_component_value value = {0}; uint64_t word;
    check_equal(turbowasm_component_task_take_result(&tasks[index], &value), TURBOWASM_OK);
    word = value.as.u64; check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
    check_equal(turbowasm_component_task_destroy(&tasks[index]), TURBOWASM_OK); return word;
}
static uint64_t run(const char *entry, uint64_t handle, uint64_t pointer, uint64_t length) {
    check_equal(start(1, entry, handle, pointer, length), TURBOWASM_OK);
    compiled(entry); return result(1);
}
static turbowasm_component_endpoint *endpoint(uint32_t handle) {
    return turbowasm_component_endpoint_get(&table, handle, turbowasm_component_handle_kind_get(&table, handle));
}
static void store_u32(unsigned wide, uint64_t address, uint32_t word) {
    turbowasm_component_value value = {0}; value.kind = TURBOWASM_COMPONENT_TYPE_U32; value.as.u32 = word;
    check_equal(turbowasm_component_canonical_lower_value(&graph, turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32),
        &memories[wide], address, &value), TURBOWASM_OK);
}
static uint32_t load_u32(unsigned wide, uint64_t address) {
    turbowasm_component_value value = {0}; uint32_t word;
    check_equal(turbowasm_component_canonical_lift_value(&graph, turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32),
        &memories[wide], address, &value), TURBOWASM_OK);
    word = value.as.u32; check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK); return word;
}
static uint32_t take(uint32_t handle) {
    turbowasm_component_event event;
    check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_OK); return event.payload;
}
static void close_pair(uint64_t packed, bool future) {
    run(future ? "fdropr" : "sdropr", (uint32_t)packed, 0, 0);
    run(future ? "fdropw" : "sdropw", packed >> 32, 0, 0);
}

spec("Core endpoint builtin ownership and progress") {
    before_each() {
        static const turbowasm_value_kind i32 = TURBOWASM_VALUE_I32, i64 = TURBOWASM_VALUE_I64;
        static const turbowasm_value_kind kinds[] = {TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I64,TURBOWASM_VALUE_F32,TURBOWASM_VALUE_F64};
        turbowasm_host_function_type get = {&i32,1,&i64,1}, ret = {&i64,1,NULL,0};
        unsigned i, j;
        live = returns = 0; allowance = SIZE_MAX; may_leave = true;
        probe_copy_reentry = fail_realloc = false; reallocations = 0;
        memset(host_readers, 0, sizeof(host_readers)); memset(host_writers, 0, sizeof(host_writers));
        memset(&host_codec, 0, sizeof(host_codec)); memset(&host_buffer, 0, sizeof(host_buffer));
        memset(&host_value, 0, sizeof(host_value)); memset(&host_number, 0, sizeof(host_number));
        memset(&external_reader, 0, sizeof(external_reader)); memset(&external_writer, 0, sizeof(external_writer));
        memset(builtins, 0, sizeof(builtins)); memset(memories, 0, sizeof(memories)); memset(&set, 0, sizeof(set));
        turbowasm_runtime_config_init(&config); config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
        check_true(turbowasm_component_resource_table_init(&table, 16));
        check_true(turbowasm_component_task_domain_init(&domain, &table, &may_leave, 4));
        check_true(turbowasm_component_type_graph_allocate(&graph, 5));
        for (i = 0; i < 3; ++i)
            check_true(turbowasm_component_type_graph_define_async_value(&graph, i,
                i == 1 ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM, true,
                turbowasm_component_type_ref_inline(i == 2 ? TURBOWASM_COMPONENT_TYPE_STRING : TURBOWASM_COMPONENT_TYPE_U32)));
        check_true(turbowasm_component_type_graph_define_function(&graph, 3, NULL, 0, true,
            turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U64)));
        graph.types[3].as.function.is_async = true;
        check_true(turbowasm_component_type_graph_define_async_value(&graph, 4, TURBOWASM_COMPONENT_TYPE_STREAM, false,
            turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32)));
        check_equal(turbowasm_module_load_borrowed_with_config(&module, component_endpoint_builtin_bytes,
            sizeof(component_endpoint_builtin_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_linker_init_with_config(&linker, &config), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker, name("t"), name("get"), &get, host, NULL), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_host_function(&linker, name("t"), name("return"), &ret, host, &domain), TURBOWASM_OK);
        for (i = 0; i < BUILTIN_COUNT; ++i) {
            turbowasm_component_async_builtin d = {0}; turbowasm_component_flat_signature sig;
            turbowasm_value_kind params[3], results[1]; turbowasm_host_function_type type;
            d.kind = definitions[i].kind; d.type_index = definitions[i].type;
            d.has_memory = definitions[i].memory; d.memory_index = definitions[i].wide; d.is_async = definitions[i].async;
            check_equal(turbowasm_component_async_builtin_signature(&graph, &d,
                definitions[i].wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32, &sig), TURBOWASM_OK);
            for (j = 0; j < sig.param_count; ++j) params[j] = kinds[sig.params[j]];
            for (j = 0; j < sig.result_count; ++j) results[j] = kinds[sig.results[j]];
            type.params = params; type.param_count = sig.param_count; type.results = results; type.result_count = sig.result_count;
            check_equal(turbowasm_linker_define_host_function(&linker, name("e"), name(definitions[i].name), &type,
                turbowasm_component_task_builtin_invoke, &builtins[i]), TURBOWASM_OK);
        }
        check_equal(turbowasm_instance_create_linked(&instance, &module, &linker), TURBOWASM_OK);
        for (i = 0; i < 2; ++i) {
            memories[i].instance = &instance; memories[i].memory_index = i;
            memories[i].pointer_type = i ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
            memories[i].guest_realloc = realloc_text;
        }
        for (i = 0; i < BUILTIN_COUNT; ++i) {
            turbowasm_component_async_builtin d = {0};
            d.kind = definitions[i].kind; d.type_index = definitions[i].type;
            d.has_memory = definitions[i].memory; d.memory_index = definitions[i].wide; d.is_async = definitions[i].async;
            check_equal(turbowasm_component_task_builtin_bind(&builtins[i], &domain, &graph, &d,
                d.has_memory ? &memories[d.memory_index] : NULL), TURBOWASM_OK);
        }
#ifdef TURBOWASM_TEST_MIR
        { turbowasm_jit_backend backend = {0};
          check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
          check_equal(turbowasm_jit_instance_attach_backend(instance.impl, &backend, 1), TURBOWASM_OK); }
#endif
    }
    after_each() {
        unsigned i; allowance = SIZE_MAX;
        for (i = 0; i < 3; ++i) check_equal(turbowasm_component_task_destroy(&tasks[i]), TURBOWASM_OK);
        if (host_codec.table != NULL)
            check_equal(turbowasm_component_endpoint_codec_rollback(&host_codec), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&host_value), TURBOWASM_OK);
        for (i = 0u; i < HOST_PAIR_LIMIT; ++i)
            if (host_readers[i] != NULL || host_writers[i] != NULL) close_host_pair(i);
        if (external_writer.initialized && !external_writer.closed) {
            if (external_writer.operation != NULL) {
                turbowasm_component_event event;
                if (external_writer.waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
                    check_equal(turbowasm_component_endpoint_cancel(&external_writer), TURBOWASM_OK);
                (void)turbowasm_component_endpoint_take(&external_writer, &event);
            }
            check_equal(turbowasm_component_endpoint_close(&external_writer), TURBOWASM_OK);
        }
        for (i = 0; i < table.capacity; ++i) {
            uint32_t handle; turbowasm_component_handle_kind kind; void *object;
            if (turbowasm_component_handle_at(&table, i, &handle, &kind, &object)) {
                turbowasm_component_endpoint *end = turbowasm_component_endpoint_get(&table, handle, kind);
                if (end != NULL) {
                    if (end->operation != NULL) {
                        turbowasm_component_event event;
                        if (end->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
                            check_equal(turbowasm_component_endpoint_cancel(end), TURBOWASM_OK);
                        (void)turbowasm_component_endpoint_take(end, &event);
                    }
                    check_equal(turbowasm_component_endpoint_close(end), TURBOWASM_OK);
                }
            }
        }
        if (set.table != NULL) check_equal(turbowasm_component_waitable_set_drop(&table, set.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_task_domain_destroy(&domain), TURBOWASM_OK);
        turbowasm_component_resource_table_destroy(&table); turbowasm_component_type_graph_destroy(&graph);
        turbowasm_instance_destroy(&instance); turbowasm_linker_destroy(&linker); turbowasm_module_destroy(&module);
        turbowasm_runtime_scope_leave(scope); check_equal(live, 0u);
    }
    it("creates packed handles and reclaims closed pair storage") {
        uint64_t pair = run("snew", 0, 0, 0); uint32_t reader = (uint32_t)pair, writer = (uint32_t)(pair >> 32);
        check_not_equal(reader, writer); check_not_null(endpoint(reader)); check_not_null(endpoint(writer));
        check_true(endpoint(reader)->peer == endpoint(writer)); check_equal(domain.pair_count, 1u);
        check_equal(turbowasm_component_task_domain_destroy(&domain), TURBOWASM_TRAPPED);
        close_pair(pair, false); check_equal(domain.pair_count, 0u); check_equal(table.live_count, 0u);
    }
    it("bounds host pair storage even when no canonical handles are occupied") {
        unsigned i;
        turbowasm_component_endpoint *reader = NULL, *writer = NULL;
        for (i = 0u; i < HOST_PAIR_LIMIT; ++i) {
            check_equal(turbowasm_component_endpoint_domain_pair_open(&domain, &graph, i % 2u, false,
                &host_readers[i], &host_writers[i]), TURBOWASM_OK);
            check_null(host_readers[i]->waitable.table); check_null(host_writers[i]->waitable.table);
        }
        check_equal(domain.pair_count, (uint32_t)HOST_PAIR_LIMIT); check_equal(table.live_count, 0u);
        check_equal(turbowasm_component_endpoint_domain_pair_open(&domain, &graph, 0u, false,
            &reader, &writer), TURBOWASM_OUT_OF_MEMORY);
        check_null(reader); check_null(writer);
        check_equal(start(1u, "snew", 0u, 0u, 0u), TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_task_domain_destroy(&domain), TURBOWASM_TRAPPED);
        close_host_pair(0u); close_pair(run("snew", 0u, 0u, 0u), false);
        check_equal(domain.pair_count, (uint32_t)HOST_PAIR_LIMIT - 1u);
    }
    it("publishes no host outputs on invalid input or allocation failure and reuses closed storage") {
        size_t baseline = live;
        allowance = 0u;
        check_equal(turbowasm_component_endpoint_domain_pair_open(&domain, &graph, 3u, false,
            &host_readers[0], &host_writers[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_endpoint_domain_pair_open(&domain, &graph, 0u, false,
            &host_readers[0], &host_readers[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_endpoint_domain_pair_open(&domain, &graph, 0u, false,
            &host_readers[0], &host_writers[0]), TURBOWASM_OUT_OF_MEMORY);
        check_null(host_readers[0]); check_null(host_writers[0]);
        check_equal(domain.pair_count, 0u); check_equal(table.live_count, 0u); check_equal(live, baseline);
        allowance = SIZE_MAX;
        check_equal(turbowasm_component_endpoint_domain_pair_open(&domain, &graph, 0u, false,
            &host_readers[0], &host_writers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_domain_pair_open(&domain, &graph, 0u, false,
            &host_readers[0], &host_writers[0]), TURBOWASM_INVALID_ARGUMENT);
        close_host_pair(0u); check_equal(domain.pair_count, 0u); check_equal(live, baseline);
    }
    it("moves a domain-owned host reader to Core while its host writer completes the rendezvous") {
        unsigned wide;
        for (wide = 0u; wide < 2u; ++wide) {
            turbowasm_component_event event;
            uint32_t handle = 0u;
            check_equal(turbowasm_component_endpoint_domain_pair_open(&domain, &graph, 0u, false,
                &host_readers[0], &host_writers[0]), TURBOWASM_OK);
            host_number.kind = TURBOWASM_COMPONENT_TYPE_U32; host_number.as.u32 = 42u + wide;
            host_buffer.values = &host_number; host_buffer.length = 1u;
            check_equal(turbowasm_component_endpoint_submit(host_writers[0], &host_buffer), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_into_value(host_readers[0], &host_value), TURBOWASM_OK);
            host_codec.table = &table;
            check_equal(turbowasm_component_endpoint_codec_lower(&host_codec, &graph,
                turbowasm_component_type_ref_indexed(0u), &host_value, &handle), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_codec_commit(&host_codec), TURBOWASM_OK);
            check_equal(turbowasm_component_value_destroy(&host_value), TURBOWASM_OK);
            check_equal(domain.pair_count, 1u); check_equal(table.live_count, 1u);
            check_equal(run(wide ? "sread64" : "sread", handle, 128u, 1u), 16u);
            check_equal(load_u32(wide, 128u), 42u + wide);
            check_equal(turbowasm_component_endpoint_take(host_writers[0], &event), TURBOWASM_OK);
            check_equal(event.payload, 16u); check_false(host_buffer.leased);
            check_equal(turbowasm_component_endpoint_close(host_writers[0]), TURBOWASM_OK);
            /* Guest drop may collect the stable pair; discard borrowed pointers first. */
            host_readers[0] = host_writers[0] = NULL;
            run("sdropr", handle, 0u, 0u);
            check_equal(domain.pair_count, 0u); check_equal(table.live_count, 0u);
            memset(&host_buffer, 0, sizeof(host_buffer));
        }
    }
    it("copies streams across memory widths and retains async descriptors through set delivery") {
        unsigned wide;
        for (wide = 0; wide < 2; ++wide) {
            uint64_t pair = run("snew", 0, 0, 0); uint32_t reader = (uint32_t)pair, writer = (uint32_t)(pair >> 32);
            turbowasm_component_event event;
            check_equal(turbowasm_component_waitable_set_register(&table, &set), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_join(&table, reader, set.handle), TURBOWASM_OK);
            store_u32(1 - wide, 32, 42); store_u32(1 - wide, 36, 77);
            check_equal(run(wide ? "sread64" : "sread", reader, 128, 2), UINT32_MAX);
            check_not_null(endpoint(reader)->operation); check_true(endpoint(reader)->guest_buffer.leased);
            check_equal(run(wide ? "swrite" : "swrite64", writer, 32, 2), 32u);
            check_equal(load_u32(wide, 128), 42u); check_equal(load_u32(wide, 132), 77u);
            check_equal(turbowasm_component_waitable_set_poll(&table, set.handle, &event), TURBOWASM_OK);
            check_equal(event.code, TURBOWASM_COMPONENT_EVENT_STREAM_READ); check_equal(event.payload, 32u);
            check_null(endpoint(reader)->operation); check_null(endpoint(reader)->guest_buffer.guest.memory.instance);
            close_pair(pair, false);
            check_equal(turbowasm_component_waitable_set_drop(&table, set.handle), TURBOWASM_OK);
        }
    }
    it("pins synchronous reads until a later writer completes without replay") {
        uint64_t pair = run("snew", 0, 0, 0); uint32_t reader = (uint32_t)pair, writer = (uint32_t)(pair >> 32);
        turbowasm_component_event event; unsigned before;
        store_u32(0, 32, 123); before = returns;
        check_equal(start(0, "sreadsync", reader, 128, 1), TURBOWASM_YIELDED);
        check_true(endpoint(reader)->waitable.sync_waiter);
        check_equal(turbowasm_component_waitable_take(&table, reader, &event), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_YIELDED);
        check_equal(returns, before);
        check_equal(run("swrite", writer, 32, 1), 16u);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(result(0), 16u); check_equal(load_u32(0, 128), 123u);
        check_false(endpoint(reader)->waitable.sync_waiter); compiled("sreadsync"); close_pair(pair, false);
    }
    it("unwinds synchronous copy waits and returns the guest memory borrow") {
        uint64_t pair = run("snew", 0, 0, 0); uint32_t reader = (uint32_t)pair;
        check_equal(start(0, "sreadsync", reader, 128, 1), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_destroy(&tasks[0]), TURBOWASM_OK);
        check_null(endpoint(reader)->operation); check_false(endpoint(reader)->waitable.sync_waiter);
        check_equal(endpoint(reader)->waitable.state.endpoint.phase, TURBOWASM_COMPONENT_ENDPOINT_IDLE);
        check_equal(run("sread", reader, 128, 1), UINT32_MAX); check_equal(run("scancelr", reader, 0, 0), 2u);
        close_pair(pair, false);
    }
    it("cancels async reads and writes and rejects repeated cancellation") {
        unsigned future, write;
        for (future = 0; future < 2; ++future) for (write = 0; write < 2; ++write) {
            uint64_t pair = run(future ? "fnew" : "snew", 0, 0, 0);
            uint32_t handle = write ? (uint32_t)(pair >> 32) : (uint32_t)pair;
            const char *cancel = future ? (write ? "fcancelw" : "fcancelr") : (write ? "scancelw" : "scancelr");
            check_equal(run(future ? (write ? "fwrite" : "fread") : (write ? "swrite" : "sread"), handle, 32, 1), UINT32_MAX);
            check_equal(run(cancel, handle, 0, 0), 2u); check_null(endpoint(handle)->operation);
            check_equal(start(1, cancel, handle, 0, 0), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
            if (future) {
                run("fdropr", (uint32_t)pair, 0, 0);
                check_equal(take((uint32_t)(pair >> 32)), 1u);
                run("fdropw", pair >> 32, 0, 0);
            } else close_pair(pair, false);
        }
    }
    it("completes futures once and forbids dropping an unwritten writable future") {
        unsigned wide;
        for (wide = 0; wide < 2; ++wide) {
            uint64_t pair = run("fnew", 0, 0, 0); uint32_t reader = (uint32_t)pair, writer = (uint32_t)(pair >> 32);
            check_equal(start(1, "fdropw", writer, 0, 0), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
            store_u32(wide, 32, 42);
            check_equal(run(wide ? "fwrite64" : "fwrite", writer, 32, 0), UINT32_MAX);
            check_equal(run(wide ? "fread" : "fread64", reader, 128, 0), 0u);
            check_equal(take(writer), 0u); check_equal(load_u32(1 - wide, 128), 42u);
            check_equal(start(1, "fwrite", writer, 32, 0), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
            close_pair(pair, true);
        }
    }
    it("checks endpoint identity, family, payload and full guest ranges before admission") {
        uint64_t pair = run("snew", 0, 0, 0); uint32_t reader = (uint32_t)pair, writer = (uint32_t)(pair >> 32);
        const char *entries[] = {"sread","fread","textread","sread","sread64","sread64"};
        uint64_t handles[] = {writer,reader,reader,reader,reader,reader};
        uint64_t pointers[] = {128,128,128,65536,UINT64_MAX,0};
        uint64_t lengths[] = {1,1,1,1,1,UINT64_MAX}; unsigned i;
        for (i = 0; i < 6; ++i) {
            check_equal(start(1, entries[i], handles[i], pointers[i], lengths[i]), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
            check_null(endpoint(reader)->operation); check_equal(endpoint(reader)->waitable.state.endpoint.phase, TURBOWASM_COMPONENT_ENDPOINT_IDLE);
        }
        check_equal(turbowasm_component_waitable_set_register(&table, &set), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&table, reader, set.handle), TURBOWASM_OK);
        check_equal(start(1, "sreadsync", reader, 128, 1), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK); check_null(endpoint(reader)->operation);
        close_pair(pair, false);
    }
    it("forwards stream and future peers while consuming the intermediate handles") {
        unsigned future;
        for (future = 0; future < 2; ++future) {
            uint64_t a = run(future ? "fnew" : "snew", 0, 0, 0), b = run(future ? "fnew" : "snew", 0, 0, 0);
            uint32_t ar = (uint32_t)a, aw = (uint32_t)(a >> 32), br = (uint32_t)b, bw = (uint32_t)(b >> 32);
            run(future ? "fforward" : "sforward", ar, bw, 0); check_null(endpoint(ar)); check_null(endpoint(bw));
            store_u32(0, 32, 88);
            check_equal(run(future ? "fread" : "sread", br, 128, 1), UINT32_MAX);
            check_equal(run(future ? "fwrite" : "swrite", aw, 32, 1), future ? 0u : 16u);
            check_equal(take(br), future ? 0u : 16u); check_equal(load_u32(0, 128), 88u);
            run(future ? "fdropr" : "sdropr", br, 0, 0); run(future ? "fdropw" : "sdropw", aw, 0, 0);
            check_equal(domain.pair_count, 0u);
        }
    }
    it("bounds pair storage even after all local readable handles have moved out") {
        turbowasm_component_endpoint *retained[2]; unsigned i;
        turbowasm_component_resource_table_destroy(&table);
        check_true(turbowasm_component_resource_table_init(&table, 2));
        for (i = 0; i < 2; ++i) {
            uint64_t pair = run("snew", 0, 0, 0);
            retained[i] = endpoint((uint32_t)pair);
            check_equal(turbowasm_component_endpoint_detach_readable(retained[i]), TURBOWASM_OK);
            run("sdropw", pair >> 32, 0, 0);
            check_equal(table.live_count, 0u);
        }
        check_equal(domain.pair_count, 2u);
        check_equal(start(1, "snew", 0, 0, 0), TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_task_domain_destroy(&domain), TURBOWASM_TRAPPED);
        for (i = 0; i < 2; ++i) check_equal(turbowasm_component_endpoint_close(retained[i]), TURBOWASM_OK);
        turbowasm_component_endpoint_domain_collect(&domain); check_equal(domain.pair_count, 0u);
        close_pair(run("snew", 0, 0, 0), false);
    }
    it("retains committed new effects across later Core OOM and cleans all owners") {
        size_t baseline, limit; bool success = false;
        close_pair(run("snew", 0, 0, 0), false); baseline = live;
        for (limit = 0; limit < 80; ++limit) {
            turbowasm_status status; allowance = limit;
            status = start(1, "snew", 0, 0, 0); allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) { close_pair(result(1), false); success = true; }
            else {
                unsigned i;
                check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
                /* A later task.return allocation may fail after new committed.
                 * The instance still owns both handles; never undo that effect. */
                check_true(table.live_count == 0 || table.live_count == 2);
                for (i = 0; i < table.capacity; ++i) {
                    uint32_t handle; turbowasm_component_handle_kind kind; void *object;
                    if (turbowasm_component_handle_at(&table, i, &handle, &kind, &object))
                        check_equal(turbowasm_component_endpoint_close(endpoint(handle)), TURBOWASM_OK);
                }
                turbowasm_component_endpoint_domain_collect(&domain);
            }
            check_equal(table.live_count, 0u); check_equal(domain.pair_count, 0u); check_equal(live, baseline);
            if (success) break;
        }
        check_true(success);
    }
    it("publishes neither half of a pair when new cannot allocate its handle table") {
        size_t limit; bool success = false;
        for (limit = 0; limit < 5; ++limit) {
            turbowasm_value output = {0}; size_t count = 99; turbowasm_trap trap = TURBOWASM_TRAP_NONE;
            turbowasm_status status;
            output.kind = TURBOWASM_VALUE_I64; output.as.i64 = 123;
            domain.active = &tasks[2]; allowance = limit;
            status = turbowasm_component_task_builtin_invoke(&builtins[0], NULL, NULL, 0, &output, 1, &count, &trap);
            allowance = SIZE_MAX; domain.active = NULL;
            if (status == TURBOWASM_OK) {
                check_equal(count, 1u); check_equal(table.live_count, 2u);
                close_pair((uint64_t)output.as.i64, false); success = true; break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(output.as.i64, 123); check_equal(count, 99u);
            check_equal(table.live_count, 0u); check_equal(domain.pair_count, 0u);
        }
        check_true(success);
    }
    it("ignores unit payload addresses and still delivers copy counts") {
        uint32_t reader, writer;
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 4, &table, &table,
            &external_reader, &external_writer), TURBOWASM_OK);
        reader = external_reader.waitable.handle; writer = external_writer.waitable.handle;
        check_equal(run("unitread", reader, UINT32_MAX, 2), UINT32_MAX);
        check_equal(run("unitwrite", writer, UINT32_MAX, 2), 32u);
        check_equal(take(reader), 32u); check_null(external_reader.operation);
    }
    it("pins both conversion and synchronous waiting while a host string is lowered") {
        turbowasm_component_buffer source = {0}; turbowasm_component_value string = {0}, output = {0};
        turbowasm_component_event event;
        unsigned i;
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 2, &table, NULL,
            &external_reader, &external_writer), TURBOWASM_OK);
        /* Bind the same signature as synchronous before invocation; the buffer
         * must capture its own resolved options until the host provides data. */
        for (i = 0; i < BUILTIN_COUNT; ++i)
            if (strcmp(definitions[i].name, "textread") == 0) builtins[i].definition.is_async = false;
        check_equal(start(0, "textread", external_reader.waitable.handle, 128, 1), TURBOWASM_YIELDED);
        string.kind = TURBOWASM_COMPONENT_TYPE_STRING; string.as.string.size = 5;
        string.as.string.data = turbowasm_rt_malloc(5); check_not_null(string.as.string.data);
        memcpy(string.as.string.data, "hello", 5);
        source.values = &string; source.length = 1;
        probe_copy_reentry = true;
        check_equal(turbowasm_component_endpoint_submit(&external_writer, &source), TURBOWASM_OK);
        check_equal(reallocations, 1u); check_equal(string.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
        check_equal(result(0), 16u); check_false(external_reader.waitable.sync_waiter);
        check_equal(turbowasm_component_endpoint_take(&external_writer, &event), TURBOWASM_OK); check_false(source.leased);
        check_equal(turbowasm_component_canonical_lift_value(&graph,
            turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_STRING), &memories[0], 128, &output), TURBOWASM_OK);
        check_equal(output.as.string.size, 5u); check_equal(output.as.string.data, "hello", 5u);
        check_equal(turbowasm_component_value_destroy(&output), TURBOWASM_OK); compiled("textread");
    }
    it("preserves captured guest options and releases both buffers on copy failure") {
        turbowasm_component_buffer source = {0}; turbowasm_component_value string = {0};
        turbowasm_component_event event;
        unsigned i;
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 2, &table, NULL,
            &external_reader, &external_writer), TURBOWASM_OK);
        check_equal(run("textread", external_reader.waitable.handle, 128, 1), UINT32_MAX);
        for (i = 0; i < BUILTIN_COUNT; ++i)
            if (strcmp(definitions[i].name, "textread") == 0) builtins[i].memory.guest_realloc = NULL;
        string.kind = TURBOWASM_COMPONENT_TYPE_STRING; string.as.string.size = 5;
        string.as.string.data = turbowasm_rt_malloc(5); check_not_null(string.as.string.data);
        memcpy(string.as.string.data, "hello", 5); source.values = &string; source.length = 1;
        /* Fail the captured guest allocator after both regions have been lent,
         * then require error delivery on each end. */
        fail_realloc = true;
        check_equal(turbowasm_component_endpoint_submit(&external_writer, &source), TURBOWASM_OK);
        check_equal(reallocations, 1u);
        check_equal(turbowasm_component_endpoint_take(&external_reader, &event), TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_endpoint_take(&external_writer, &event), TURBOWASM_OUT_OF_MEMORY);
        check_null(external_reader.operation); check_false(source.leased);
        check_equal(turbowasm_component_value_destroy(&string), TURBOWASM_OK);
    }
    it("synchronously writes streams and reads and writes futures through retained waits") {
        unsigned mode;
        for (mode = 0; mode < 3; ++mode) {
            bool future = mode != 0, read = mode == 1;
            uint64_t pair = run(future ? "fnew" : "snew", 0, 0, 0);
            uint32_t reader = (uint32_t)pair, writer = (uint32_t)(pair >> 32);
            const char *entry = mode == 0 ? "swritesync" : read ? "freadsync" : "fwritesync";
            store_u32(0, 32, 42);
            check_equal(start(0, entry, read ? reader : writer, read ? 128 : 32, 1), TURBOWASM_YIELDED);
            check_equal(run(future ? (read ? "fwrite" : "fread") : "sread", read ? writer : reader, read ? 32 : 128, 1), future ? 0u : 16u);
            check_equal(turbowasm_component_task_resume(&tasks[0], NULL), TURBOWASM_OK);
            check_equal(result(0), future ? 0u : 16u); check_equal(load_u32(0, 128), 42u);
            compiled(entry); close_pair(pair, future);
        }
    }
    it("rejects forged endpoint waitables and rolls back a half-registered new pair") {
        turbowasm_component_waitable fake = {0};
        check_equal(turbowasm_component_waitable_register(&table, TURBOWASM_COMPONENT_HANDLE_STREAM_READ, &fake), TURBOWASM_OK);
        check_equal(start(1, "sread", fake.handle, 128, 1), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_drop(&table, fake.handle), TURBOWASM_OK);
        turbowasm_component_resource_table_destroy(&table);
        check_true(turbowasm_component_resource_table_init(&table, 1));
        check_equal(start(1, "snew", 0, 0, 0), TURBOWASM_OUT_OF_MEMORY);
        check_equal(turbowasm_component_task_destroy(&tasks[1]), TURBOWASM_OK);
        check_equal(table.live_count, 0u); check_equal(domain.pair_count, 0u);
        turbowasm_component_resource_table_destroy(&table);
        check_true(turbowasm_component_resource_table_init(&table, 16));
        close_pair(run("snew", 0, 0, 0), false);
    }
}
