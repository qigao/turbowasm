#include "../src/component_canonical.h"
#include "../src/instance_internal.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

static const uint8_t memory32_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x05,0x03,0x01,0x00,0x01
};

static const uint8_t memory64_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x05,0x03,0x01,0x04,0x01
};

typedef struct bump_allocator {
    turbowasm_instance_impl *instance;
    uint32_t memory_index;
    uint64_t cursor;
} bump_allocator;

static uint64_t align_to(uint64_t value, uint64_t alignment) {
    uint64_t mask;
    if (alignment <= 1u)
        return value;
    mask = alignment - 1u;
    return (value + mask) & ~mask;
}

static turbowasm_status bump_realloc(
    void *context,
    uint64_t old_pointer,
    uint64_t old_size,
    uint64_t alignment,
    uint64_t new_size,
    uint64_t *out_pointer) {
    bump_allocator *allocator = (bump_allocator *)context;
    uint64_t pointer;
    uint8_t *range = NULL;
    turbowasm_status status;

    (void)old_pointer;
    (void)old_size;

    if (allocator == NULL || out_pointer == NULL ||
        alignment == 0u || (alignment & (alignment - 1u)) != 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    if (allocator->cursor > UINT64_MAX - (alignment - 1u))
        return TURBOWASM_TRAPPED;
    pointer = align_to(allocator->cursor, alignment);
    if (new_size > UINT64_MAX - pointer)
        return TURBOWASM_TRAPPED;

    status = turbowasm_instance_memory_bounds(
        allocator->instance,
        allocator->memory_index,
        pointer, 0u, (size_t)new_size, &range);
    if (status != TURBOWASM_OK)
        return status;

    allocator->cursor = pointer + new_size;
    *out_pointer = pointer;
    return TURBOWASM_OK;
}

static turbowasm_component_type_ref inline_ref(
    turbowasm_component_type_kind kind) {
    return turbowasm_component_type_ref_inline(kind);
}

static void build_graph(turbowasm_component_type_graph *graph) {
    turbowasm_component_record_field fields[2];
    turbowasm_component_type_ref tuple_elements[2];
    turbowasm_component_type_ref unit = {0};
    static const uint8_t seconds[] = "seconds";
    static const uint8_t nanoseconds[] = "nanoseconds";

    assert(turbowasm_component_type_graph_allocate(graph, 8u));
    assert(turbowasm_component_type_graph_define_string(graph, 0u));
    assert(turbowasm_component_type_graph_define_list_ref(
        graph, 1u, inline_ref(TURBOWASM_COMPONENT_TYPE_STRING)));
    assert(turbowasm_component_type_graph_define_scalar(
        graph, 2u, TURBOWASM_COMPONENT_TYPE_U64));
    assert(turbowasm_component_type_graph_define_scalar(
        graph, 3u, TURBOWASM_COMPONENT_TYPE_U32));

    fields[0].name = seconds;
    fields[0].name_size = 7u;
    fields[0].type = turbowasm_component_type_ref_indexed(2u);
    fields[1].name = nanoseconds;
    fields[1].name_size = 11u;
    fields[1].type = turbowasm_component_type_ref_indexed(3u);
    assert(turbowasm_component_type_graph_define_record(
        graph, 4u, fields, 2u));

    tuple_elements[0] = turbowasm_component_type_ref_indexed(2u);
    tuple_elements[1] = turbowasm_component_type_ref_indexed(2u);
    assert(turbowasm_component_type_graph_define_tuple(
        graph, 5u, tuple_elements, 2u));

    assert(turbowasm_component_type_graph_define_option(
        graph, 6u, turbowasm_component_type_ref_indexed(0u)));
    assert(turbowasm_component_type_graph_define_result(
        graph, 7u, false, unit, false, unit));
    assert(turbowasm_component_type_graph_validate(graph));
}

static void init_memory(
    const uint8_t *bytes,
    size_t size,
    turbowasm_component_pointer_type pointer_type,
    turbowasm_module *module,
    turbowasm_instance *instance,
    turbowasm_component_canonical_memory *memory,
    bump_allocator *allocator) {
    assert(turbowasm_module_load_borrowed(
               module, bytes, size) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               instance, module) == TURBOWASM_OK);

    allocator->instance = (turbowasm_instance_impl *)instance->impl;
    allocator->memory_index = 0u;
    allocator->cursor = 256u;

    memset(memory, 0, sizeof(*memory));
    memory->instance = instance;
    memory->memory_index = 0u;
    memory->pointer_type = pointer_type;
    memory->string_encoding = TURBOWASM_COMPONENT_STRING_UTF8;
    memory->guest_realloc = bump_realloc;
    memory->realloc_context = allocator;
}

static void test_scalar_round_trip(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_canonical_memory *memory) {
    turbowasm_component_value in = {0};
    turbowasm_component_value out = {0};

    in.kind = TURBOWASM_COMPONENT_TYPE_U32;
    in.as.u32 = UINT32_C(0x78563412);
    assert(turbowasm_component_canonical_lower_value(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_U32),
               memory, 32u, &in) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_U32),
               memory, 32u, &out) == TURBOWASM_OK);
    assert(out.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(out.as.u32 == in.as.u32);
    turbowasm_component_value_destroy(&out);

    in.kind = TURBOWASM_COMPONENT_TYPE_F64;
    in.as.f64 = 3.5;
    assert(turbowasm_component_canonical_lower_value(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_F64),
               memory, 40u, &in) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_F64),
               memory, 40u, &out) == TURBOWASM_OK);
    assert(out.kind == TURBOWASM_COMPONENT_TYPE_F64);
    assert(out.as.f64 == 3.5);
    turbowasm_component_value_destroy(&out);
}

static void test_string_round_trip(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_canonical_memory *memory) {
    static const uint8_t hello[] = {
        'h','e','l','l','o',' ',0xe4,0xb8,0x96,0xe7,0x95,0x8c
    };
    turbowasm_component_value in = {0};
    turbowasm_component_value out = {0};

    in.kind = TURBOWASM_COMPONENT_TYPE_STRING;
    in.as.string.data = (uint8_t *)hello;
    in.as.string.size = sizeof(hello);

    assert(turbowasm_component_canonical_lower_value(
               graph, inline_ref(TURBOWASM_COMPONENT_TYPE_STRING),
               memory, 64u, &in) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph, inline_ref(TURBOWASM_COMPONENT_TYPE_STRING),
               memory, 64u, &out) == TURBOWASM_OK);
    assert(out.kind == TURBOWASM_COMPONENT_TYPE_STRING);
    assert(out.as.string.size == sizeof(hello));
    assert(memcmp(out.as.string.data, hello, sizeof(hello)) == 0);
    turbowasm_component_value_destroy(&out);
}

static void test_nested_list_round_trip(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_canonical_memory *memory) {
    static const uint8_t a[] = {'a'};
    static const uint8_t bc[] = {'b','c'};
    turbowasm_component_value items[2] = {{0}};
    turbowasm_component_value in = {0};
    turbowasm_component_value out = {0};

    items[0].kind = TURBOWASM_COMPONENT_TYPE_STRING;
    items[0].as.string.data = (uint8_t *)a;
    items[0].as.string.size = sizeof(a);
    items[1].kind = TURBOWASM_COMPONENT_TYPE_STRING;
    items[1].as.string.data = (uint8_t *)bc;
    items[1].as.string.size = sizeof(bc);

    in.kind = TURBOWASM_COMPONENT_TYPE_LIST;
    in.as.list.items = items;
    in.as.list.count = 2u;

    assert(turbowasm_component_canonical_lower_value(
               graph, turbowasm_component_type_ref_indexed(1u),
               memory, 96u, &in) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph, turbowasm_component_type_ref_indexed(1u),
               memory, 96u, &out) == TURBOWASM_OK);

    assert(out.kind == TURBOWASM_COMPONENT_TYPE_LIST);
    assert(out.as.list.count == 2u);
    assert(out.as.list.items[0].kind ==
           TURBOWASM_COMPONENT_TYPE_STRING);
    assert(out.as.list.items[0].as.string.size == 1u);
    assert(out.as.list.items[0].as.string.data[0] == (uint8_t)'a');
    assert(out.as.list.items[1].as.string.size == 2u);
    assert(memcmp(
        out.as.list.items[1].as.string.data, bc, sizeof(bc)) == 0);

    turbowasm_component_value_destroy(&out);
}

static void test_composite_round_trip(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_canonical_memory *memory) {
    static const uint8_t cwd[] = {'/','t','m','p'};
    turbowasm_component_value record_items[2] = {{0}};
    turbowasm_component_value tuple_items[2] = {{0}};
    turbowasm_component_value option_payload = {0};
    turbowasm_component_value in = {0};
    turbowasm_component_value out = {0};

    record_items[0].kind = TURBOWASM_COMPONENT_TYPE_U64;
    record_items[0].as.u64 = UINT64_C(123456789);
    record_items[1].kind = TURBOWASM_COMPONENT_TYPE_U32;
    record_items[1].as.u32 = UINT32_C(987654321);
    in.kind = TURBOWASM_COMPONENT_TYPE_RECORD;
    in.as.record.items = record_items;
    in.as.record.count = 2u;

    assert(turbowasm_component_canonical_lower_value(
               graph, turbowasm_component_type_ref_indexed(4u),
               memory, 160u, &in) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph, turbowasm_component_type_ref_indexed(4u),
               memory, 160u, &out) == TURBOWASM_OK);
    assert(out.kind == TURBOWASM_COMPONENT_TYPE_RECORD);
    assert(out.as.record.count == 2u);
    assert(out.as.record.items[0].kind ==
           TURBOWASM_COMPONENT_TYPE_U64);
    assert(out.as.record.items[0].as.u64 ==
           UINT64_C(123456789));
    assert(out.as.record.items[1].kind ==
           TURBOWASM_COMPONENT_TYPE_U32);
    assert(out.as.record.items[1].as.u32 ==
           UINT32_C(987654321));
    turbowasm_component_value_destroy(&out);

    tuple_items[0].kind = TURBOWASM_COMPONENT_TYPE_U64;
    tuple_items[0].as.u64 = UINT64_C(11);
    tuple_items[1].kind = TURBOWASM_COMPONENT_TYPE_U64;
    tuple_items[1].as.u64 = UINT64_C(22);
    memset(&in, 0, sizeof(in));
    in.kind = TURBOWASM_COMPONENT_TYPE_TUPLE;
    in.as.tuple.items = tuple_items;
    in.as.tuple.count = 2u;

    assert(turbowasm_component_canonical_lower_value(
               graph, turbowasm_component_type_ref_indexed(5u),
               memory, 176u, &in) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph, turbowasm_component_type_ref_indexed(5u),
               memory, 176u, &out) == TURBOWASM_OK);
    assert(out.kind == TURBOWASM_COMPONENT_TYPE_TUPLE);
    assert(out.as.tuple.count == 2u);
    assert(out.as.tuple.items[0].as.u64 == UINT64_C(11));
    assert(out.as.tuple.items[1].as.u64 == UINT64_C(22));
    turbowasm_component_value_destroy(&out);

    option_payload.kind = TURBOWASM_COMPONENT_TYPE_STRING;
    option_payload.as.string.data = (uint8_t *)cwd;
    option_payload.as.string.size = sizeof(cwd);
    memset(&in, 0, sizeof(in));
    in.kind = TURBOWASM_COMPONENT_TYPE_OPTION;
    in.as.option.case_index = 1u;
    in.as.option.payload = &option_payload;

    assert(turbowasm_component_canonical_lower_value(
               graph, turbowasm_component_type_ref_indexed(6u),
               memory, 192u, &in) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph, turbowasm_component_type_ref_indexed(6u),
               memory, 192u, &out) == TURBOWASM_OK);
    assert(out.kind == TURBOWASM_COMPONENT_TYPE_OPTION);
    assert(out.as.option.case_index == 1u);
    assert(out.as.option.payload != NULL);
    assert(out.as.option.payload->kind ==
           TURBOWASM_COMPONENT_TYPE_STRING);
    assert(out.as.option.payload->as.string.size == sizeof(cwd));
    assert(memcmp(
        out.as.option.payload->as.string.data,
        cwd, sizeof(cwd)) == 0);
    turbowasm_component_value_destroy(&out);

    memset(&in, 0, sizeof(in));
    in.kind = TURBOWASM_COMPONENT_TYPE_OPTION;
    in.as.option.case_index = 0u;
    in.as.option.payload = NULL;
    assert(turbowasm_component_canonical_lower_value(
               graph, turbowasm_component_type_ref_indexed(6u),
               memory, 192u, &in) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph, turbowasm_component_type_ref_indexed(6u),
               memory, 192u, &out) == TURBOWASM_OK);
    assert(out.kind == TURBOWASM_COMPONENT_TYPE_OPTION);
    assert(out.as.option.case_index == 0u);
    assert(out.as.option.payload == NULL);
    turbowasm_component_value_destroy(&out);

    memset(&in, 0, sizeof(in));
    in.kind = TURBOWASM_COMPONENT_TYPE_RESULT;
    in.as.result.case_index = 1u;
    in.as.result.payload = NULL;
    assert(turbowasm_component_canonical_lower_value(
               graph, turbowasm_component_type_ref_indexed(7u),
               memory, 224u, &in) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph, turbowasm_component_type_ref_indexed(7u),
               memory, 224u, &out) == TURBOWASM_OK);
    assert(out.kind == TURBOWASM_COMPONENT_TYPE_RESULT);
    assert(out.as.result.case_index == 1u);
    assert(out.as.result.payload == NULL);
    turbowasm_component_value_destroy(&out);
}

static void test_fail_closed_inputs(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_canonical_memory *memory) {
    turbowasm_instance_impl *impl =
        (turbowasm_instance_impl *)memory->instance->impl;
    turbowasm_component_value value = {0};
    uint8_t invalid_bool = 2u;
    uint8_t invalid_char[4] = {0x00u, 0xd8u, 0x00u, 0x00u};
    uint8_t invalid_utf8[2] = {0xffu, 0xfeu};
    uint8_t pair[16] = {0};
    uint8_t invalid_variant = 2u;
    size_t pointer_width =
        memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64
            ? 8u : 4u;

    assert(turbowasm_instance_memory_write_bytes(
               impl, 0u, 120u, 0u,
               &invalid_bool, 1u) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_BOOL),
               memory, 120u, &value) == TURBOWASM_TRAPPED);

    assert(turbowasm_instance_memory_write_bytes(
               impl, 0u, 124u, 0u,
               invalid_char, sizeof(invalid_char)) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_CHAR),
               memory, 124u, &value) == TURBOWASM_TRAPPED);

    assert(turbowasm_instance_memory_write_bytes(
               impl, 0u, 512u, 0u,
               invalid_utf8, sizeof(invalid_utf8)) == TURBOWASM_OK);
    pair[0] = 0x00u;
    pair[1] = 0x02u; /* ptr = 512 little-endian */
    pair[pointer_width] = 2u;
    assert(turbowasm_instance_memory_write_bytes(
               impl, 0u, 128u, 0u,
               pair, pointer_width * 2u) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_STRING),
               memory, 128u, &value) == TURBOWASM_TRAPPED);

    assert(turbowasm_component_canonical_lift_value(
               graph,
               inline_ref(TURBOWASM_COMPONENT_TYPE_U64),
               memory, UINT64_MAX - 1u, &value) ==
           TURBOWASM_TRAPPED);

    assert(turbowasm_instance_memory_write_bytes(
               impl, 0u, 224u, 0u,
               &invalid_variant, 1u) == TURBOWASM_OK);
    assert(turbowasm_component_canonical_lift_value(
               graph,
               turbowasm_component_type_ref_indexed(7u),
               memory, 224u, &value) == TURBOWASM_TRAPPED);
}

static void run_memory_suite(
    const uint8_t *bytes,
    size_t size,
    turbowasm_component_pointer_type pointer_type) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};

    build_graph(&graph);
    init_memory(
        bytes, size, pointer_type,
        &module, &instance, &memory, &allocator);

    test_scalar_round_trip(&graph, &memory);
    test_string_round_trip(&graph, &memory);
    test_nested_list_round_trip(&graph, &memory);
    test_composite_round_trip(&graph, &memory);
    test_fail_closed_inputs(&graph, &memory);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_type_graph_destroy(&graph);
}

int main(void) {
    run_memory_suite(
        memory32_module, sizeof(memory32_module),
        TURBOWASM_COMPONENT_POINTER_I32);
    run_memory_suite(
        memory64_module, sizeof(memory64_module),
        TURBOWASM_COMPONENT_POINTER_I64);
    return 0;
}
