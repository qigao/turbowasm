#include "component_endpoint.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include <turbowasm/turbowasm.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t memory32_bytes[] = {
    0,0x61,0x73,0x6d,1,0,0,0, 5,3,1,0,1
};
static const uint8_t memory64_bytes[] = {
    0,0x61,0x73,0x6d,1,0,0,0, 5,3,1,4,1
};
static turbowasm_module module;
static turbowasm_instance instance;
static turbowasm_component_type_graph graph;
static turbowasm_component_type_graph expected_graph;
static turbowasm_component_resource_table table;
static turbowasm_component_endpoint reader[2], writer[2];
static turbowasm_component_endpoint_codec codec, other_codec;
static turbowasm_component_canonical_memory memory;
static turbowasm_component_waitable_set set;
static turbowasm_component_waitable standalone_waitable;
static turbowasm_component_value values[4];
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t live_allocations, allowed_allocations;
static uint64_t allocation_cursor;
static bool probe_reentry;
static turbowasm_status reentry_status;
static uint32_t reentry_count;
static bool move_admitted, destroy_on_publish;
static unsigned move_finishes, move_transfers;
static unsigned move_begins;

static void begin_move(void *context) { (void)context; ++move_begins; }

static void finish_move(void *context, bool transferred) {
    (void)context; ++move_finishes;
    if (transferred) ++move_transfers;
    if (destroy_on_publish) {
        unsigned i;
        /* A cleanup callback observes the entire published transaction and can
         * retire both its own canonical carrier and the next notification. */
        check_null(codec.lower_head);
        check_equal(move_begins, 2u);
        for (i = 0u; i < 2u; ++i) {
            check_equal(reader[i].waitable.table, &table);
            check_equal(turbowasm_component_endpoint_get(&table, reader[i].waitable.handle,
                TURBOWASM_COMPONENT_HANDLE_STREAM_READ), &reader[i]);
            check_equal(turbowasm_component_value_destroy(&values[i]), TURBOWASM_OK);
        }
    }
}

static void *allocate(void *context, size_t size) {
    void *pointer; (void)context;
    if (allowed_allocations == 0u) return NULL;
    if (allowed_allocations != SIZE_MAX) --allowed_allocations;
    pointer = malloc(size);
    if (pointer != NULL) ++live_allocations;
    return pointer;
}
static void deallocate(void *context, void *pointer) {
    (void)context;
    if (pointer != NULL) --live_allocations;
    free(pointer);
}
static turbowasm_component_type_ref indexed(uint32_t index) {
    return turbowasm_component_type_ref_indexed(index);
}
static void cleanup(void) {
    uint32_t i;
    allowed_allocations = SIZE_MAX;
    if (codec.table != NULL) (void)turbowasm_component_endpoint_codec_rollback(&codec);
    if (other_codec.table != NULL) (void)turbowasm_component_endpoint_codec_rollback(&other_codec);
    for (i = 0u; i < 4u; ++i) (void)turbowasm_component_value_destroy(&values[i]);
    for (i = 0u; i < 2u; ++i) {
        if (reader[i].initialized && !reader[i].closed) (void)turbowasm_component_endpoint_close(&reader[i]);
        if (writer[i].initialized && !writer[i].closed) (void)turbowasm_component_endpoint_close(&writer[i]);
    }
    if (set.table != NULL) (void)turbowasm_component_waitable_set_drop(&table, set.handle);
    if (standalone_waitable.table != NULL)
        (void)turbowasm_component_waitable_drop(&table, standalone_waitable.handle);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
    turbowasm_component_type_graph_destroy(&expected_graph);
    turbowasm_instance_destroy(&instance); turbowasm_module_destroy(&module);
    memset(reader, 0, sizeof(reader)); memset(writer, 0, sizeof(writer));
    memset(&codec, 0, sizeof(codec)); memset(&other_codec, 0, sizeof(other_codec));
    memset(&memory, 0, sizeof(memory)); memset(&set, 0, sizeof(set));
    memset(&standalone_waitable, 0, sizeof(standalone_waitable));
    probe_reentry = false;
}
static uint8_t *bytes_at(uint64_t address, size_t length) {
    uint8_t *bytes = NULL;
    check_equal(turbowasm_instance_memory_bounds(instance.impl, 0u, address, 0u, length, &bytes), TURBOWASM_OK);
    return bytes;
}
static uint32_t read_word(uint64_t address) {
    uint8_t *bytes = bytes_at(address, 4u);
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8u |
        (uint32_t)bytes[2] << 16u | (uint32_t)bytes[3] << 24u;
}
static void write_word(uint64_t address, uint32_t value) {
    uint8_t *bytes = bytes_at(address, 4u);
    unsigned i;
    for (i = 0u; i < 4u; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
}
static turbowasm_status guest_realloc(void *context, uint64_t old_pointer,
    uint64_t old_size, uint64_t alignment, uint64_t size, uint64_t *out) {
    uint64_t pointer;
    uint8_t *bytes;
    turbowasm_status status;
    (void)context; (void)old_pointer; (void)old_size;
    if (probe_reentry) {
        uint64_t previous;
        uint32_t i;
        turbowasm_component_resource_handle handle;
        turbowasm_component_value attempt = {0};
        turbowasm_value rep = {0};
        ++reentry_count;
        if (codec.lower_head == NULL) return TURBOWASM_TRAPPED;
        /* Force table relocation during the reentrant boundary, then verify
         * that the previously written endpoint handle is still unpublished. */
        rep.kind = TURBOWASM_VALUE_I32;
        for (i = 0u; i < 8u; ++i) {
            status = turbowasm_component_resource_new_owned(&table, 42u, rep, &handle);
            if (status != TURBOWASM_OK) return status;
        }
        reentry_status = turbowasm_component_endpoint_codec_lift(&codec, &graph,
            indexed(0u), codec.lower_head->lower_handle, &attempt);
        (void)turbowasm_component_value_destroy(&attempt);
        if (reentry_status != TURBOWASM_TRAPPED) return TURBOWASM_TRAPPED;
        status = turbowasm_instance_memory_grow64(instance.impl, 0u, 1u, &previous);
        if (status != TURBOWASM_OK) return status;
        allocation_cursor = previous * UINT64_C(65536);
    }
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        allocation_cursor > UINT64_MAX - (alignment - 1u)) return TURBOWASM_TRAPPED;
    pointer = (allocation_cursor + alignment - 1u) & ~(alignment - 1u);
    if (size > SIZE_MAX || pointer > UINT64_MAX - size) return TURBOWASM_TRAPPED;
    status = turbowasm_instance_memory_bounds(instance.impl, 0u, pointer, 0u, (size_t)size, &bytes);
    if (status != TURBOWASM_OK) return status;
    allocation_cursor = pointer + size; *out = pointer;
    return TURBOWASM_OK;
}
static void initialize(bool memory64, bool future, uint32_t capacity) {
    turbowasm_component_record_field fields[2] = {
        {(const uint8_t *)"endpoint", 8u, {0}},
        {(const uint8_t *)"character", 9u, {0}}
    };
    check_true(turbowasm_component_type_graph_allocate(&graph, 4u));
    check_true(turbowasm_component_type_graph_define_async_value(&graph, 0u,
        future ? TURBOWASM_COMPONENT_TYPE_FUTURE : TURBOWASM_COMPONENT_TYPE_STREAM, true,
        turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32)));
    fields[0].type = indexed(0u);
    fields[1].type = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_CHAR);
    check_true(turbowasm_component_type_graph_define_record(&graph, 1u, fields, 2u));
    check_true(turbowasm_component_type_graph_define_list(&graph, 2u, 1u));
    fields[1].type = turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_STRING);
    check_true(turbowasm_component_type_graph_define_record(&graph, 3u, fields, 2u));
    check_true(turbowasm_component_type_graph_validate(&graph));
    check_equal(turbowasm_module_load_borrowed_with_config(&module,
        memory64 ? memory64_bytes : memory32_bytes, sizeof(memory32_bytes), &config), TURBOWASM_OK);
    check_equal(turbowasm_instance_create(&instance, &module), TURBOWASM_OK);
    check_true(turbowasm_component_resource_table_init(&table, capacity));
    codec.table = other_codec.table = &table;
    memory.instance = &instance;
    memory.pointer_type = memory64 ? TURBOWASM_COMPONENT_POINTER_I64 : TURBOWASM_COMPONENT_POINTER_I32;
    memory.endpoint_lower = turbowasm_component_endpoint_codec_lower;
    memory.endpoint_lift = turbowasm_component_endpoint_codec_lift;
    memory.endpoint_context = &codec;
    memory.guest_realloc = guest_realloc;
    allocation_cursor = 128u;
}
static void open_pair(uint32_t index, bool guest) {
    check_equal(turbowasm_component_endpoint_pair_open(&graph, 0u, guest ? &table : NULL,
        NULL, &reader[index], &writer[index]), TURBOWASM_OK);
}
static void host_value(uint32_t pair, turbowasm_component_value *out) {
    open_pair(pair, false);
    check_equal(turbowasm_component_endpoint_into_value(&reader[pair], out), TURBOWASM_OK);
}
static void record_value(turbowasm_component_value *out, uint32_t pair, uint32_t character) {
    turbowasm_component_value *fields = turbowasm_rt_calloc(2u, sizeof(*fields));
    check_not_null(fields);
    out->kind = TURBOWASM_COMPONENT_TYPE_RECORD;
    out->as.record.items = fields; out->as.record.count = 2u;
    host_value(pair, &fields[0]);
    fields[1].kind = TURBOWASM_COMPONENT_TYPE_CHAR;
    fields[1].as.character = character;
}

spec("Canonical endpoint handle ownership") {
    before_each() {
        allowed_allocations = SIZE_MAX; live_allocations = 0u;
        probe_reentry = false; reentry_count = 0u; reentry_status = TURBOWASM_OK;
        move_admitted = destroy_on_publish = false; move_finishes = move_transfers = 0u;
        move_begins = 0u;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
    }
    after_each() {
        cleanup(); turbowasm_runtime_scope_leave(scope);
        check_equal(live_allocations, 0u);
    }

    it("rolls back an unadmitted endpoint without closing it or consuming its peer") {
        turbowasm_component_endpoint *taken = NULL;
        uint32_t handle;
        initialize(false, true, 4u); host_value(0u, &values[0]);
        check_equal(turbowasm_component_endpoint_value_adopt(&values[0], &move_admitted,
            NULL, finish_move, &move_finishes), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_value_adopt(&values[0], &move_admitted,
            NULL, finish_move, &move_finishes), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_endpoint_take_value(&values[0], &taken), TURBOWASM_TRAPPED);
        check_null(taken);
        check_equal(turbowasm_component_endpoint_codec_lower(&codec, &graph, indexed(0u), &values[0], &handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_commit(&codec), TURBOWASM_TRAPPED);
        check_equal(table.live_count, 1u); check_equal(move_finishes, 0u);
        check_equal(turbowasm_component_endpoint_codec_rollback(&codec), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&values[0]), TURBOWASM_OK);
        check_equal(move_finishes, 1u); check_equal(move_transfers, 0u);
        check_false(reader[0].closed); check_null(reader[0].value_owner);
        check_equal(reader[0].peer, &writer[0]); check_equal(table.live_count, 0u);
        check_equal(turbowasm_component_endpoint_into_value(&reader[0], &values[0]), TURBOWASM_OK);
    }

    it("takes an admitted proxy once and restores direct ownership") {
        turbowasm_component_endpoint *taken = NULL;
        initialize(false, false, 4u); host_value(0u, &values[0]);
        check_equal(turbowasm_component_endpoint_value_adopt(&values[0], &move_admitted,
            NULL, finish_move, &move_finishes), TURBOWASM_OK);
        move_admitted = true;
        check_equal(turbowasm_component_endpoint_take_value(&values[0], &taken), TURBOWASM_OK);
        check_equal(taken, &reader[0]); check_false(reader[0].closed); check_null(reader[0].value_owner);
        check_equal(move_finishes, 1u); check_equal(move_transfers, 1u);
        check_equal(turbowasm_component_value_destroy(&values[0]), TURBOWASM_OK);
        check_equal(move_finishes, 1u);
    }

    it("closes an admitted unpublished proxy exactly once") {
        initialize(false, true, 4u); host_value(0u, &values[0]);
        check_equal(turbowasm_component_endpoint_value_adopt(&values[0], &move_admitted,
            NULL, finish_move, &move_finishes), TURBOWASM_OK);
        move_admitted = true;
        check_equal(turbowasm_component_value_destroy(&values[0]), TURBOWASM_OK);
        check_true(reader[0].closed); check_null(writer[0].peer);
        check_equal(move_finishes, 1u); check_equal(move_transfers, 1u);
        check_equal(turbowasm_component_value_destroy(&values[0]), TURBOWASM_OK);
        check_equal(move_finishes, 1u);
    }

    it("publishes every handle before cleanup reentry retires both notification records") {
        uint32_t handles[2]; unsigned i;
        initialize(false, false, 4u);
        for (i = 0u; i < 2u; ++i) {
            host_value(i, &values[i]);
            check_equal(turbowasm_component_endpoint_value_adopt(&values[i], &move_admitted,
                begin_move, finish_move, &move_finishes), TURBOWASM_OK);
            check_equal(turbowasm_component_endpoint_codec_lower(&codec, &graph, indexed(0u), &values[i], &handles[i]), TURBOWASM_OK);
        }
        move_admitted = true; destroy_on_publish = true;
        check_equal(turbowasm_component_endpoint_codec_commit(&codec), TURBOWASM_OK);
        destroy_on_publish = false;
        check_equal(move_finishes, 2u); check_equal(move_transfers, 2u);
        for (i = 0u; i < 2u; ++i) {
            check_equal(reader[i].waitable.handle, handles[i]); check_false(reader[i].closed);
            check_equal(values[i].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        }
    }

    it("uses one i32 carrier and four bytes for endpoints under both memory widths") {
        uint32_t scenario;
        for (scenario = 0u; scenario < 4u; ++scenario) {
            turbowasm_component_layout layout;
            turbowasm_component_flat_type_list flat;
            turbowasm_value carrier = {0};
            uint32_t count = 0u;
            turbowasm_component_event event;
            initialize((scenario & 1u) != 0u, scenario < 2u, 4u);
            host_value(0u, &values[0]);
            check_equal(turbowasm_component_canonical_layout(&graph, indexed(0u), memory.pointer_type, &layout), TURBOWASM_OK);
            check_equal(layout.size, UINT64_C(4)); check_equal(layout.alignment, UINT64_C(4));
            check_equal(turbowasm_component_canonical_flatten_type(&graph, indexed(0u), memory.pointer_type, &flat), TURBOWASM_OK);
            check_equal(flat.count, 1u); check_equal(flat.types[0], TURBOWASM_COMPONENT_FLAT_I32);
            check_equal(turbowasm_component_canonical_lower_flat_value(&graph, indexed(0u), &memory,
                &values[0], &carrier, 1u, &count), TURBOWASM_OK);
            check_equal(count, 1u); check_equal(carrier.kind, TURBOWASM_VALUE_I32);
            check_equal(turbowasm_component_waitable_take(&table, (uint32_t)carrier.as.i32, &event), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_canonical_lift_flat_value(&graph, indexed(0u), &memory,
                &carrier, 1u, &values[1]), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_endpoint_codec_commit(&codec), TURBOWASM_OK);
            check_equal(turbowasm_component_canonical_validate_value(&graph, indexed(0u), &values[0]), TURBOWASM_TYPE_MISMATCH);
            check_equal(turbowasm_component_canonical_lift_flat_value(&graph, indexed(0u), &memory,
                &carrier, 1u, &values[1]), TURBOWASM_OK);
            check_equal(table.live_count, 0u);
            /* The obsolete record must not close the newly lifted owner. */
            check_equal(turbowasm_component_value_destroy(&values[0]), TURBOWASM_OK);
            check_false(reader[0].closed); check_not_null(reader[0].value_owner);
            check_equal(turbowasm_component_value_destroy(&values[1]), TURBOWASM_OK);
            check_true(reader[0].closed);
            check_equal(turbowasm_component_endpoint_take(&writer[0], &event), TURBOWASM_OK);
            check_equal(event.payload, TURBOWASM_COMPONENT_COPY_DROPPED);
            cleanup();
        }
    }

    it("round trips endpoint records through actual guest memory and rejects stale handles") {
        unsigned memory64;
        for (memory64 = 0u; memory64 < 2u; ++memory64) {
            uint32_t handle;
            initialize(memory64 != 0u, false, 4u);
            record_value(&values[0], 0u, 0x1f642u);
            check_equal(turbowasm_component_canonical_lower_value(&graph, indexed(1u), &memory, 65528u, &values[0]), TURBOWASM_OK);
            handle = read_word(65528u); check_equal(read_word(65532u), 0x1f642u);
            check_equal(turbowasm_component_endpoint_codec_commit(&codec), TURBOWASM_OK);
            check_equal(turbowasm_component_canonical_lift_value(&graph, indexed(1u), &memory, 65528u, &values[1]), TURBOWASM_OK);
            check_equal(values[1].as.record.items[1].as.character, 0x1f642u);
            check_equal(turbowasm_component_value_destroy(&values[0]), TURBOWASM_OK);
            check_false(reader[0].closed);
            check_equal(turbowasm_component_endpoint_codec_lift(&codec, &graph, indexed(0u), handle, &values[2]), TURBOWASM_TRAPPED);
            cleanup();
        }
    }

    it("checks whole-object bounds and alignment before consuming or staging endpoints") {
        static const uint64_t invalid[] = {1u, 65532u, UINT64_MAX - 3u, UINT64_C(0x100000000)};
        unsigned width, i;
        for (width = 0u; width < 2u; ++width) {
            uint32_t handle;
            initialize(width != 0u, true, 4u); open_pair(0u, true);
            handle = reader[0].waitable.handle;
            host_value(1u, &values[0]);
            for (i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
                check_equal(turbowasm_component_canonical_lift_value(&graph, indexed(1u), &memory, invalid[i], &values[1]), TURBOWASM_TRAPPED);
                check_equal(reader[0].waitable.handle, handle); check_equal(table.live_count, 1u);
                check_equal(turbowasm_component_canonical_lower_value(&graph, indexed(1u), &memory, invalid[i], &values[0]), TURBOWASM_TRAPPED);
                check_null(codec.lower_head); check_not_null(reader[1].value_owner);
            }
            cleanup();
        }
    }

    it("rolls back a reserved handle after a later composite field fails") {
        unsigned width;
        for (width = 0u; width < 2u; ++width) {
            uint32_t stale;
            initialize(width != 0u, true, 4u); record_value(&values[0], 0u, 0xd800u);
            check_equal(turbowasm_component_canonical_lower_value(&graph, indexed(1u), &memory, 16u, &values[0]), TURBOWASM_INVALID_ARGUMENT);
            stale = read_word(16u); check_equal(table.live_count, 1u);
            check_not_null(reader[0].value_owner);
            check_equal(turbowasm_component_endpoint_codec_rollback(&codec), TURBOWASM_OK);
            check_equal(table.live_count, 0u); check_null(codec.lower_head);
            check_equal(turbowasm_component_handle_kind_get(&table, stale), TURBOWASM_COMPONENT_HANDLE_INVALID);
            values[0].as.record.items[1].as.character = 'x';
            check_equal(turbowasm_component_canonical_lower_value(&graph, indexed(1u), &memory, 16u, &values[0]), TURBOWASM_OK);
            check_not_equal(read_word(16u), stale);
            check_equal(turbowasm_component_endpoint_codec_commit(&codec), TURBOWASM_OK);
            check_equal(turbowasm_component_canonical_lift_value(&graph, indexed(1u), &memory, 16u, &values[1]), TURBOWASM_OK);
            cleanup();
        }
    }

    it("destroys an already lifted endpoint when a later guest field is invalid") {
        unsigned width;
        for (width = 0u; width < 2u; ++width) {
            turbowasm_component_event event;
            initialize(width != 0u, true, 4u); open_pair(0u, true);
            write_word(16u, reader[0].waitable.handle); write_word(20u, 0xd800u);
            check_equal(turbowasm_component_canonical_lift_value(&graph, indexed(1u), &memory, 16u, &values[0]), TURBOWASM_TRAPPED);
            check_equal(values[0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
            check_equal(table.live_count, 0u); check_true(reader[0].closed);
            check_equal(turbowasm_component_endpoint_take(&writer[0], &event), TURBOWASM_OK);
            check_equal(event.payload, TURBOWASM_COMPONENT_COPY_DROPPED);
            cleanup();
        }
    }

    it("preserves guest handles when owner or composite allocation fails") {
        size_t budget;
        for (budget = 0u; budget < 2u; ++budget) {
            uint32_t handle;
            turbowasm_status status;
            initialize(false, false, 4u); open_pair(0u, true);
            handle = reader[0].waitable.handle;
            write_word(16u, handle); write_word(20u, 'x');
            allowed_allocations = budget;
            status = turbowasm_component_canonical_lift_value(&graph, indexed(1u), &memory, 16u, &values[0]);
            allowed_allocations = SIZE_MAX;
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_equal(reader[0].waitable.handle, handle); check_equal(table.live_count, 1u);
            check_null(reader[0].value_owner); check_false(reader[0].closed);
            check_equal(turbowasm_component_canonical_lift_value(&graph, indexed(1u), &memory, 16u, &values[0]), TURBOWASM_OK);
            cleanup();
        }
    }

    it("rejects wrong kind, payload, set membership and duplicate lowers before consumption") {
        uint32_t handle, ignored = 99u;
        turbowasm_component_endpoint *taken = NULL;
        initialize(false, true, 4u); open_pair(0u, true);
        handle = reader[0].waitable.handle;
        check_equal(turbowasm_component_endpoint_codec_lift(&codec, &graph, indexed(1u), handle, &values[0]), TURBOWASM_TYPE_MISMATCH);
        check_true(turbowasm_component_type_graph_allocate(&expected_graph, 2u));
        check_true(turbowasm_component_type_graph_define_async_value(&expected_graph, 0u,
            TURBOWASM_COMPONENT_TYPE_FUTURE, true, turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U64)));
        check_true(turbowasm_component_type_graph_define_async_value(&expected_graph, 1u,
            TURBOWASM_COMPONENT_TYPE_STREAM, true, turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32)));
        check_equal(turbowasm_component_endpoint_codec_lift(&codec, &expected_graph, indexed(0u), handle, &values[0]), TURBOWASM_TYPE_MISMATCH);
        check_equal(turbowasm_component_endpoint_codec_lift(&codec, &expected_graph, indexed(1u), handle, &values[0]), TURBOWASM_TRAPPED);
        check_equal(reader[0].waitable.handle, handle);
        check_equal(turbowasm_component_waitable_set_register(&table, &set), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&table, handle, set.handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_lift(&codec, &graph, indexed(0u), handle, &values[0]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_join(&table, handle, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_begin(&table, handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_lift(&codec, &graph, indexed(0u), handle, &values[0]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_wait_cancel(&table, handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_lift(&codec, &graph, indexed(0u), handle, &values[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_lower(&codec, &expected_graph, indexed(0u), &values[0], &ignored), TURBOWASM_TYPE_MISMATCH);
        check_equal(ignored, 99u); check_null(codec.lower_head);
        check_equal(turbowasm_component_endpoint_codec_lower(&codec, &graph, indexed(0u), &values[0], &handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_lower(&other_codec, &graph, indexed(0u), &values[0], &ignored), TURBOWASM_TRAPPED);
        check_equal(ignored, 99u); check_null(other_codec.lower_head);
        check_equal(turbowasm_component_endpoint_take_value(&values[0], &taken), TURBOWASM_TRAPPED); check_null(taken);
        check_equal(turbowasm_component_endpoint_codec_rollback(&codec), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_take_value(&values[0], &taken), TURBOWASM_OK);
        check_true(taken == &reader[0]);
    }

    it("preserves both source owners across quota and allocation failures") {
        uint32_t first, second = 99u;
        turbowasm_status status;
        initialize(false, false, 1u); host_value(0u, &values[0]); host_value(1u, &values[1]);
        allowed_allocations = 0u;
        status = turbowasm_component_endpoint_codec_lower(&codec, &graph, indexed(0u), &values[0], &second);
        allowed_allocations = SIZE_MAX;
        check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_equal(second, 99u);
        check_null(codec.lower_head); check_not_null(reader[0].value_owner);
        check_equal(turbowasm_component_endpoint_codec_lower(&codec, &graph, indexed(0u), &values[0], &first), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_lower(&codec, &graph, indexed(0u), &values[1], &second), TURBOWASM_OUT_OF_MEMORY);
        check_equal(second, 99u); check_not_null(reader[1].value_owner);
        check_equal(turbowasm_component_endpoint_codec_rollback(&codec), TURBOWASM_OK);
        check_equal(table.live_count, 0u);
        check_equal(turbowasm_component_endpoint_codec_lower(&codec, &graph, indexed(0u), &values[1], &second), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_commit(&codec), TURBOWASM_OK);
        check_not_null(reader[0].value_owner); check_null(reader[1].value_owner);
    }

    it("requires endpoint callbacks even when resource callbacks are supplied") {
        turbowasm_value carrier = {0}; uint32_t count = 99u;
        initialize(false, true, 4u); host_value(0u, &values[0]);
        memory.resource_lower = memory.endpoint_lower; memory.resource_lift = memory.endpoint_lift;
        memory.resource_context = &codec; memory.endpoint_lower = NULL; memory.endpoint_lift = NULL;
        check_equal(turbowasm_component_canonical_lower_flat_value(&graph, indexed(0u), &memory,
            &values[0], &carrier, 1u, &count), TURBOWASM_UNSUPPORTED);
        check_equal(count, 99u); check_null(codec.lower_head); check_equal(table.live_count, 0u);
        carrier.kind = TURBOWASM_VALUE_I32;
        check_equal(turbowasm_component_canonical_lift_flat_value(&graph, indexed(0u), &memory,
            &carrier, 1u, &values[1]), TURBOWASM_UNSUPPORTED);
    }

    it("does not consume a host endpoint when allocating its value owner fails") {
        turbowasm_status status;
        initialize(false, false, 4u); open_pair(0u, false);
        allowed_allocations = 0u;
        status = turbowasm_component_endpoint_into_value(&reader[0], &values[0]);
        allowed_allocations = SIZE_MAX;
        check_equal(status, TURBOWASM_OUT_OF_MEMORY);
        check_null(reader[0].value_owner); check_equal(values[0].kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
        check_equal(turbowasm_component_endpoint_into_value(&reader[0], &values[0]), TURBOWASM_OK);
    }

    it("keeps reservations unpublished through realloc reentry and memory growth") {
        unsigned width;
        for (width = 0u; width < 2u; ++width) {
            uint32_t reserved, before;
            turbowasm_component_value *fields;
            initialize(width != 0u, true, 16u);
            record_value(&values[0], 0u, 'x');
            fields = values[0].as.record.items;
            fields[1].kind = TURBOWASM_COMPONENT_TYPE_STRING;
            fields[1].as.string.data = turbowasm_rt_malloc(3u); check_not_null(fields[1].as.string.data);
            memcpy(fields[1].as.string.data, "abc", 3u); fields[1].as.string.size = 3u;
            probe_reentry = true; before = reentry_count;
            check_equal(turbowasm_component_canonical_lower_value(&graph, indexed(3u), &memory, 16u, &values[0]), TURBOWASM_OK);
            probe_reentry = false;
            check_equal(reentry_count, before + 1u); check_equal(reentry_status, TURBOWASM_TRAPPED);
            reserved = read_word(16u); check_equal(read_word(width ? 24u : 20u), 65536u);
            check_equal(table.capacity, 16u);
            check_equal(turbowasm_component_endpoint_codec_commit(&codec), TURBOWASM_OK);
            check_equal(reader[0].waitable.handle, reserved);
            check_equal(turbowasm_component_canonical_lift_value(&graph, indexed(3u), &memory, 16u, &values[1]), TURBOWASM_OK);
            check_equal(values[1].as.record.items[1].as.string.size, (size_t)3u);
            check_equal(values[1].as.record.items[1].as.string.data, (const uint8_t *)"abc", 3u);
            check_equal(turbowasm_component_value_destroy(&values[0]), TURBOWASM_OK);
            check_false(reader[0].closed);
            cleanup();
        }
    }

    it("commits and lifts multiple endpoint owners nested in a guest list") {
        unsigned width;
        for (width = 0u; width < 2u; ++width) {
            turbowasm_component_value *records;
            initialize(width != 0u, false, 4u);
            records = turbowasm_rt_calloc(2u, sizeof(*records)); check_not_null(records);
            values[0].kind = TURBOWASM_COMPONENT_TYPE_LIST;
            values[0].as.list.items = records; values[0].as.list.count = 2u;
            record_value(&records[0], 0u, 'a'); record_value(&records[1], 1u, 'b');
            check_equal(turbowasm_component_canonical_lower_value(&graph, indexed(2u), &memory, 16u, &values[0]), TURBOWASM_OK);
            check_equal(table.live_count, 2u); check_not_null(codec.lower_head->lower_next);
            check_equal(turbowasm_component_endpoint_codec_commit(&codec), TURBOWASM_OK);
            check_equal(turbowasm_component_canonical_lift_value(&graph, indexed(2u), &memory, 16u, &values[1]), TURBOWASM_OK);
            check_equal(table.live_count, 0u);
            check_equal(values[1].as.list.count, UINT64_C(2));
            check_equal(values[1].as.list.items[1].as.record.items[1].as.character, (uint32_t)'b');
            check_equal(turbowasm_component_value_destroy(&values[0]), TURBOWASM_OK);
            check_false(reader[0].closed); check_false(reader[1].closed);
            check_equal(turbowasm_component_value_destroy(&values[1]), TURBOWASM_OK);
            check_true(reader[0].closed); check_true(reader[1].closed);
            cleanup();
        }
    }

    it("rejects standalone notification objects and writable handles as readable endpoints") {
        uint32_t handle;
        initialize(false, true, 4u);
        check_equal(turbowasm_component_waitable_register(&table, TURBOWASM_COMPONENT_HANDLE_FUTURE_READ,
            &standalone_waitable), TURBOWASM_OK);
        handle = standalone_waitable.handle;
        check_equal(turbowasm_component_endpoint_codec_lift(&codec, &graph, indexed(0u), handle, &values[0]), TURBOWASM_TRAPPED);
        check_equal(standalone_waitable.handle, handle);
        open_pair(0u, false);
        check_equal(turbowasm_component_waitable_register(&table, TURBOWASM_COMPONENT_HANDLE_FUTURE_WRITE,
            &writer[0].waitable), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_lift(&codec, &graph, indexed(0u), writer[0].waitable.handle,
            &values[0]), TURBOWASM_TRAPPED);
        check_equal(table.live_count, 2u);
    }

    it("preserves a peer-close notification while lower ownership is reserved") {
        turbowasm_component_event event;
        uint32_t handle;
        initialize(false, false, 4u); host_value(0u, &values[0]);
        check_equal(turbowasm_component_endpoint_codec_lower(&codec, &graph, indexed(0u), &values[0], &handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_close(&writer[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_endpoint_codec_commit(&codec), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&values[0]), TURBOWASM_OK);
        check_false(reader[0].closed);
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_OK);
        check_equal(event.handle, handle); check_equal(event.code, TURBOWASM_COMPONENT_EVENT_STREAM_READ);
        check_equal(event.payload, TURBOWASM_COMPONENT_COPY_DROPPED);
        check_equal(turbowasm_component_waitable_take(&table, handle, &event), TURBOWASM_YIELDED);
    }
}
