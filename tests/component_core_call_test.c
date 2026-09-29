#include "../src/component_core_call.h"
#include "../src/instance_internal.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

static const uint8_t string_len32_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
    0x03,0x02,0x01,0x00,
    0x05,0x03,0x01,0x00,0x01,
    0x0a,0x06,0x01,0x04,0x00,0x20,0x01,0x0b
};

static const uint8_t string_len64_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x07,0x01,0x60,0x02,0x7e,0x7e,0x01,0x7e,
    0x03,0x02,0x01,0x00,
    0x05,0x03,0x01,0x04,0x01,
    0x0a,0x06,0x01,0x04,0x00,0x20,0x01,0x0b
};

static const uint8_t indirect_params_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,
    0x03,0x02,0x01,0x00,
    0x05,0x03,0x01,0x00,0x01,
    0x0a,0x09,0x01,0x07,0x00,0x20,0x00,0x28,0x02,0x00,0x0b
};

static const uint8_t indirect_string_result_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
    0x03,0x02,0x01,0x00,
    0x05,0x03,0x01,0x00,0x01,
    0x0a,0x07,0x01,0x05,0x00,0x41,0xc0,0x00,0x0b
};

static const uint8_t trap_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
    0x03,0x02,0x01,0x00,
    0x0a,0x05,0x01,0x03,0x00,0x00,0x0b
};

typedef struct bump_allocator {
    turbowasm_instance_impl *instance;
    uint32_t memory_index;
    uint64_t cursor;
} bump_allocator;

static uint64_t align_to(uint64_t value, uint64_t alignment) {
    uint64_t mask = alignment - 1u;
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
        alignment == 0u ||
        (alignment & (alignment - 1u)) != 0u)
        return TURBOWASM_INVALID_ARGUMENT;
    if (allocator->cursor > UINT64_MAX - (alignment - 1u))
        return TURBOWASM_TRAPPED;

    pointer = align_to(allocator->cursor, alignment);
    if (new_size > UINT64_MAX - pointer)
        return TURBOWASM_TRAPPED;
    if (new_size > (uint64_t)SIZE_MAX)
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

static void init_memory(
    turbowasm_instance *instance,
    turbowasm_component_pointer_type pointer_type,
    uint64_t cursor,
    turbowasm_component_canonical_memory *memory,
    bump_allocator *allocator) {
    memset(memory, 0, sizeof(*memory));
    memset(allocator, 0, sizeof(*allocator));

    allocator->instance =
        (turbowasm_instance_impl *)instance->impl;
    allocator->memory_index = 0u;
    allocator->cursor = cursor;

    memory->instance = instance;
    memory->memory_index = 0u;
    memory->pointer_type = pointer_type;
    memory->string_encoding = TURBOWASM_COMPONENT_STRING_UTF8;
    memory->guest_realloc = bump_realloc;
    memory->realloc_context = allocator;
}

static void test_string_direct_memory32(void) {
    static const uint8_t text[] = {'h','e','l','l','o'};
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_type_ref params[1];
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_value argument = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    params[0] = inline_ref(TURBOWASM_COMPONENT_TYPE_STRING);
    assert(turbowasm_component_type_graph_allocate(&graph, 1u));
    assert(turbowasm_component_type_graph_define_function(
        &graph, 0u, params, 1u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_U32)));

    assert(turbowasm_module_load_borrowed(
        &module, string_len32_module,
        sizeof(string_len32_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
        &instance, &module) == TURBOWASM_OK);
    init_memory(
        &instance, TURBOWASM_COMPONENT_POINTER_I32,
        256u, &memory, &allocator);

    assert(turbowasm_component_core_call_adapter_init(
        &adapter, &graph, 0u, &instance, 0u, &memory) ==
        TURBOWASM_OK);

    argument.kind = TURBOWASM_COMPONENT_TYPE_STRING;
    argument.as.string.data = (uint8_t *)text;
    argument.as.string.size = sizeof(text);

    assert(turbowasm_component_core_call_invoke(
        &adapter, &argument, 1u, &result, &trap) ==
        TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == sizeof(text));

    turbowasm_component_value_destroy(&result);
    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_string_direct_memory64(void) {
    static const uint8_t text[] = {'6','4','!'};
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_type_ref params[1];
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_value argument = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    params[0] = inline_ref(TURBOWASM_COMPONENT_TYPE_STRING);
    assert(turbowasm_component_type_graph_allocate(&graph, 1u));
    assert(turbowasm_component_type_graph_define_function(
        &graph, 0u, params, 1u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_U64)));

    assert(turbowasm_module_load_borrowed(
        &module, string_len64_module,
        sizeof(string_len64_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
        &instance, &module) == TURBOWASM_OK);
    init_memory(
        &instance, TURBOWASM_COMPONENT_POINTER_I64,
        256u, &memory, &allocator);

    assert(turbowasm_component_core_call_adapter_init(
        &adapter, &graph, 0u, &instance, 0u, &memory) ==
        TURBOWASM_OK);

    argument.kind = TURBOWASM_COMPONENT_TYPE_STRING;
    argument.as.string.data = (uint8_t *)text;
    argument.as.string.size = sizeof(text);

    assert(turbowasm_component_core_call_invoke(
        &adapter, &argument, 1u, &result, &trap) ==
        TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U64);
    assert(result.as.u64 == sizeof(text));

    turbowasm_component_value_destroy(&result);
    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_indirect_parameters(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_type_ref params[17];
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_value arguments[17] = {{0}};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    uint32_t i;

    for (i = 0u; i < 17u; ++i)
        params[i] = inline_ref(TURBOWASM_COMPONENT_TYPE_U32);

    assert(turbowasm_component_type_graph_allocate(&graph, 1u));
    assert(turbowasm_component_type_graph_define_function(
        &graph, 0u, params, 17u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_U32)));

    assert(turbowasm_module_load_borrowed(
        &module, indirect_params_module,
        sizeof(indirect_params_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
        &instance, &module) == TURBOWASM_OK);
    init_memory(
        &instance, TURBOWASM_COMPONENT_POINTER_I32,
        256u, &memory, &allocator);

    assert(turbowasm_component_core_call_adapter_init(
        &adapter, &graph, 0u, &instance, 0u, &memory) ==
        TURBOWASM_OK);
    assert(adapter.flat_signature.params_indirect);

    for (i = 0u; i < 17u; ++i) {
        arguments[i].kind = TURBOWASM_COMPONENT_TYPE_U32;
        arguments[i].as.u32 = 100u + i;
    }

    assert(turbowasm_component_core_call_invoke(
        &adapter, arguments, 17u, &result, &trap) ==
        TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == 100u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_indirect_string_result(void) {
    static const uint8_t ok[] = {'o','k'};
    turbowasm_component_type_graph graph = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_value prepared = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_component_type_graph_allocate(&graph, 1u));
    assert(turbowasm_component_type_graph_define_function(
        &graph, 0u, NULL, 0u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_STRING)));

    assert(turbowasm_module_load_borrowed(
        &module, indirect_string_result_module,
        sizeof(indirect_string_result_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
        &instance, &module) == TURBOWASM_OK);
    init_memory(
        &instance, TURBOWASM_COMPONENT_POINTER_I32,
        128u, &memory, &allocator);

    prepared.kind = TURBOWASM_COMPONENT_TYPE_STRING;
    prepared.as.string.data = (uint8_t *)ok;
    prepared.as.string.size = sizeof(ok);
    assert(turbowasm_component_canonical_lower_value(
        &graph,
        inline_ref(TURBOWASM_COMPONENT_TYPE_STRING),
        &memory, 64u, &prepared) == TURBOWASM_OK);

    assert(turbowasm_component_core_call_adapter_init(
        &adapter, &graph, 0u, &instance, 0u, &memory) ==
        TURBOWASM_OK);
    assert(adapter.flat_signature.results_indirect);

    assert(turbowasm_component_core_call_invoke(
        &adapter, NULL, 0u, &result, &trap) ==
        TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_STRING);
    assert(result.as.string.size == sizeof(ok));
    assert(memcmp(result.as.string.data, ok, sizeof(ok)) == 0);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_signature_mismatch_and_trap(void) {
    turbowasm_component_type_graph mismatch_graph = {0};
    turbowasm_component_type_ref mismatch_params[1];
    turbowasm_component_type_graph trap_graph = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    mismatch_params[0] =
        inline_ref(TURBOWASM_COMPONENT_TYPE_U64);
    assert(turbowasm_component_type_graph_allocate(
        &mismatch_graph, 1u));
    assert(turbowasm_component_type_graph_define_function(
        &mismatch_graph, 0u, mismatch_params, 1u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_U64)));

    assert(turbowasm_module_load_borrowed(
        &module, string_len32_module,
        sizeof(string_len32_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
        &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_component_core_call_adapter_init(
        &adapter, &mismatch_graph, 0u,
        &instance, 0u, NULL) == TURBOWASM_TYPE_MISMATCH);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_type_graph_destroy(&mismatch_graph);

    assert(turbowasm_component_type_graph_allocate(
        &trap_graph, 1u));
    assert(turbowasm_component_type_graph_define_function(
        &trap_graph, 0u, NULL, 0u, true,
        inline_ref(TURBOWASM_COMPONENT_TYPE_U32)));
    assert(turbowasm_module_load_borrowed(
        &module, trap_module, sizeof(trap_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
        &instance, &module) == TURBOWASM_OK);
    assert(turbowasm_component_core_call_adapter_init(
        &adapter, &trap_graph, 0u,
        &instance, 0u, NULL) == TURBOWASM_OK);
    assert(turbowasm_component_core_call_invoke(
        &adapter, NULL, 0u, &result, &trap) ==
        TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_UNREACHABLE);

    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_type_graph_destroy(&trap_graph);
}

static void test_resource_types_deferred_to_c5b(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_type_ref params[1];
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_component_core_call_adapter adapter = {0};

    assert(turbowasm_component_type_graph_allocate(&graph, 3u));
    assert(turbowasm_component_type_graph_define_resource_full(
        &graph, 0u, UINT64_C(55), 0x7fu, false, UINT32_MAX));
    assert(turbowasm_component_type_graph_define_handle(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_OWN, 0u));
    params[0] = turbowasm_component_type_ref_indexed(1u);
    assert(turbowasm_component_type_graph_define_function(
        &graph, 2u, params, 1u, false,
        inline_ref(TURBOWASM_COMPONENT_TYPE_BOOL)));

    assert(turbowasm_module_load_borrowed(
        &module, string_len32_module,
        sizeof(string_len32_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
        &instance, &module) == TURBOWASM_OK);

    assert(turbowasm_component_core_call_adapter_init(
        &adapter, &graph, 2u,
        &instance, 0u, NULL) == TURBOWASM_UNSUPPORTED);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_type_graph_destroy(&graph);
}

int main(void) {
    test_string_direct_memory32();
    test_string_direct_memory64();
    test_indirect_parameters();
    test_indirect_string_result();
    test_signature_mismatch_and_trap();
    test_resource_types_deferred_to_c5b();
    return 0;
}
