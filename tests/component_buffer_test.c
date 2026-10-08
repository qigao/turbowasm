#include "component_endpoint.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include <turbowasm/turbowasm.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t wasm32[] = {0,0x61,0x73,0x6d,1,0,0,0,5,3,1,0,1};
static const uint8_t wasm64[] = {0,0x61,0x73,0x6d,1,0,0,0,5,3,1,4,1};
static turbowasm_module modules[2];
static turbowasm_instance instances[2];
static turbowasm_component_type_graph graph;
static turbowasm_component_type_ref payload;
static turbowasm_component_endpoint reader, writer, children[2][2];
static turbowasm_component_resource_table tables[2];
static turbowasm_component_endpoint_codec codecs[2];
static turbowasm_component_canonical_memory memories[2];
static turbowasm_component_waitable_set set;
static turbowasm_component_buffer buffers[4];
static turbowasm_component_value cells[3][4];
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t allocations, allowance;
static uint64_t cursors[2];
static unsigned memory_ids[2] = {0u, 1u};
static bool probe_reentry, reentry_ok;
static unsigned commits, rollbacks, resource_drops;
static uint32_t staged_resource;

static void *allocate(void *context, size_t size) {
    void *pointer; (void)context;
    if (allowance == 0u) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    pointer = malloc(size); if (pointer != NULL) ++allocations;
    return pointer;
}
static void deallocate(void *context, void *pointer) {
    (void)context; if (pointer != NULL) --allocations; free(pointer);
}
static void finish_end(turbowasm_component_endpoint *end) {
    turbowasm_component_event event;
    if (!end->initialized || end->closed) return;
    if (end->operation != NULL) {
        if (end->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
            (void)turbowasm_component_endpoint_cancel(end);
        (void)turbowasm_component_endpoint_take(end, &event);
    }
    (void)turbowasm_component_endpoint_close(end);
}
static void cleanup(void) {
    unsigned i, j;
    allowance = SIZE_MAX;
    for (i = 0u; i < 2u; ++i)
        if (codecs[i].table != NULL) (void)turbowasm_component_endpoint_codec_rollback(&codecs[i]);
    finish_end(&reader); finish_end(&writer);
    for (i = 0u; i < 3u; ++i)
        for (j = 0u; j < 4u; ++j) (void)turbowasm_component_value_destroy(&cells[i][j]);
    for (i = 0u; i < 2u; ++i) {
        finish_end(&children[i][0]); finish_end(&children[i][1]);
    }
    if (set.table != NULL) (void)turbowasm_component_waitable_set_drop(set.table, set.handle);
    for (i = 0u; i < 2u; ++i) {
        turbowasm_component_resource_table_destroy(&tables[i]);
        turbowasm_instance_destroy(&instances[i]); turbowasm_module_destroy(&modules[i]);
    }
    turbowasm_component_type_graph_destroy(&graph);
    memset(&reader, 0, sizeof(reader)); memset(&writer, 0, sizeof(writer));
    memset(children, 0, sizeof(children)); memset(codecs, 0, sizeof(codecs));
    memset(memories, 0, sizeof(memories)); memset(buffers, 0, sizeof(buffers)); memset(&set, 0, sizeof(set));
    probe_reentry = false;
}
static uint8_t *bytes_at(unsigned memory, uint64_t address, size_t length) {
    uint8_t *bytes = NULL;
    check_equal(turbowasm_instance_memory_bounds(instances[memory].impl, 0u, address, 0u, length, &bytes), TURBOWASM_OK);
    return bytes;
}
static uint32_t word(unsigned memory, uint64_t address) {
    uint8_t *p = bytes_at(memory, address, 4u);
    return (uint32_t)p[0] | (uint32_t)p[1] << 8u | (uint32_t)p[2] << 16u | (uint32_t)p[3] << 24u;
}
static void put(unsigned memory, uint64_t address, uint32_t value) {
    unsigned i; uint8_t *p = bytes_at(memory, address, 4u);
    for (i = 0u; i < 4u; ++i) p[i] = (uint8_t)(value >> (i * 8u));
}
static turbowasm_status guest_realloc(void *context, uint64_t old_pointer,
    uint64_t old_size, uint64_t alignment, uint64_t size, uint64_t *out) {
    unsigned index = *(unsigned *)context;
    uint64_t pointer;
    uint8_t *bytes;
    turbowasm_status status;
    if (probe_reentry && index == 1u) {
        uint64_t previous;
        turbowasm_component_event event;
        reentry_ok = turbowasm_component_endpoint_take(&writer, &event) == TURBOWASM_TRAPPED &&
            turbowasm_component_endpoint_take(&reader, &event) == TURBOWASM_TRAPPED &&
            turbowasm_component_endpoint_close(&reader) == TURBOWASM_TRAPPED &&
            turbowasm_component_endpoint_cancel(&writer) == TURBOWASM_TRAPPED;
        if (!reentry_ok) return TURBOWASM_TRAPPED;
        status = turbowasm_instance_memory_grow64(instances[index].impl, 0u, 1u, &previous);
        if (status != TURBOWASM_OK) return status;
        cursors[index] = previous * UINT64_C(65536);
        probe_reentry = false;
    }
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        cursors[index] > UINT64_MAX - (alignment - 1u)) return TURBOWASM_TRAPPED;
    pointer = (cursors[index] + alignment - 1u) & ~(alignment - 1u);
    if (size > SIZE_MAX || pointer > UINT64_MAX - size) return TURBOWASM_TRAPPED;
    status = turbowasm_instance_memory_bounds(instances[index].impl, 0u, pointer, 0u, (size_t)size, &bytes);
    if (status != TURBOWASM_OK) return status;
    if (old_pointer != 0u && old_size != 0u) {
        uint8_t *old_bytes;
        uint64_t copy_size = old_size < size ? old_size : size;
        status = turbowasm_instance_memory_bounds(instances[index].impl, 0u,
            old_pointer, 0u, (size_t)copy_size, &old_bytes);
        if (status != TURBOWASM_OK) return status;
        memmove(bytes, old_bytes, (size_t)copy_size);
    }
    cursors[index] = pointer + size; *out = pointer;
    return status;
}
static turbowasm_status commit_endpoints(void *context, turbowasm_component_value *values, uint32_t count) {
    (void)values; (void)count; ++commits;
    return turbowasm_component_endpoint_codec_commit(context);
}
static turbowasm_status rollback_endpoints(void *context) {
    ++rollbacks; return turbowasm_component_endpoint_codec_rollback(context);
}
static turbowasm_status reject_commit(void *context, turbowasm_component_value *values, uint32_t count) {
    (void)context; (void)values; (void)count; ++commits;
    return TURBOWASM_TRAPPED;
}
static void initialize(turbowasm_component_type_kind element, bool wide_source,
    bool unit, bool future, uint32_t destination_capacity) {
    unsigned i;
    check_true(turbowasm_component_type_graph_allocate(&graph, 3u));
    if (element == TURBOWASM_COMPONENT_TYPE_FUTURE)
        check_true(turbowasm_component_type_graph_define_async_value(&graph, 0u, element, true,
            turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32)));
    else if (element == TURBOWASM_COMPONENT_TYPE_STRING)
        check_true(turbowasm_component_type_graph_define_string(&graph, 0u));
    else if (element == TURBOWASM_COMPONENT_TYPE_OWN)
        check_true(turbowasm_component_type_graph_define_resource(&graph, 0u, 42u));
    else check_true(turbowasm_component_type_graph_define_scalar(&graph, 0u, element));
    if (element == TURBOWASM_COMPONENT_TYPE_OWN)
        check_true(turbowasm_component_type_graph_define_handle(&graph, 1u, element, 0u));
    else check_true(turbowasm_component_type_graph_define_scalar(&graph, 1u, TURBOWASM_COMPONENT_TYPE_U8));
    payload = turbowasm_component_type_ref_indexed(element == TURBOWASM_COMPONENT_TYPE_OWN ? 1u : 0u);
    check_true(turbowasm_component_type_graph_define_async_value(&graph, 2u,
        future ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM, !unit, payload));
    for (i = 0u; i < 2u; ++i) {
        bool wide = i == 0u ? wide_source : !wide_source;
        check_equal(turbowasm_module_load_borrowed_with_config(&modules[i], wide ? wasm64 : wasm32,
            sizeof(wasm32), &config), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&instances[i], &modules[i]), TURBOWASM_OK);
        check_true(turbowasm_component_resource_table_init(&tables[i], i == 1u ? destination_capacity : 8u));
        codecs[i].table = &tables[i];
        memories[i].instance = &instances[i];
        memories[i].pointer_type = wide ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
        memories[i].guest_realloc = guest_realloc; memories[i].realloc_context = &memory_ids[i];
        memories[i].endpoint_lift = turbowasm_component_endpoint_codec_lift;
        memories[i].endpoint_lower = turbowasm_component_endpoint_codec_lower;
        memories[i].endpoint_context = &codecs[i]; cursors[i] = 128u;
    }
    check_equal(turbowasm_component_endpoint_pair_open(&graph, 2u, NULL, NULL, &reader, &writer), TURBOWASM_OK);
}
static void host(unsigned buffer, unsigned row, uint32_t length) {
    buffers[buffer].values = cells[row]; buffers[buffer].length = length;
}
static void guest(unsigned buffer, unsigned memory, uint64_t address, uint32_t length) {
    buffers[buffer].kind = TURBOWASM_COMPONENT_BUFFER_GUEST; buffers[buffer].length = length;
    buffers[buffer].guest.graph = &graph; buffers[buffer].guest.type = payload;
    buffers[buffer].guest.memory = memories[memory]; buffers[buffer].guest.address = address;
    buffers[buffer].guest.commit = commit_endpoints;
    buffers[buffer].guest.rollback = rollback_endpoints; buffers[buffer].guest.context = &codecs[memory];
}
static uint32_t take(turbowasm_component_endpoint *end) {
    turbowasm_component_event event;
    check_equal(turbowasm_component_endpoint_take(end, &event), TURBOWASM_OK); return event.payload;
}
static void pair_copy(bool read_first) {
    check_equal(turbowasm_component_endpoint_submit(read_first ? &reader : &writer,
        &buffers[read_first ? 1u : 0u]), TURBOWASM_OK);
    check_equal(turbowasm_component_endpoint_submit(read_first ? &writer : &reader,
        &buffers[read_first ? 0u : 1u]), TURBOWASM_OK);
}
static void errors(turbowasm_status expected) {
    turbowasm_component_event event = {TURBOWASM_COMPONENT_EVENT_TASK_CANCELLED, 42u, 99u};
    check_equal(turbowasm_component_endpoint_take(&reader, &event), expected);
    check_equal(event.code, TURBOWASM_COMPONENT_EVENT_TASK_CANCELLED); check_equal(event.payload, 99u);
    check_equal(turbowasm_component_endpoint_take(&writer, &event), expected);
    check_false(buffers[0].leased); check_false(buffers[1].leased);
    check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), expected);
}

static turbowasm_status release_resource(void *context) {
    (void)context; ++resource_drops; return TURBOWASM_OK;
}
static turbowasm_status lower_resource(void *context, const turbowasm_component_type_graph *types,
    turbowasm_component_type_ref type, const turbowasm_component_value *value, uint32_t *out) {
    turbowasm_status status;
    (void)types; (void)type;
    if (value->kind != TURBOWASM_COMPONENT_TYPE_OWN || value->resource_identity != 42u || staged_resource != 0u)
        return TURBOWASM_TYPE_MISMATCH;
    status = turbowasm_component_resource_new_owned(context, 42u, value->as.resource_rep, out);
    if (status == TURBOWASM_OK) staged_resource = *out;
    return status;
}
static turbowasm_status lift_resource(void *context, const turbowasm_component_type_graph *types,
    turbowasm_component_type_ref type, uint32_t handle, turbowasm_component_value *out) {
    turbowasm_value rep;
    turbowasm_status status;
    (void)types; (void)type;
    status = turbowasm_component_resource_take_owned(context, handle, 42u, &rep);
    if (status != TURBOWASM_OK) return status;
    out->kind = TURBOWASM_COMPONENT_TYPE_OWN; out->as.resource_rep = rep;
    out->resource_identity = 42u; out->release = release_resource;
    return TURBOWASM_OK;
}
static turbowasm_status commit_resource(void *context, turbowasm_component_value *values, uint32_t count) {
    (void)context;
    if (count != 1u || values[0].kind != TURBOWASM_COMPONENT_TYPE_OWN || staged_resource == 0u)
        return TURBOWASM_TRAPPED;
    values[0].release = NULL; values[0].release_context = NULL;
    staged_resource = 0u; ++commits; return TURBOWASM_OK;
}
static turbowasm_status rollback_resource(void *context) {
    turbowasm_value rep;
    turbowasm_status status = TURBOWASM_OK;
    if (staged_resource != 0u)
        status = turbowasm_component_resource_take_owned(context, staged_resource, 42u, &rep);
    staged_resource = 0u; ++rollbacks; return status;
}

spec("Component host and guest buffer rendezvous") {
    before_each() {
        allocations = 0u; allowance = SIZE_MAX; commits = rollbacks = resource_drops = 0u;
        staged_resource = 0u; probe_reentry = reentry_ok = false;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
    }
    after_each() { cleanup(); turbowasm_runtime_scope_leave(scope); check_equal(allocations, 0u); }

    it("forwards pending guest copies and delivers conversion failure after consuming intermediates") {
        unsigned fail;
        for (fail = 0u; fail < 2u; ++fail) {
            turbowasm_component_event event;
            initialize(TURBOWASM_COMPONENT_TYPE_U32, fail != 0u, false, false, 8u);
            check_equal(turbowasm_component_endpoint_pair_open(&graph, 2u, NULL, NULL,
                &children[0][0], &children[0][1]), TURBOWASM_OK);
            put(0u, 16u, 42u); guest(0u, 0u, 16u, 1u); host(1u, 1u, 1u);
            check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_submit(&children[0][0], &buffers[1]), TURBOWASM_OK);
            if (fail) allowance = 0u;
            check_equal(turbowasm_component_endpoint_forward(&reader, &children[0][1]),
                fail ? TURBOWASM_OUT_OF_MEMORY : TURBOWASM_OK);
            allowance = SIZE_MAX;
            check_true(reader.closed); check_true(children[0][1].closed);
            check_true(buffers[0].leased); check_true(buffers[1].leased);
            if (fail) {
                check_equal(turbowasm_component_endpoint_take(&writer, &event), TURBOWASM_OUT_OF_MEMORY);
                check_equal(turbowasm_component_endpoint_take(&children[0][0], &event), TURBOWASM_OUT_OF_MEMORY);
                check_equal(cells[1][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
            } else {
                check_equal(take(&writer), 16u); check_equal(take(&children[0][0]), 16u);
                check_equal(cells[1][0].as.u32, 42u);
            }
            check_false(buffers[0].leased); check_false(buffers[1].leased); cleanup();
        }
    }

    it("copies numeric values between host and memory32 or memory64 in both arrival orders") {
        unsigned source_guest, dest_guest, scenario, i;
        for (source_guest = 0u; source_guest < 2u; ++source_guest)
            for (dest_guest = 0u; dest_guest < 2u; ++dest_guest)
                for (scenario = 0u; scenario < 4u; ++scenario) {
                    initialize(TURBOWASM_COMPONENT_TYPE_U32, (scenario & 1u) != 0u, false, false, 8u);
                    for (i = 0u; i < 3u; ++i) {
                        cells[0][i].kind = TURBOWASM_COMPONENT_TYPE_U32; cells[0][i].as.u32 = 42u + i;
                        put(0u, 16u + 4u * i, 42u + i);
                    }
                    if (source_guest) guest(0u, 0u, 16u, 3u); else host(0u, 0u, 3u);
                    if (dest_guest) guest(1u, 1u, 16u, 3u); else host(1u, 1u, 3u);
                    pair_copy((scenario & 2u) != 0u);
                    check_equal(take(&reader), 48u); check_equal(take(&writer), 48u);
                    for (i = 0u; i < 3u; ++i)
                        check_equal(dest_guest ? word(1u, 16u + 4u * i) : cells[1][i].as.u32, 42u + i);
                    if (!source_guest) check_equal(cells[0][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
                    cleanup();
                }
    }

    it("snapshots overlapping guest numeric regions before any destination write") {
        unsigned width, i;
        for (width = 0u; width < 2u; ++width) {
            initialize(TURBOWASM_COMPONENT_TYPE_U32, width != 0u, false, false, 8u);
            for (i = 0u; i < 3u; ++i) put(0u, 16u + 4u * i, i + 1u);
            guest(0u, 0u, 16u, 3u); guest(1u, 0u, 20u, 3u);
            pair_copy(false); check_equal(take(&reader), 48u); check_equal(take(&writer), 48u);
            for (i = 0u; i < 3u; ++i) check_equal(word(0u, 20u + 4u * i), i + 1u);
            cleanup();
        }
    }

    it("rejects invalid guest ranges without changing an admitted peer operation") {
        static const uint64_t addresses[] = {1u, 65536u, UINT64_MAX, UINT64_C(0x100000000)};
        unsigned i;
        initialize(TURBOWASM_COMPONENT_TYPE_U32, true, false, false, 8u);
        cells[0][0].kind = TURBOWASM_COMPONENT_TYPE_U32; cells[0][0].as.u32 = 42u; host(0u, 0u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        for (i = 0u; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
            guest(1u, 1u, addresses[i], 1u);
            check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_TRAPPED);
            check_false(buffers[1].leased); check_true(buffers[0].leased);
            check_equal(writer.failure, TURBOWASM_OK);
        }
        guest(1u, 1u, 65532u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(take(&reader), 16u); check_equal(take(&writer), 16u); check_equal(word(1u, 65532u), 42u);
    }

    it("ignores guest pointers and options for unit and zero-length copies") {
        unsigned future;
        for (future = 0u; future < 2u; ++future) {
            uint32_t count = future ? 1u : TURBOWASM_COMPONENT_COPY_MAX_LENGTH;
            size_t baseline;
            initialize(TURBOWASM_COMPONENT_TYPE_U32, false, true, future != 0u, 8u);
            buffers[0].kind = buffers[1].kind = TURBOWASM_COMPONENT_BUFFER_GUEST;
            buffers[0].length = buffers[1].length = count;
            buffers[0].guest.address = buffers[1].guest.address = UINT64_MAX;
            baseline = allocations; pair_copy(false);
            check_equal(take(&reader), future ? 0u : count << 4u);
            check_equal(take(&writer), future ? 0u : count << 4u); check_equal(allocations, baseline);
            cleanup();
        }
        initialize(TURBOWASM_COMPONENT_TYPE_U32, false, false, false, 8u);
        buffers[0].kind = buffers[1].kind = TURBOWASM_COMPONENT_BUFFER_GUEST;
        buffers[0].guest.address = buffers[1].guest.address = UINT64_MAX;
        pair_copy(false); check_equal(take(&writer), 0u);
        check_true(buffers[1].leased);
        check_equal(turbowasm_component_endpoint_cancel(&reader), TURBOWASM_OK);
        check_equal(take(&reader), TURBOWASM_COMPONENT_COPY_CANCELLED);
    }

    it("accumulates guest source progress and reads bytes at rendezvous time") {
        unsigned i;
        initialize(TURBOWASM_COMPONENT_TYPE_U32, true, false, false, 8u);
        for (i = 0u; i < 4u; ++i) put(0u, 16u + i * 4u, i);
        guest(0u, 0u, 16u, 4u); host(1u, 1u, 2u); host(2u, 2u, 2u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        put(0u, 16u, 42u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(take(&reader), 32u); check_equal(cells[1][0].as.u32, 42u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[2]), TURBOWASM_OK);
        check_equal(take(&reader), 32u); check_equal(take(&writer), 64u);
        check_equal(cells[2][1].as.u32, 3u); check_equal(buffers[0].progress, 4u);
    }

    it("converts strings across memory widths and encodings while guarding realloc reentry") {
        unsigned width;
        for (width = 0u; width < 2u; ++width) {
            static const uint8_t utf8[] = {'A', 0xc3, 0xa9};
            static const uint8_t utf16[] = {'A', 0, 0xe9, 0};
            initialize(TURBOWASM_COMPONENT_TYPE_STRING, width != 0u, false, false, 8u);
            cells[0][0].kind = TURBOWASM_COMPONENT_TYPE_STRING;
            cells[0][0].as.string.data = turbowasm_rt_malloc(sizeof(utf8)); check_not_null(cells[0][0].as.string.data);
            memcpy(cells[0][0].as.string.data, utf8, sizeof(utf8)); cells[0][0].as.string.size = sizeof(utf8);
            check_equal(turbowasm_component_canonical_lower_value(&graph, payload, &memories[0], 16u, &cells[0][0]), TURBOWASM_OK);
            memories[1].string_encoding = TURBOWASM_COMPONENT_STRING_UTF16;
            guest(0u, 0u, 16u, 1u); guest(1u, 1u, 16u, 1u); probe_reentry = true;
            pair_copy(true); check_equal(take(&reader), 16u); check_equal(take(&writer), 16u);
            check_true(reentry_ok); check_greater_equal(word(1u, 16u), 65536u);
            check_equal(bytes_at(1u, word(1u, 16u), sizeof(utf16)), utf16, sizeof(utf16));
            check_equal(turbowasm_component_canonical_lift_value(&graph, payload, &memories[1], 16u, &cells[1][0]), TURBOWASM_OK);
            check_equal(cells[1][0].as.string.data, utf8, sizeof(utf8));
            cleanup();
        }
    }

    it("delivers snapshot allocation errors once to both sides without consuming source bytes") {
        turbowasm_status status;
        initialize(TURBOWASM_COMPONENT_TYPE_U32, false, false, false, 8u);
        put(0u, 16u, 42u); guest(0u, 0u, 16u, 1u); host(1u, 1u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        allowance = 0u; status = turbowasm_component_endpoint_submit(&writer, &buffers[0]); allowance = SIZE_MAX;
        check_equal(status, TURBOWASM_OK); check_true(buffers[0].leased); check_true(buffers[1].leased);
        errors(TURBOWASM_OUT_OF_MEMORY); check_equal(word(0u, 16u), 42u);
        check_equal(buffers[0].progress, 0u); check_equal(cells[1][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
    }

    it("commits nested endpoints as a batch or rolls back every reserved destination handle") {
        unsigned fail, i;
        for (fail = 0u; fail < 2u; ++fail) {
            turbowasm_component_event event = {TURBOWASM_COMPONENT_EVENT_TASK_CANCELLED, 42u, 99u};
            unsigned before_rollback = rollbacks;
            initialize(TURBOWASM_COMPONENT_TYPE_FUTURE, true, false, false, fail ? 3u : 8u);
            check_equal(turbowasm_component_endpoint_attach_readable(&reader, &tables[1]), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_set_register(&tables[1], &set), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_join(&tables[1], reader.waitable.handle, set.handle), TURBOWASM_OK);
            for (i = 0u; i < 2u; ++i) {
                check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, NULL, NULL,
                    &children[i][0], &children[i][1]), TURBOWASM_OK);
                check_equal(turbowasm_component_endpoint_into_value(&children[i][0], &cells[0][i]), TURBOWASM_OK);
            }
            host(0u, 0u, 2u); guest(1u, 1u, 16u, 2u); pair_copy(true);
            check_true(buffers[0].leased); check_true(buffers[1].leased);
            if (fail) {
                check_equal(rollbacks, before_rollback + 1u); check_null(codecs[1].lower_head);
                check_equal(tables[1].live_count, 2u);
                check_equal(turbowasm_component_waitable_set_poll(&tables[1], set.handle, &event), TURBOWASM_OUT_OF_MEMORY);
                check_equal(event.code, TURBOWASM_COMPONENT_EVENT_TASK_CANCELLED); check_equal(event.payload, 99u);
                check_equal(turbowasm_component_endpoint_take(&writer, &event), TURBOWASM_OUT_OF_MEMORY);
                check_equal(buffers[0].progress, 0u); check_equal(buffers[1].progress, 0u);
                check_not_null(children[0][0].value_owner); check_not_null(children[1][0].value_owner);
            } else {
                check_equal(turbowasm_component_waitable_set_poll(&tables[1], set.handle, &event), TURBOWASM_OK);
                check_equal(event.payload, 32u); check_equal(take(&writer), 32u);
                for (i = 0u; i < 2u; ++i) {
                    check_equal(cells[0][i].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
                    check_equal(turbowasm_component_canonical_lift_value(&graph, payload, &memories[1],
                        16u + i * 4u, &cells[1][i]), TURBOWASM_OK);
                    check_equal(turbowasm_component_value_destroy(&cells[1][i]), TURBOWASM_OK);
                    check_true(children[i][0].closed);
                }
            }
            check_false(buffers[0].leased); check_false(buffers[1].leased); cleanup();
        }
    }

    it("releases consumed guest owners when destination lowering or commit fails") {
        unsigned fail_commit, i;
        for (fail_commit = 0u; fail_commit < 2u; ++fail_commit) {
            unsigned before_rollback = rollbacks, before_commit = commits;
            initialize(TURBOWASM_COMPONENT_TYPE_FUTURE, fail_commit != 0u,
                false, false, fail_commit ? 8u : 1u);
            for (i = 0u; i < 2u; ++i) {
                check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], NULL,
                    &children[i][0], &children[i][1]), TURBOWASM_OK);
                put(0u, 16u + i * 4u, children[i][0].waitable.handle);
            }
            guest(0u, 0u, 16u, 2u); guest(1u, 1u, 16u, 2u);
            if (fail_commit) buffers[1].guest.commit = reject_commit;
            pair_copy(fail_commit != 0u);
            errors(fail_commit ? TURBOWASM_TRAPPED : TURBOWASM_OUT_OF_MEMORY);
            check_equal(buffers[0].progress, 2u); check_equal(buffers[1].progress, 0u);
            check_equal(rollbacks, before_rollback + 1u);
            check_equal(commits, before_commit + fail_commit);
            check_equal(tables[0].live_count, 0u); check_equal(tables[1].live_count, 0u);
            check_null(codecs[1].lower_head);
            for (i = 0u; i < 2u; ++i) {
                check_true(children[i][0].closed);
                check_null(children[i][0].value_owner);
                check_equal(take(&children[i][1]), TURBOWASM_COMPONENT_COPY_DROPPED);
            }
            cleanup();
        }
    }

    it("cleans up destructive guest lifts before publishing any host destination values") {
        initialize(TURBOWASM_COMPONENT_TYPE_FUTURE, false, false, false, 8u);
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], NULL,
            &children[0][0], &children[0][1]), TURBOWASM_OK);
        put(0u, 16u, children[0][0].waitable.handle); put(0u, 20u, 0u);
        guest(0u, 0u, 16u, 2u); host(1u, 1u, 2u); pair_copy(true);
        errors(TURBOWASM_TRAPPED);
        check_equal(tables[0].live_count, 0u); check_true(children[0][0].closed);
        check_equal(take(&children[0][1]), TURBOWASM_COMPONENT_COPY_DROPPED);
        check_equal(cells[1][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(cells[1][1].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
    }

    it("cancels an unmatched guest buffer without lifting its endpoint handles") {
        uint32_t handle;
        initialize(TURBOWASM_COMPONENT_TYPE_FUTURE, false, false, false, 8u);
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], NULL,
            &children[0][0], &children[0][1]), TURBOWASM_OK);
        handle = children[0][0].waitable.handle;
        put(0u, 16u, handle); guest(0u, 0u, 16u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_cancel(&writer), TURBOWASM_OK);
        check_true(buffers[0].leased); check_equal(take(&writer), TURBOWASM_COMPONENT_COPY_CANCELLED);
        check_false(buffers[0].leased); check_equal(children[0][0].waitable.handle, handle);
        check_equal(tables[0].live_count, 1u); check_false(children[0][0].closed);
    }

    it("transfers actual own resource handles without destroying committed representations") {
        uint32_t handle;
        initialize(TURBOWASM_COMPONENT_TYPE_OWN, true, false, false, 8u);
        memories[1].resource_lower = lower_resource; memories[1].resource_lift = lift_resource;
        memories[1].resource_context = &tables[1];
        cells[0][0].kind = TURBOWASM_COMPONENT_TYPE_OWN; cells[0][0].resource_identity = 42u;
        cells[0][0].as.resource_rep.kind = TURBOWASM_VALUE_I32; cells[0][0].as.resource_rep.as.i32 = 123;
        cells[0][0].release = release_resource;
        host(0u, 0u, 1u); guest(1u, 1u, 16u, 1u);
        buffers[1].guest.commit = commit_resource; buffers[1].guest.rollback = rollback_resource;
        buffers[1].guest.context = &tables[1];
        pair_copy(true); check_equal(take(&reader), 16u); check_equal(take(&writer), 16u);
        check_equal(cells[0][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED); check_equal(resource_drops, 0u);
        handle = word(1u, 16u); check_equal(tables[1].live_count, 1u);
        memset(buffers, 0, sizeof(buffers)); guest(0u, 1u, 16u, 1u); host(1u, 1u, 1u);
        pair_copy(false); check_equal(take(&reader), 16u); check_equal(take(&writer), 16u);
        check_equal(turbowasm_component_handle_kind_get(&tables[1], handle), TURBOWASM_COMPONENT_HANDLE_INVALID);
        check_equal(cells[1][0].as.resource_rep.as.i32, 123); check_equal(resource_drops, 0u);
        check_equal(turbowasm_component_value_destroy(&cells[1][0]), TURBOWASM_OK);
        check_equal(resource_drops, 1u);
    }
}
