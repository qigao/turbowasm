#include "component_endpoint.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static turbowasm_component_type_graph graph;
static turbowasm_component_type_graph value_graph;
static turbowasm_component_resource_table tables[2];
static turbowasm_component_waitable_set set;
static turbowasm_component_endpoint reader, writer;
static turbowasm_component_endpoint nested_reader, nested_writer;
static turbowasm_component_buffer buffers[6];
static turbowasm_component_value cells[3][8];
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t allocations;
static unsigned destroyed;
static bool refuse_allocations;

static void *allocate(void *context, size_t size) {
    void *pointer; (void)context;
    if (refuse_allocations) return NULL;
    pointer = malloc(size); if (pointer != NULL) ++allocations;
    return pointer;
}
static void deallocate(void *context, void *pointer) {
    (void)context;
    if (pointer != NULL) --allocations;
    free(pointer);
}
static turbowasm_status release_resource(void *context) {
    (void)context; ++destroyed; return TURBOWASM_OK;
}
static void cleanup(void) {
    turbowasm_component_endpoint *ends[4] = {&reader, &writer, &nested_reader, &nested_writer};
    unsigned i, j;
    for (i = 0u; i < 4u; ++i) {
        turbowasm_component_event event;
        if (!ends[i]->initialized || ends[i]->closed) continue;
        if (ends[i]->operation != NULL) {
            if (ends[i]->waitable.state.endpoint.phase == TURBOWASM_COMPONENT_ENDPOINT_COPYING)
                (void)turbowasm_component_endpoint_cancel(ends[i]);
            (void)turbowasm_component_endpoint_take(ends[i], &event);
        }
        (void)turbowasm_component_endpoint_close(ends[i]);
    }
    if (set.table != NULL)
        (void)turbowasm_component_waitable_set_drop(set.table, set.handle);
    for (i = 0u; i < 3u; ++i)
        for (j = 0u; j < 8u; ++j)
            (void)turbowasm_component_value_destroy(&cells[i][j]);
    turbowasm_component_resource_table_destroy(&tables[0]);
    turbowasm_component_resource_table_destroy(&tables[1]);
    turbowasm_component_type_graph_destroy(&graph);
    turbowasm_component_type_graph_destroy(&value_graph);
    memset(&reader, 0, sizeof(reader)); memset(&writer, 0, sizeof(writer));
    memset(&nested_reader, 0, sizeof(nested_reader)); memset(&nested_writer, 0, sizeof(nested_writer));
    memset(&set, 0, sizeof(set)); memset(buffers, 0, sizeof(buffers));
}
static void primitive_type(bool future, bool unit, turbowasm_component_type_kind kind) {
    check_true(turbowasm_component_type_graph_allocate(&graph, 1u));
    check_true(turbowasm_component_type_graph_define_async_value(&graph, 0u,
        future ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM,
        !unit, turbowasm_component_type_ref_inline(kind)));
}
static void open_hosts(void) {
    check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, NULL, NULL,
        &reader, &writer), TURBOWASM_OK);
}
static void numbers(unsigned row, unsigned count, uint32_t first) {
    unsigned i;
    for (i = 0u; i < count; ++i) {
        cells[row][i].kind = TURBOWASM_COMPONENT_TYPE_U32;
        cells[row][i].as.u32 = first + i;
    }
}
static void buffer(unsigned index, unsigned row, unsigned length) {
    buffers[index].values = cells[row]; buffers[index].length = length;
}
static uint32_t take(turbowasm_component_endpoint *endpoint) {
    turbowasm_component_event event;
    check_equal(turbowasm_component_endpoint_take(endpoint, &event), TURBOWASM_OK);
    return event.payload;
}
static void composite_type(void) {
    static const uint8_t own_name[] = "handle", text_name[] = "text";
    turbowasm_component_record_field fields[2] = {
        {own_name, 6u, {0}}, {text_name, 4u, {0}}
    };
    check_true(turbowasm_component_type_graph_allocate(&graph, 4u));
    check_true(turbowasm_component_type_graph_define_resource(&graph, 0u, 42u));
    check_true(turbowasm_component_type_graph_define_handle(&graph, 1u,
        TURBOWASM_COMPONENT_TYPE_OWN, 0u));
    fields[0].type = turbowasm_component_type_ref_indexed(1u);
    fields[1].type = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_STRING);
    check_true(turbowasm_component_type_graph_define_record(&graph, 2u, fields, 2u));
    check_true(turbowasm_component_type_graph_define_async_value(&graph, 3u,
        TURBOWASM_COMPONENT_TYPE_STREAM, true, turbowasm_component_type_ref_indexed(2u)));
}
static void composite_value(unsigned index) {
    turbowasm_component_value *value = &cells[0][index], *fields;
    value->kind = TURBOWASM_COMPONENT_TYPE_RECORD;
    fields = turbowasm_rt_calloc(2u, sizeof(*fields)); check_not_null(fields);
    value->as.record.items = fields; value->as.record.count = 2u;
    fields[0].kind = TURBOWASM_COMPONENT_TYPE_OWN;
    fields[0].as.resource_rep.kind = TURBOWASM_VALUE_I32;
    fields[0].as.resource_rep.as.i32 = (int32_t)index;
    fields[0].resource_identity = 42u; fields[0].release = release_resource;
    fields[1].kind = TURBOWASM_COMPONENT_TYPE_STRING;
    fields[1].as.string.data = turbowasm_rt_malloc(3u);
    check_not_null(fields[1].as.string.data);
    memcpy(fields[1].as.string.data, "abc", 3u); fields[1].as.string.size = 3u;
}

spec("Component endpoint host-value rendezvous") {
    before_each() {
        allocations = 0u; destroyed = 0u; refuse_allocations = false;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
    }
    after_each() {
        cleanup();
        turbowasm_runtime_scope_leave(scope);
        check_equal(allocations, 0u);
    }

    it("forwards pending buffers with numeric progress and asymmetric zero-length readiness") {
        unsigned read_count, write_count, i;
        for (read_count = 0u; read_count <= 3u; ++read_count)
            for (write_count = 0u; write_count <= 3u; ++write_count) {
                uint32_t count = read_count < write_count ? read_count : write_count;
                uint32_t read_handle, write_handle;
                primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32);
                check_true(turbowasm_component_resource_table_init(&tables[0], 8u));
                check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], NULL,
                    &reader, &writer), TURBOWASM_OK);
                check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, NULL, &tables[0],
                    &nested_reader, &nested_writer), TURBOWASM_OK);
                read_handle = reader.waitable.handle; write_handle = nested_writer.waitable.handle;
                numbers(0u, write_count, 42u); buffer(0u, 0u, write_count); buffer(1u, 1u, read_count);
                check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
                check_equal(turbowasm_component_endpoint_submit(&nested_reader, &buffers[1]), TURBOWASM_OK);
                refuse_allocations = true;
                check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_OK);
                refuse_allocations = false;
                check_true(reader.closed); check_true(nested_writer.closed);
                check_null(reader.peer); check_null(nested_writer.peer);
                check_true(writer.peer == &nested_reader); check_true(nested_reader.peer == &writer);
                check_equal(turbowasm_component_handle_kind_get(&tables[0], read_handle), TURBOWASM_COMPONENT_HANDLE_INVALID);
                check_equal(turbowasm_component_handle_kind_get(&tables[0], write_handle), TURBOWASM_COMPONENT_HANDLE_INVALID);
                check_equal(tables[0].live_count, 0u);
                for (i = 0u; i < count; ++i) {
                    check_equal(cells[1][i].as.u32, 42u + i);
                    check_equal(cells[0][i].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
                }
                if (count != 0u) {
                    check_equal(writer.available != NULL, write_count > count);
                    check_equal(nested_reader.available != NULL, read_count > count);
                    check_equal(take(&writer), count << 4u); check_equal(take(&nested_reader), count << 4u);
                } else if (write_count == 0u) {
                    check_equal(take(&writer), 0u); check_true(buffers[1].leased);
                    check_true(nested_reader.available == &buffers[1]);
                    check_false(nested_reader.waitable.state.endpoint.pending_event);
                } else {
                    check_equal(take(&nested_reader), 0u); check_true(buffers[0].leased);
                    check_true(writer.available == &buffers[0]);
                    check_false(writer.waitable.state.endpoint.pending_event);
                }
                cleanup();
            }
    }

    it("preserves previous undelivered progress across forwarding and later copies") {
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, NULL, NULL,
            &nested_reader, &nested_writer), TURBOWASM_OK);
        numbers(0u, 4u, 40u); buffer(0u, 0u, 4u); buffer(1u, 1u, 1u); buffer(2u, 2u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(take(&reader), 16u);
        check_equal(turbowasm_component_endpoint_submit(&nested_reader, &buffers[2]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_OK);
        check_equal(take(&nested_reader), 16u); check_equal(cells[2][0].as.u32, 41u);
        check_equal(buffers[0].progress, 2u); check_true(writer.available == &buffers[0]);
        buffers[3].values = &cells[2][1]; buffers[3].length = 2u;
        check_equal(turbowasm_component_endpoint_submit(&nested_reader, &buffers[3]), TURBOWASM_OK);
        check_equal(take(&nested_reader), 32u); check_equal(take(&writer), 64u);
        check_equal(cells[2][2].as.u32, 43u); check_false(buffers[0].leased);
    }

    it("forwards idle or pending futures and preserves unit completion") {
        unsigned scenario;
        for (scenario = 0u; scenario < 4u; ++scenario) {
            bool unit = (scenario & 1u) != 0u, pending = (scenario & 2u) != 0u;
            primitive_type(true, unit != 0u, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
            check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, NULL, NULL,
                &nested_reader, &nested_writer), TURBOWASM_OK);
            if (!pending) check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_OK);
            numbers(0u, 1u, 42u); buffer(0u, 0u, 1u); buffer(1u, 1u, 1u);
            check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_submit(&nested_reader, &buffers[1]), TURBOWASM_OK);
            if (pending) check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_OK);
            check_equal(take(&writer), TURBOWASM_COMPONENT_COPY_COMPLETED);
            check_equal(take(&nested_reader), TURBOWASM_COMPONENT_COPY_COMPLETED);
            check_equal(writer.waitable.state.endpoint.phase, TURBOWASM_COMPONENT_ENDPOINT_DONE);
            if (!unit) check_equal(cells[1][0].as.u32, 42u);
            cleanup();
        }
    }

    it("closes recursive forwarding and propagates an absent peer without losing a pending borrow") {
        unsigned side;
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        check_equal(turbowasm_component_endpoint_forward(&reader, &writer), TURBOWASM_OK);
        check_true(reader.closed); check_true(writer.closed); cleanup();
        for (side = 0u; side < 2u; ++side) {
            turbowasm_component_endpoint *survivor;
            primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
            check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, NULL, NULL,
                &nested_reader, &nested_writer), TURBOWASM_OK);
            survivor = side ? &writer : &nested_reader;
            if (side) numbers(0u, 1u, 42u);
            buffer(0u, 0u, 1u);
            check_equal(turbowasm_component_endpoint_submit(survivor, &buffers[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_close(side ? &nested_reader : &writer), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_OK);
            check_true(reader.closed); check_true(nested_writer.closed); check_true(buffers[0].leased);
            check_equal(take(survivor), TURBOWASM_COMPONENT_COPY_DROPPED); check_false(buffers[0].leased);
            if (side) check_equal(cells[0][0].as.u32, 42u);
            cleanup();
        }
    }

    it("rejects forwarding admission before consuming either endpoint") {
        turbowasm_component_event event;
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32);
        check_true(turbowasm_component_resource_table_init(&tables[0], 8u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], NULL,
            &reader, &writer), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, NULL, &tables[0],
            &nested_reader, &nested_writer), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_forward(&writer, &nested_writer), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_set_register(&tables[0], &set), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&tables[0], nested_writer.waitable.handle, set.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_join(&tables[0], nested_writer.waitable.handle, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_begin(&tables[0], reader.waitable.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_wait_cancel(&tables[0], reader.waitable.handle), TURBOWASM_OK);
        writer.waitable.delivering = true;
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_TRAPPED);
        writer.waitable.delivering = false;
        buffer(0u, 0u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_endpoint_cancel(&reader), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_take(&reader, &event), TURBOWASM_OK);
        check_false(reader.closed); check_false(nested_writer.closed);
        check_equal(tables[0].live_count, 3u);
        check_true(reader.peer == &writer); check_true(nested_writer.peer == &nested_reader);
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_OK);
    }

    it("matches independent type graphs and completes the surviving synchronous waits") {
        turbowasm_component_event event;
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32);
        check_true(turbowasm_component_type_graph_allocate(&value_graph, 2u));
        check_true(turbowasm_component_type_graph_define_scalar(&value_graph, 0u, TURBOWASM_COMPONENT_TYPE_U8));
        check_true(turbowasm_component_type_graph_define_async_value(&value_graph, 1u,
            TURBOWASM_COMPONENT_TYPE_STREAM, true, turbowasm_component_type_ref_indexed(0u)));
        check_true(turbowasm_component_resource_table_init(&tables[0], 8u));
        check_true(turbowasm_component_resource_table_init(&tables[1], 8u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], &tables[0],
            &reader, &writer), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_pair_open(&value_graph, 1u, &tables[1], &tables[1],
            &nested_reader, &nested_writer), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_TYPE_MISMATCH);
        check_false(reader.closed); check_false(nested_writer.closed);
        check_equal(tables[0].live_count, 2u); check_equal(tables[1].live_count, 2u);
        value_graph.types[0].kind = TURBOWASM_COMPONENT_TYPE_U32;
        numbers(0u, 1u, 42u); buffer(0u, 0u, 1u); buffer(1u, 1u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&nested_reader, &buffers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_begin(&tables[0], writer.waitable.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_begin(&tables[1], nested_reader.waitable.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_end(&tables[0], writer.waitable.handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 16u);
        check_equal(turbowasm_component_waitable_wait_end(&tables[1], nested_reader.waitable.handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 16u); check_equal(cells[1][0].as.u32, 42u);
        check_false(buffers[0].leased); check_false(buffers[1].leased);
    }

    it("preserves the same-component composite restriction across forwarding") {
        composite_type();
        check_true(turbowasm_component_resource_table_init(&tables[0], 8u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 3u, NULL, &tables[0],
            &reader, &writer), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 3u, &tables[0], NULL,
            &nested_reader, &nested_writer), TURBOWASM_OK);
        composite_value(0u); buffer(0u, 0u, 1u); buffer(1u, 1u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&nested_reader, &buffers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_TRAPPED);
        check_false(reader.closed); check_false(nested_writer.closed);
        check_true(buffers[0].leased); check_true(buffers[1].leased);
        check_equal(destroyed, 0u); check_equal(cells[0][0].kind, TURBOWASM_COMPONENT_TYPE_RECORD);
        check_equal(cells[1][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
    }

    it("forwards composite owners once after an intermediate readable value is explicitly taken") {
        turbowasm_component_endpoint *taken = NULL;
        composite_type();
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 3u, NULL, NULL,
            &reader, &writer), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 3u, NULL, NULL,
            &nested_reader, &nested_writer), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_into_value(&reader, &cells[2][0]), TURBOWASM_OK);
        composite_value(0u); buffer(0u, 0u, 1u); buffer(1u, 1u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&nested_reader, &buffers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_forward(&reader, &nested_writer), TURBOWASM_TRAPPED);
        check_not_null(reader.value_owner); check_false(reader.closed); check_false(nested_writer.closed);
        check_equal(turbowasm_component_endpoint_take_value(&cells[2][0], &taken), TURBOWASM_OK);
        check_true(taken == &reader);
        check_equal(turbowasm_component_endpoint_forward(taken, &nested_writer), TURBOWASM_OK);
        check_equal(take(&writer), 16u); check_equal(take(&nested_reader), 16u);
        check_equal(destroyed, 0u); check_equal(cells[0][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(cells[1][0].as.record.items[0].resource_identity, 42u);
        check_equal(turbowasm_component_value_destroy(&cells[1][0]), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&cells[1][0]), TURBOWASM_OK);
        check_equal(destroyed, 1u);
    }

    it("moves actual values in either arrival order and releases buffers at delivery") {
        unsigned read_first;
        for (read_first = 0u; read_first < 2u; ++read_first) {
            primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
            numbers(0u, 3u, 40u); buffer(0u, 0u, 3u); buffer(1u, 1u, 3u);
            check_equal(turbowasm_component_endpoint_submit(read_first ? &reader : &writer,
                &buffers[read_first ? 1u : 0u]), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_submit(read_first ? &writer : &reader,
                &buffers[read_first ? 0u : 1u]), TURBOWASM_OK);
            check_equal(cells[1][2].as.u32, 42u);
            check_equal(cells[0][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
            check_true(buffers[0].leased); check_true(buffers[1].leased);
            check_equal(turbowasm_component_endpoint_close(&reader), TURBOWASM_TRAPPED);
            check_equal(take(&reader), 48u); check_false(buffers[1].leased);
            check_true(buffers[0].leased);
            check_equal(take(&writer), 48u); check_false(buffers[0].leased);
            cleanup();
        }
    }

    it("accumulates partial progress on the older buffer until its event is consumed") {
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        numbers(0u, 4u, 10u); buffer(0u, 0u, 4u); buffer(1u, 1u, 2u); buffer(2u, 2u, 2u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(take(&reader), 32u); check_equal(buffers[0].progress, 2u);
        check_true(writer.available == &buffers[0]);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[2]), TURBOWASM_OK);
        check_equal(cells[2][0].as.u32, 12u); check_equal(cells[2][1].as.u32, 13u);
        check_equal(take(&writer), 64u); check_equal(take(&reader), 32u);
        check_null(writer.available); check_false(buffers[0].leased);
    }

    it("stops using the old buffer when a partial event returns its borrow") {
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        numbers(0u, 4u, 10u); buffer(0u, 0u, 4u); buffer(1u, 1u, 1u); buffer(2u, 2u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(take(&writer), 16u); check_null(writer.available);
        check_equal(take(&reader), 16u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[2]), TURBOWASM_OK);
        check_equal(cells[2][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(cells[0][1].as.u32, 11u);
        check_equal(turbowasm_component_endpoint_cancel(&reader), TURBOWASM_OK);
        check_equal(take(&reader), TURBOWASM_COMPONENT_COPY_CANCELLED);
    }

    it("accumulates separate writes into an older read and releases an exclusive wait") {
        turbowasm_component_event event;
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32);
        check_true(turbowasm_component_resource_table_init(&tables[0], 2u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], NULL,
            &reader, &writer), TURBOWASM_OK);
        numbers(0u, 2u, 42u); numbers(2u, 2u, 44u);
        buffer(0u, 0u, 2u); buffer(1u, 1u, 4u); buffer(2u, 2u, 2u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_begin(&tables[0], reader.waitable.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(take(&writer), 32u);
        check_true(reader.available == &buffers[1]);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[2]), TURBOWASM_OK);
        check_equal(take(&writer), 32u);
        check_true(buffers[1].leased); check_null(reader.available);
        check_equal(turbowasm_component_waitable_wait_end(&tables[0], reader.waitable.handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 64u); check_false(buffers[1].leased);
        check_false(reader.waitable.sync_waiter); check_null(reader.operation);
        check_equal(cells[1][0].as.u32, 42u); check_equal(cells[1][3].as.u32, 45u);
    }

    it("rejects overlapping destination cells even in an already moved source prefix") {
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        numbers(0u, 4u, 42u); buffer(0u, 0u, 4u); buffer(1u, 1u, 2u); buffer(2u, 0u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(take(&reader), 32u);
        check_equal(cells[0][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[2]), TURBOWASM_INVALID_ARGUMENT);
        check_false(buffers[2].leased); check_equal(buffers[0].progress, 2u);
        check_equal(cells[0][2].as.u32, 44u);
        check_equal(take(&writer), 32u);
    }

    it("completes only the writer in a zero-length rendezvous in either arrival order") {
        unsigned read_first;
        for (read_first = 0u; read_first < 2u; ++read_first) {
            turbowasm_component_event event;
            primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
            check_equal(turbowasm_component_endpoint_submit(read_first ? &reader : &writer,
                &buffers[read_first ? 0u : 1u]), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_submit(read_first ? &writer : &reader,
                &buffers[read_first ? 1u : 0u]), TURBOWASM_OK);
            check_equal(take(&writer), 0u);
            check_equal(turbowasm_component_endpoint_take(&reader, &event), TURBOWASM_YIELDED);
            check_true(buffers[0].leased); check_false(buffers[1].leased);
            numbers(0u, 1u, 42u); buffer(2u, 0u, 1u);
            check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[2]), TURBOWASM_OK);
            check_equal(take(&reader), 0u); check_false(buffers[0].leased);
            buffer(3u, 1u, 1u);
            check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[3]), TURBOWASM_OK);
            check_equal(cells[1][0].as.u32, 42u);
            check_equal(take(&reader), 16u); check_equal(take(&writer), 16u);
            cleanup();
        }
    }

    it("moves composite storage and resource owners without cloning or early destruction") {
        turbowasm_component_value *original;
        composite_type();
        check_true(turbowasm_component_resource_table_init(&tables[0], 4u));
        check_true(turbowasm_component_resource_table_init(&tables[1], 4u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 3u, &tables[0], &tables[1],
            &reader, &writer), TURBOWASM_OK);
        composite_value(0u); composite_value(1u); original = cells[0][0].as.record.items;
        buffer(0u, 0u, 2u); buffer(1u, 1u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_true(cells[1][0].as.record.items == original);
        check_equal(destroyed, 0u);
        check_equal(turbowasm_component_endpoint_cancel(&writer), TURBOWASM_OK);
        check_equal(take(&writer), 16u | TURBOWASM_COMPONENT_COPY_CANCELLED);
        check_equal(take(&reader), 16u);
        check_equal(turbowasm_component_value_destroy(&cells[1][0]), TURBOWASM_OK);
        check_equal(destroyed, 1u);
        check_equal(turbowasm_component_value_destroy(&cells[0][1]), TURBOWASM_OK);
        check_equal(destroyed, 2u);
    }

    it("releases the actual buffer through waitable-set delivery") {
        turbowasm_component_event event;
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32);
        check_true(turbowasm_component_resource_table_init(&tables[0], 4u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], &tables[0],
            &reader, &writer), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_register(&tables[0], &set), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&tables[0], reader.waitable.handle, set.handle), TURBOWASM_OK);
        numbers(0u, 1u, 42u); buffer(0u, 0u, 1u); buffer(1u, 1u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_true(buffers[1].leased);
        check_equal(turbowasm_component_waitable_set_poll(&tables[0], set.handle, &event), TURBOWASM_OK);
        check_equal(event.code, TURBOWASM_COMPONENT_EVENT_STREAM_READ);
        check_equal(event.payload, 16u); check_false(buffers[1].leased);
        check_null(reader.operation); check_equal(take(&writer), 16u);
    }

    it("rejects same-component nonnumeric copies before moving any value") {
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_BOOL);
        check_true(turbowasm_component_resource_table_init(&tables[0], 4u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], &tables[0],
            &reader, &writer), TURBOWASM_OK);
        cells[0][0].kind = TURBOWASM_COMPONENT_TYPE_BOOL; cells[0][0].as.boolean = true;
        buffer(0u, 0u, 1u); buffer(1u, 1u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_TRAPPED);
        check_false(buffers[0].leased); check_true(cells[0][0].as.boolean);
        check_equal(cells[1][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(turbowasm_component_endpoint_close(&writer), TURBOWASM_OK);
        check_equal(take(&reader), TURBOWASM_COMPONENT_COPY_DROPPED);
    }

    it("checks the entire source and destination before transferring any element") {
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        numbers(0u, 2u, 42u); cells[0][1].kind = TURBOWASM_COMPONENT_TYPE_S32;
        buffer(0u, 0u, 2u); buffer(1u, 1u, 2u);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_TYPE_MISMATCH);
        check_equal(cells[0][0].as.u32, 42u); check_false(buffers[0].leased);
        check_equal(cells[1][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(turbowasm_component_endpoint_cancel(&reader), TURBOWASM_OK); (void)take(&reader);
        cells[0][1].kind = TURBOWASM_COMPONENT_TYPE_U32;
        cells[1][1].kind = TURBOWASM_COMPONENT_TYPE_BOOL;
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_INVALID_ARGUMENT);
        check_false(buffers[1].leased);
        cells[1][1].kind = TURBOWASM_COMPONENT_TYPE_UNDEFINED;
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        buffers[1].values = cells[0];
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(cells[0][0].as.u32, 42u);
    }

    it("preserves copied values when closing changes the pending stream result") {
        unsigned future;
        for (future = 0u; future < 2u; ++future) {
            primitive_type(future != 0u, false, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
            numbers(0u, 1u, 42u); buffer(0u, 0u, 1u); buffer(1u, 1u, 1u);
            check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
            check_equal(take(&writer), future ? 0u : 16u);
            check_equal(turbowasm_component_endpoint_close(&writer), TURBOWASM_OK);
            check_equal(take(&reader), future ? TURBOWASM_COMPONENT_COPY_COMPLETED :
                (16u | TURBOWASM_COMPONENT_COPY_DROPPED));
            check_equal(cells[1][0].as.u32, 42u); check_false(buffers[1].leased);
            cleanup();
        }
    }

    it("supports maximal unit-stream counts without allocating dummy values") {
        size_t baseline;
        primitive_type(false, true, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        baseline = allocations;
        buffers[0].length = buffers[1].length = TURBOWASM_COMPONENT_COPY_MAX_LENGTH;
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(take(&reader), (uint32_t)TURBOWASM_COMPONENT_COPY_MAX_LENGTH << 4u);
        check_equal(take(&writer), (uint32_t)TURBOWASM_COMPONENT_COPY_MAX_LENGTH << 4u);
        check_equal(allocations, baseline);
        buffers[0].length = TURBOWASM_COMPONENT_COPY_MAX_LENGTH + 1u;
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_INVALID_ARGUMENT);
        check_false(buffers[0].leased);
    }

    it("enforces one unit completion per future") {
        primitive_type(true, true, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[0]), TURBOWASM_INVALID_ARGUMENT);
        buffers[0].length = 2u;
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[0]), TURBOWASM_INVALID_ARGUMENT);
        buffers[0].length = buffers[1].length = 1u;
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[1]), TURBOWASM_OK);
        check_equal(take(&reader), TURBOWASM_COMPONENT_COPY_COMPLETED);
        check_equal(take(&writer), TURBOWASM_COMPONENT_COPY_COMPLETED);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[0]), TURBOWASM_INVALID_ARGUMENT);
    }

    it("rolls back the first canonical handle when pair admission exhausts capacity") {
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32);
        check_true(turbowasm_component_resource_table_init(&tables[0], 1u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], &tables[0],
            &reader, &writer), TURBOWASM_OUT_OF_MEMORY);
        check_false(reader.initialized); check_false(writer.initialized);
        check_equal(tables[0].live_count, 0u);
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], NULL,
            &reader, &writer), TURBOWASM_OK);
        check_equal(tables[0].live_count, 1u);
    }

    it("moves readable ownership between tables while the writer remains pending") {
        turbowasm_component_resource_handle old;
        primitive_type(false, false, TURBOWASM_COMPONENT_TYPE_U32);
        check_true(turbowasm_component_resource_table_init(&tables[0], 4u));
        check_true(turbowasm_component_resource_table_init(&tables[1], 4u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], &tables[0],
            &reader, &writer), TURBOWASM_OK);
        numbers(0u, 1u, 42u); buffer(0u, 0u, 1u); buffer(1u, 1u, 1u);
        check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
        old = reader.waitable.handle;
        check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_OK);
        check_null(reader.waitable.table); check_equal(reader.waitable.handle, 0u);
        check_equal(turbowasm_component_handle_kind_get(&tables[0], old), TURBOWASM_COMPONENT_HANDLE_INVALID);
        check_true(reader.peer == &writer); check_true(writer.peer == &reader);
        check_true(buffers[0].leased);
        check_equal(turbowasm_component_endpoint_attach_readable(&reader, &tables[1]), TURBOWASM_OK);
        check_true(reader.waitable.table == &tables[1]);
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
        check_equal(cells[1][0].as.u32, 42u);
        check_equal(take(&writer), 16u); check_equal(take(&reader), 16u);
        check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_attach_readable(&reader, &tables[0]), TURBOWASM_OK);
        check_not_equal(reader.waitable.handle, old);
        check_equal(turbowasm_component_handle_kind_get(&tables[0], old), TURBOWASM_COMPONENT_HANDLE_INVALID);
    }

    it("preserves idle peer-closure notifications across readable ownership transfer") {
        unsigned future;
        for (future = 0u; future < 2u; ++future) {
            turbowasm_component_event event;
            primitive_type(future != 0u, true, TURBOWASM_COMPONENT_TYPE_U32);
            check_true(turbowasm_component_resource_table_init(&tables[0], 2u));
            check_true(turbowasm_component_resource_table_init(&tables[1], 2u));
            check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], NULL,
                &reader, &writer), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_close(&writer), TURBOWASM_OK);
            check_true(reader.waitable.state.endpoint.pending_event);
            check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_attach_readable(&reader, &tables[1]), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_take(&reader, &event), TURBOWASM_OK);
            check_equal(event.handle, reader.waitable.handle);
            check_equal(event.code, future ? TURBOWASM_COMPONENT_EVENT_FUTURE_READ : TURBOWASM_COMPONENT_EVENT_STREAM_READ);
            check_equal(event.payload, TURBOWASM_COMPONENT_COPY_DROPPED);
            check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_TRAPPED);
            check_true(reader.waitable.table == &tables[1]);
            cleanup();
        }
    }

    it("rejects moving joined, waiting, copying or cancelling readable ends") {
        turbowasm_component_resource_handle original;
        primitive_type(false, true, TURBOWASM_COMPONENT_TYPE_U32);
        check_true(turbowasm_component_resource_table_init(&tables[0], 4u));
        check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, &tables[0], &tables[0],
            &reader, &writer), TURBOWASM_OK);
        original = reader.waitable.handle;
        check_equal(turbowasm_component_endpoint_detach_readable(&writer), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_endpoint_attach_readable(&reader, &tables[0]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_set_register(&tables[0], &set), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&tables[0], original, set.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_TRAPPED);
        check_equal(reader.waitable.set_handle, set.handle);
        check_equal(turbowasm_component_waitable_join(&tables[0], original, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_begin(&tables[0], original), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_wait_cancel(&tables[0], original), TURBOWASM_OK);
        buffers[0].length = 1u;
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_endpoint_cancel(&reader), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_TRAPPED);
        check_true(buffers[0].leased); check_equal(reader.waitable.handle, original);
        check_equal(take(&reader), TURBOWASM_COMPONENT_COPY_CANCELLED);
        check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_endpoint_attach_readable(&reader, NULL), TURBOWASM_TRAPPED);
        check_null(reader.waitable.table);
        check_equal(turbowasm_component_endpoint_attach_readable(&writer, &tables[0]), TURBOWASM_TRAPPED);
    }

    it("retains host ownership and peer state when destination allocation or quota fails") {
        turbowasm_status status;
        primitive_type(false, true, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        check_true(turbowasm_component_resource_table_init(&tables[0], 1u));
        check_true(turbowasm_component_resource_table_init(&tables[1], 1u));
        check_equal(turbowasm_component_waitable_set_register(&tables[1], &set), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_attach_readable(&reader, &tables[1]), TURBOWASM_OUT_OF_MEMORY);
        check_null(reader.waitable.table); check_equal(reader.waitable.handle, 0u);
        check_true(reader.peer == &writer); check_true(writer.peer == &reader);
        refuse_allocations = true;
        status = turbowasm_component_endpoint_attach_readable(&reader, &tables[0]);
        refuse_allocations = false;
        check_equal(status, TURBOWASM_OUT_OF_MEMORY);
        check_null(reader.waitable.table); check_equal(tables[0].live_count, 0u);
        check_equal(turbowasm_component_endpoint_attach_readable(&reader, &tables[0]), TURBOWASM_OK);
        check_equal(tables[0].live_count, 1u);
        check_equal(turbowasm_component_endpoint_detach_readable(&reader), TURBOWASM_OK);
        check_equal(tables[0].live_count, 0u);
    }

    it("freezes direct access while a value owns a readable endpoint") {
        turbowasm_component_endpoint *taken = &writer;
        turbowasm_component_event event;
        primitive_type(true, true, TURBOWASM_COMPONENT_TYPE_U32); open_hosts();
        check_equal(turbowasm_component_endpoint_into_value(&writer, &cells[0][0]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_endpoint_into_value(&reader, &cells[0][0]), TURBOWASM_OK);
        check_not_null(reader.value_owner);
        check_equal(turbowasm_component_endpoint_into_value(&reader, &cells[0][1]), TURBOWASM_TRAPPED);
        check_equal(cells[0][1].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        buffers[0].length = 1u;
        check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[0]), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_endpoint_take(&reader, &event), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_endpoint_close(&reader), TURBOWASM_INVALID_ARGUMENT);
        check_true(turbowasm_component_resource_table_init(&tables[0], 1u));
        check_equal(turbowasm_component_endpoint_attach_readable(&reader, &tables[0]), TURBOWASM_TRAPPED);
        check_equal(tables[0].live_count, 0u);
        check_equal(turbowasm_component_endpoint_take_value(&cells[0][1], &taken), TURBOWASM_INVALID_ARGUMENT);
        check_true(taken == &writer);
        check_equal(turbowasm_component_endpoint_close(&writer), TURBOWASM_OK);
        check_equal(turbowasm_component_canonical_validate_value(&graph,
            turbowasm_component_type_ref_indexed(0u), &cells[0][0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_take_value(&cells[0][0], &taken), TURBOWASM_OK);
        check_true(taken == &reader); check_null(reader.value_owner);
        check_equal(cells[0][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(take(taken), TURBOWASM_COMPONENT_COPY_DROPPED);
        check_equal(turbowasm_component_endpoint_into_value(taken, &cells[0][0]), TURBOWASM_TRAPPED);
    }

    it("moves nested endpoints through lists of records across independently indexed types") {
        unsigned scenario;
        for (scenario = 0u; scenario < 4u; ++scenario) {
            bool extract = (scenario & 1u) != 0u;
            bool future = scenario < 2u;
            turbowasm_component_type_kind endpoint_type = future
                ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM;
            turbowasm_component_record_field field = {
                (const uint8_t *)"endpoint", 8u, {0}
            };
            turbowasm_component_value *record, *leaf;
            turbowasm_component_endpoint *taken = NULL;
            check_true(turbowasm_component_type_graph_allocate(&graph, 4u));
            check_true(turbowasm_component_type_graph_define_async_value(&graph, 0u,
                endpoint_type, true,
                turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32)));
            field.type = turbowasm_component_type_ref_indexed(0u);
            check_true(turbowasm_component_type_graph_define_record(&graph, 1u, &field, 1u));
            check_true(turbowasm_component_type_graph_define_list(&graph, 2u, 1u));
            check_true(turbowasm_component_type_graph_define_async_value(&graph, 3u,
                TURBOWASM_COMPONENT_TYPE_STREAM, true, turbowasm_component_type_ref_indexed(2u)));
            check_true(turbowasm_component_type_graph_allocate(&value_graph, 2u));
            check_true(turbowasm_component_type_graph_define_scalar(&value_graph, 0u, TURBOWASM_COMPONENT_TYPE_U32));
            check_true(turbowasm_component_type_graph_define_async_value(&value_graph, 1u,
                endpoint_type, true, turbowasm_component_type_ref_indexed(0u)));
            check_equal(turbowasm_component_endpoint_pair_open(&graph, 3u, NULL, NULL,
                &reader, &writer), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_pair_open(&value_graph, 1u, NULL, NULL,
                &nested_reader, &nested_writer), TURBOWASM_OK);
            cells[0][0].kind = TURBOWASM_COMPONENT_TYPE_LIST;
            record = turbowasm_rt_calloc(1u, sizeof(*record)); check_not_null(record);
            cells[0][0].as.list.items = record; cells[0][0].as.list.count = 1u;
            record->kind = TURBOWASM_COMPONENT_TYPE_RECORD;
            leaf = turbowasm_rt_calloc(1u, sizeof(*leaf)); check_not_null(leaf);
            record->as.record.items = leaf; record->as.record.count = 1u;
            check_equal(turbowasm_component_endpoint_into_value(&nested_reader, leaf), TURBOWASM_OK);
            buffer(0u, 0u, 1u); buffer(1u, 1u, 1u);
            graph.types[0].as.async_value.payload = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U64);
            check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_TYPE_MISMATCH);
            check_false(buffers[0].leased); check_not_null(nested_reader.value_owner);
            graph.types[0].as.async_value.payload = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32);
            check_equal(turbowasm_component_endpoint_submit(&writer, &buffers[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_submit(&reader, &buffers[1]), TURBOWASM_OK);
            check_equal(take(&writer), 16u); check_equal(take(&reader), 16u);
            check_equal(cells[0][0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
            check_true(cells[1][0].as.list.items == record); check_not_null(nested_reader.value_owner);
            numbers(2u, 1u, 42u); buffer(2u, 2u, 1u);
            check_equal(turbowasm_component_endpoint_submit(&nested_writer, &buffers[2]), TURBOWASM_OK);
            if (extract) {
                check_equal(turbowasm_component_endpoint_take_value(leaf, &taken), TURBOWASM_OK);
                check_true(taken == &nested_reader); check_null(nested_reader.value_owner);
                buffers[3].values = &cells[2][1]; buffers[3].length = 1u;
                check_equal(turbowasm_component_endpoint_submit(taken, &buffers[3]), TURBOWASM_OK);
                check_equal(take(taken), future ? TURBOWASM_COMPONENT_COPY_COMPLETED : 16u);
                check_equal(take(&nested_writer), future ? TURBOWASM_COMPONENT_COPY_COMPLETED : 16u);
                check_equal(cells[2][1].as.u32, 42u);
                check_equal(turbowasm_component_value_destroy(&cells[1][0]), TURBOWASM_OK);
                check_false(nested_reader.closed);
            } else {
                check_equal(turbowasm_component_value_destroy(&cells[1][0]), TURBOWASM_OK);
                check_true(nested_reader.closed); check_null(nested_reader.value_owner);
                check_equal(take(&nested_writer), TURBOWASM_COMPONENT_COPY_DROPPED);
                check_equal(cells[2][0].as.u32, 42u);
                check_equal(turbowasm_component_value_destroy(&cells[1][0]), TURBOWASM_OK);
            }
            cleanup();
        }
    }
}
