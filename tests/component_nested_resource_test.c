#include "../src/component_core_call.h"
#include "../src/component_resource_binding.h"
#include "../src/instance_internal.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

static const uint8_t use_list32_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    /* type0: (i32,i32)->i32 */
    0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
    /* import h.use type0 */
    0x02,0x09,0x01,0x01,'h',0x03,'u','s','e',0x00,0x00,
    /* memory32 min=1 */
    0x05,0x03,0x01,0x00,0x01
};

static const uint8_t use_list64_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    /* type0: (i64,i64)->i32 */
    0x01,0x07,0x01,0x60,0x02,0x7e,0x7e,0x01,0x7f,
    /* import h.use type0 */
    0x02,0x09,0x01,0x01,'h',0x03,'u','s','e',0x00,0x00,
    /* memory64 min=1 */
    0x05,0x03,0x01,0x04,0x01
};

static const uint8_t make_list_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    /* type0: ()->i32 */
    0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
    /* import h.make type0 */
    0x02,0x0a,0x01,0x01,'h',0x04,'m','a','k','e',0x00,0x00,
    /* memory32 min=1 */
    0x05,0x03,0x01,0x00,0x01
};

static const uint8_t indirect_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    /* type0: (i32)->i32 */
    0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,
    /* import h.indirect type0 */
    0x02,0x0e,0x01,0x01,'h',0x08,
      'i','n','d','i','r','e','c','t',0x00,0x00,
    /* memory32 min=1 */
    0x05,0x03,0x01,0x00,0x01
};

typedef struct bump_allocator {
    turbowasm_instance_impl *instance;
    uint32_t memory_index;
    uint64_t cursor;
} bump_allocator;

typedef struct use_context {
    turbowasm_component_resource_binding *binding;
    bool pointer64;
    bool drop_handle;
} use_context;

typedef struct make_context {
    turbowasm_component_resource_binding *binding;
} make_context;

typedef struct indirect_context {
    turbowasm_component_resource_binding *binding;
} indirect_context;

static turbowasm_name name_span(const char *text, uint32_t size) {
    turbowasm_name name = {(const uint8_t *)text, size};
    return name;
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) |
           ((uint32_t)p[3] << 24u);
}

static void write_u32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8u);
    p[2] = (uint8_t)(value >> 16u);
    p[3] = (uint8_t)(value >> 24u);
}

static uint64_t value_u64(
    const turbowasm_value *value,
    bool pointer64) {
    if (pointer64) {
        assert(value->kind == TURBOWASM_VALUE_I64);
        return (uint64_t)value->as.i64;
    }
    assert(value->kind == TURBOWASM_VALUE_I32);
    return (uint32_t)value->as.i32;
}

static uint64_t align_up(uint64_t value, uint64_t alignment) {
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
    pointer = align_up(allocator->cursor, alignment);
    if (new_size > UINT64_MAX - pointer ||
        new_size > (uint64_t)SIZE_MAX)
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

static turbowasm_status host_use(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    use_context *use = (use_context *)context;
    turbowasm_host_memory_span span = {0};
    turbowasm_value rep = {0};
    uint64_t pointer;
    uint64_t count;
    uint32_t handle;
    turbowasm_status status;

    if (use == NULL || use->binding == NULL ||
        arguments == NULL || argument_count != 2u ||
        results == NULL || result_capacity < 1u ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *trap = TURBOWASM_TRAP_NONE;
    pointer = value_u64(&arguments[0], use->pointer64);
    count = value_u64(&arguments[1], use->pointer64);
    if (count != 1u)
        return TURBOWASM_TRAPPED;

    status = turbowasm_host_call_memory_span64(
        call, 0u, pointer, 4u, &span, trap);
    if (status != TURBOWASM_OK)
        return status;

    handle = read_u32(span.data);
    status = turbowasm_component_resource_binding_rep(
        use->binding, handle, &rep);
    if (status != TURBOWASM_OK)
        return status;

    if (use->drop_handle) {
        status = turbowasm_component_resource_binding_drop(
            use->binding, handle);
        if (status != TURBOWASM_OK)
            return status;
    }

    results[0] = rep;
    *result_count = 1u;
    return TURBOWASM_OK;
}

static turbowasm_status host_make(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    make_context *make = (make_context *)context;
    turbowasm_component_resource_handle handle = 0u;
    turbowasm_host_memory_span element = {0};
    turbowasm_host_memory_span list = {0};
    turbowasm_value rep = {0};
    turbowasm_status status;

    (void)arguments;

    if (make == NULL || make->binding == NULL ||
        argument_count != 0u ||
        results == NULL || result_capacity < 1u ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *trap = TURBOWASM_TRAP_NONE;
    rep.kind = TURBOWASM_VALUE_I32;
    rep.as.i32 = 55;

    status = turbowasm_component_resource_binding_new(
        make->binding, rep, &handle);
    if (status != TURBOWASM_OK)
        return status;

    status = turbowasm_host_call_memory_span(
        call, 0u, 256u, 4u, &element, trap);
    if (status != TURBOWASM_OK)
        goto fail;
    write_u32(element.data, handle);

    status = turbowasm_host_call_memory_span(
        call, 0u, 128u, 8u, &list, trap);
    if (status != TURBOWASM_OK)
        goto fail;
    write_u32(list.data + 0u, 256u);
    write_u32(list.data + 4u, 1u);

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = 128;
    *result_count = 1u;
    return TURBOWASM_OK;

fail:
    (void)turbowasm_component_resource_binding_drop(
        make->binding, handle);
    return status;
}

static turbowasm_status host_indirect(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    indirect_context *indirect = (indirect_context *)context;
    turbowasm_host_memory_span tuple = {0};
    turbowasm_host_memory_span element = {0};
    turbowasm_value rep = {0};
    uint32_t pointer;
    uint32_t list_pointer;
    uint32_t list_count;
    uint32_t handle;
    turbowasm_status status;

    if (indirect == NULL || indirect->binding == NULL ||
        arguments == NULL || argument_count != 1u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        results == NULL || result_capacity < 1u ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *trap = TURBOWASM_TRAP_NONE;
    pointer = (uint32_t)arguments[0].as.i32;

    status = turbowasm_host_call_memory_span(
        call, 0u, pointer, 8u, &tuple, trap);
    if (status != TURBOWASM_OK)
        return status;

    list_pointer = read_u32(tuple.data + 0u);
    list_count = read_u32(tuple.data + 4u);
    if (list_count != 1u)
        return TURBOWASM_TRAPPED;

    status = turbowasm_host_call_memory_span(
        call, 0u, list_pointer, 4u, &element, trap);
    if (status != TURBOWASM_OK)
        return status;

    handle = read_u32(element.data);
    status = turbowasm_component_resource_binding_rep(
        indirect->binding, handle, &rep);
    if (status != TURBOWASM_OK)
        return status;
    status = turbowasm_component_resource_binding_drop(
        indirect->binding, handle);
    if (status != TURBOWASM_OK)
        return status;

    results[0] = rep;
    *result_count = 1u;
    return TURBOWASM_OK;
}

static void define_host(
    turbowasm_linker *linker,
    const char *function_name,
    const turbowasm_host_function_type *type,
    turbowasm_host_function_fn function,
    void *context) {
    assert(turbowasm_linker_define_host_function(
        linker,
        name_span("h", 1u),
        name_span(function_name, (uint32_t)strlen(function_name)),
        type,
        function,
        context) == TURBOWASM_OK);
}

static void init_memory(
    turbowasm_instance *instance,
    turbowasm_component_pointer_type pointer_type,
    bump_allocator *allocator,
    turbowasm_component_canonical_memory *memory) {
    memset(allocator, 0, sizeof(*allocator));
    memset(memory, 0, sizeof(*memory));

    allocator->instance =
        (turbowasm_instance_impl *)instance->impl;
    allocator->memory_index = 0u;
    allocator->cursor = 512u;

    memory->instance = instance;
    memory->memory_index = 0u;
    memory->pointer_type = pointer_type;
    memory->string_encoding = TURBOWASM_COMPONENT_STRING_UTF8;
    memory->guest_realloc = bump_realloc;
    memory->realloc_context = allocator;
}

static void build_list_graph(
    turbowasm_component_type_graph *graph,
    bool borrowed,
    bool list_result,
    uint32_t parameter_count) {
    turbowasm_component_type_ref *params = NULL;
    turbowasm_component_type_ref handle_ref;
    turbowasm_component_type_ref list_ref;
    turbowasm_component_type_ref result;
    uint32_t i;

    assert(turbowasm_component_type_graph_allocate(graph, 4u));
    assert(turbowasm_component_type_graph_define_resource_full(
        graph, 0u, UINT64_C(0x6001), 0x7fu, false, UINT32_MAX));
    assert(turbowasm_component_type_graph_define_handle(
        graph, 1u,
        borrowed
            ? TURBOWASM_COMPONENT_TYPE_BORROW
            : TURBOWASM_COMPONENT_TYPE_OWN,
        0u));

    handle_ref = turbowasm_component_type_ref_indexed(1u);
    assert(turbowasm_component_type_graph_define_list_ref(
        graph, 2u, handle_ref));
    list_ref = turbowasm_component_type_ref_indexed(2u);

    if (parameter_count != 0u) {
        params = (turbowasm_component_type_ref *)calloc(
            parameter_count, sizeof(*params));
        assert(params != NULL);
        params[0] = list_ref;
        for (i = 1u; i < parameter_count; ++i) {
            params[i] = turbowasm_component_type_ref_inline(
                TURBOWASM_COMPONENT_TYPE_U32);
        }
    }

    result = list_result
        ? list_ref
        : turbowasm_component_type_ref_inline(
            TURBOWASM_COMPONENT_TYPE_U32);

    assert(turbowasm_component_type_graph_define_function(
        graph, 3u,
        params, parameter_count,
        true, result));
    assert(turbowasm_component_type_graph_validate(graph));
    free(params);
}

static turbowasm_component_value one_resource_list(
    bool borrowed,
    int32_t rep_value) {
    turbowasm_component_value list = {0};
    turbowasm_component_value *items =
        (turbowasm_component_value *)calloc(1u, sizeof(*items));

    assert(items != NULL);
    items[0].kind = borrowed
        ? TURBOWASM_COMPONENT_TYPE_BORROW
        : TURBOWASM_COMPONENT_TYPE_OWN;
    items[0].as.resource_rep.kind = TURBOWASM_VALUE_I32;
    items[0].as.resource_rep.as.i32 = rep_value;

    list.kind = TURBOWASM_COMPONENT_TYPE_LIST;
    list.as.list.items = items;
    list.as.list.count = 1u;
    return list;
}

static void destroy_input_list(turbowasm_component_value *value) {
    if (value == NULL)
        return;
    free(value->as.list.items);
    memset(value, 0, sizeof(*value));
}

static void test_list_own_memory32(void) {
    static const turbowasm_value_kind params[] = {
        TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32
    };
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        params, 2u, results, 1u
    };
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_binding binding = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};
    use_context use = {0};
    turbowasm_component_value argument;
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    build_list_graph(&graph, false, false, 1u);
    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_component_resource_binding_init(
        &binding, &graph, 0u, &table, NULL, NULL) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
        &module, use_list32_module,
        sizeof(use_list32_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    use.binding = &binding;
    use.drop_handle = true;
    define_host(&linker, "use", &host_type, host_use, &use);
    assert(turbowasm_instance_create_linked(
        &instance, &module, &linker) == TURBOWASM_OK);

    init_memory(
        &instance, TURBOWASM_COMPONENT_POINTER_I32,
        &allocator, &memory);
    assert(turbowasm_component_core_call_adapter_init_with_resources(
        &adapter, &graph, 3u,
        &instance, 0u, &memory, &table) == TURBOWASM_OK);

    argument = one_resource_list(false, 71);
    assert(turbowasm_component_core_call_invoke(
        &adapter, &argument, 1u, &result, &trap) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == 71u);
    assert(table.live_count == 0u);

    destroy_input_list(&argument);
    turbowasm_component_value_destroy(&result);
    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_component_resource_binding_destroy(&binding);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_list_borrow_memory64(void) {
    static const turbowasm_value_kind params[] = {
        TURBOWASM_VALUE_I64, TURBOWASM_VALUE_I64
    };
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        params, 2u, results, 1u
    };
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_binding binding = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};
    use_context use = {0};
    turbowasm_component_value argument;
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    build_list_graph(&graph, true, false, 1u);
    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_component_resource_binding_init(
        &binding, &graph, 0u, &table, NULL, NULL) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
        &module, use_list64_module,
        sizeof(use_list64_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    use.binding = &binding;
    use.pointer64 = true;
    use.drop_handle = true;
    define_host(&linker, "use", &host_type, host_use, &use);
    assert(turbowasm_instance_create_linked(
        &instance, &module, &linker) == TURBOWASM_OK);

    init_memory(
        &instance, TURBOWASM_COMPONENT_POINTER_I64,
        &allocator, &memory);
    assert(turbowasm_component_core_call_adapter_init_with_resources(
        &adapter, &graph, 3u,
        &instance, 0u, &memory, &table) == TURBOWASM_OK);

    argument = one_resource_list(true, 81);
    assert(turbowasm_component_core_call_invoke(
        &adapter, &argument, 1u, &result, &trap) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == 81u);
    assert(table.live_count == 0u);

    destroy_input_list(&argument);
    turbowasm_component_value_destroy(&result);
    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_component_resource_binding_destroy(&binding);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_nested_borrow_leak_traps_and_cleans(void) {
    static const turbowasm_value_kind params[] = {
        TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32
    };
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        params, 2u, results, 1u
    };
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_binding binding = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};
    use_context use = {0};
    turbowasm_component_value argument;
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    build_list_graph(&graph, true, false, 1u);
    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_component_resource_binding_init(
        &binding, &graph, 0u, &table, NULL, NULL) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
        &module, use_list32_module,
        sizeof(use_list32_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    use.binding = &binding;
    use.drop_handle = false;
    define_host(&linker, "use", &host_type, host_use, &use);
    assert(turbowasm_instance_create_linked(
        &instance, &module, &linker) == TURBOWASM_OK);

    init_memory(
        &instance, TURBOWASM_COMPONENT_POINTER_I32,
        &allocator, &memory);
    assert(turbowasm_component_core_call_adapter_init_with_resources(
        &adapter, &graph, 3u,
        &instance, 0u, &memory, &table) == TURBOWASM_OK);

    argument = one_resource_list(true, 91);
    assert(turbowasm_component_core_call_invoke(
        &adapter, &argument, 1u, &result, &trap) ==
        TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(table.live_count == 0u);

    destroy_input_list(&argument);
    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_component_resource_binding_destroy(&binding);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_list_own_indirect_result(void) {
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        NULL, 0u, results, 1u
    };
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_binding binding = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};
    make_context make = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    build_list_graph(&graph, false, true, 0u);
    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_component_resource_binding_init(
        &binding, &graph, 0u, &table, NULL, NULL) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
        &module, make_list_module,
        sizeof(make_list_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    make.binding = &binding;
    define_host(&linker, "make", &host_type, host_make, &make);
    assert(turbowasm_instance_create_linked(
        &instance, &module, &linker) == TURBOWASM_OK);

    init_memory(
        &instance, TURBOWASM_COMPONENT_POINTER_I32,
        &allocator, &memory);
    assert(turbowasm_component_core_call_adapter_init_with_resources(
        &adapter, &graph, 3u,
        &instance, 0u, &memory, &table) == TURBOWASM_OK);
    assert(adapter.flat_signature.results_indirect);

    assert(turbowasm_component_core_call_invoke(
        &adapter, NULL, 0u, &result, &trap) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_LIST);
    assert(result.as.list.count == 1u);
    assert(result.as.list.items[0].kind ==
           TURBOWASM_COMPONENT_TYPE_OWN);
    assert(result.as.list.items[0].as.resource_rep.kind ==
           TURBOWASM_VALUE_I32);
    assert(result.as.list.items[0].as.resource_rep.as.i32 == 55);
    assert(table.live_count == 0u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_component_resource_binding_destroy(&binding);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_resource_list_in_indirect_tuple(void) {
    static const turbowasm_value_kind params[] = {
        TURBOWASM_VALUE_I32
    };
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        params, 1u, results, 1u
    };
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_binding binding = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_canonical_memory memory = {0};
    bump_allocator allocator = {0};
    indirect_context indirect = {0};
    turbowasm_component_value arguments[17] = {{0}};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    uint32_t i;

    build_list_graph(&graph, true, false, 17u);
    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_component_resource_binding_init(
        &binding, &graph, 0u, &table, NULL, NULL) == TURBOWASM_OK);

    assert(turbowasm_module_load_borrowed(
        &module, indirect_module,
        sizeof(indirect_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    indirect.binding = &binding;
    define_host(
        &linker, "indirect", &host_type,
        host_indirect, &indirect);
    assert(turbowasm_instance_create_linked(
        &instance, &module, &linker) == TURBOWASM_OK);

    init_memory(
        &instance, TURBOWASM_COMPONENT_POINTER_I32,
        &allocator, &memory);
    assert(turbowasm_component_core_call_adapter_init_with_resources(
        &adapter, &graph, 3u,
        &instance, 0u, &memory, &table) == TURBOWASM_OK);
    assert(adapter.flat_signature.params_indirect);

    arguments[0] = one_resource_list(true, 101);
    for (i = 1u; i < 17u; ++i) {
        arguments[i].kind = TURBOWASM_COMPONENT_TYPE_U32;
        arguments[i].as.u32 = i;
    }

    assert(turbowasm_component_core_call_invoke(
        &adapter, arguments, 17u, &result, &trap) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == 101u);
    assert(table.live_count == 0u);

    destroy_input_list(&arguments[0]);
    turbowasm_component_value_destroy(&result);
    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_component_resource_binding_destroy(&binding);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

int main(void) {
    test_list_own_memory32();
    test_list_borrow_memory64();
    test_nested_borrow_leak_traps_and_cleans();
    test_list_own_indirect_result();
    test_resource_list_in_indirect_tuple();
    return 0;
}
