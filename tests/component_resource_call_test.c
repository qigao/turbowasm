#include "../src/component_core_call.h"
#include "../src/component_resource_binding.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

static const uint8_t identity_i32_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,
    0x03,0x02,0x01,0x00,
    0x0a,0x06,0x01,0x04,0x00,0x20,0x00,0x0b
};

/*
 * imports:
 *   r.rep  : (i32 handle) -> i32 rep
 *   r.drop : (i32 handle) -> ()
 * export target body:
 *   rep = r.rep(handle)
 *   r.drop(handle)
 *   return rep
 */
static const uint8_t borrow_use_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x0f,0x03,
      0x60,0x01,0x7f,0x01,0x7f,
      0x60,0x01,0x7f,0x00,
      0x60,0x01,0x7f,0x01,0x7f,
    0x02,0x12,0x02,
      0x01,0x72,0x03,0x72,0x65,0x70,0x00,0x00,
      0x01,0x72,0x04,0x64,0x72,0x6f,0x70,0x00,0x01,
    0x03,0x02,0x01,0x02,
    0x0a,0x12,0x01,0x10,
      0x01,0x01,0x7f,
      0x20,0x00,
      0x10,0x00,
      0x21,0x01,
      0x20,0x00,
      0x10,0x01,
      0x20,0x01,
      0x0b
};

/* imports r.drop:(i32)->(); defined target:(i32)->() calls it. */
static const uint8_t owned_drop_module[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x0a,0x02,
      0x60,0x01,0x7f,0x00,
      0x60,0x01,0x7f,0x00,
    0x02,0x0b,0x01,
      0x01,0x72,0x04,0x64,0x72,0x6f,0x70,0x00,0x00,
    0x03,0x02,0x01,0x01,
    0x0a,0x08,0x01,0x06,
      0x00,0x20,0x00,0x10,0x00,0x0b
};

typedef struct destructor_probe {
    uint32_t calls;
    turbowasm_value last_rep;
} destructor_probe;

typedef struct host_resource_context {
    turbowasm_component_resource_binding *binding;
} host_resource_context;

static turbowasm_value i32_rep(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
    return out;
}

static turbowasm_status resource_destructor(
    void *context,
    uint64_t resource_identity,
    turbowasm_value rep) {
    destructor_probe *probe = (destructor_probe *)context;
    assert(probe != NULL);
    assert(resource_identity == UINT64_C(0x5001));
    ++probe->calls;
    probe->last_rep = rep;
    return TURBOWASM_OK;
}

static turbowasm_status host_resource_rep(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    host_resource_context *host = (host_resource_context *)context;
    turbowasm_status status;
    (void)call;

    if (host == NULL || host->binding == NULL ||
        arguments == NULL || argument_count != 1u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        results == NULL || result_capacity < 1u ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *trap = TURBOWASM_TRAP_NONE;
    status = turbowasm_component_resource_binding_rep(
        host->binding,
        (uint32_t)arguments[0].as.i32,
        &results[0]);
    if (status != TURBOWASM_OK)
        return status;
    *result_count = 1u;
    return TURBOWASM_OK;
}

static turbowasm_status host_resource_drop(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    host_resource_context *host = (host_resource_context *)context;
    (void)call;
    (void)results;
    (void)result_capacity;

    if (host == NULL || host->binding == NULL ||
        arguments == NULL || argument_count != 1u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    *trap = TURBOWASM_TRAP_NONE;
    *result_count = 0u;
    return turbowasm_component_resource_binding_drop(
        host->binding,
        (uint32_t)arguments[0].as.i32);
}

static void build_resource_graph(
    turbowasm_component_type_graph *graph,
    bool borrow_parameter,
    bool own_result) {
    turbowasm_component_type_ref params[1];
    turbowasm_component_type_ref result =
        turbowasm_component_type_ref_inline(
            TURBOWASM_COMPONENT_TYPE_U32);

    assert(turbowasm_component_type_graph_allocate(graph, 4u));
    assert(turbowasm_component_type_graph_define_resource_full(
        graph, 0u, UINT64_C(0x5001), 0x7fu, true, 0u));
    assert(turbowasm_component_type_graph_define_handle(
        graph, 1u, TURBOWASM_COMPONENT_TYPE_OWN, 0u));
    assert(turbowasm_component_type_graph_define_handle(
        graph, 2u, TURBOWASM_COMPONENT_TYPE_BORROW, 0u));

    params[0] = turbowasm_component_type_ref_indexed(
        borrow_parameter ? 2u : 1u);
    if (own_result)
        result = turbowasm_component_type_ref_indexed(1u);

    assert(turbowasm_component_type_graph_define_function(
        graph, 3u, params, 1u, true, result));
    assert(turbowasm_component_type_graph_validate(graph));
}

static void define_resource_hosts(
    turbowasm_linker *linker,
    host_resource_context *host,
    bool define_rep) {
    static const turbowasm_value_kind i32_param[] = {
        TURBOWASM_VALUE_I32
    };
    static const turbowasm_value_kind i32_result[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type rep_type = {
        i32_param, 1u, i32_result, 1u
    };
    const turbowasm_host_function_type drop_type = {
        i32_param, 1u, NULL, 0u
    };
    const turbowasm_name module = {
        (const uint8_t *)"r", 1u
    };
    const turbowasm_name rep_name = {
        (const uint8_t *)"rep", 3u
    };
    const turbowasm_name drop_name = {
        (const uint8_t *)"drop", 4u
    };

    if (define_rep) {
        assert(turbowasm_linker_define_host_function(
            linker, module, rep_name,
            &rep_type, host_resource_rep, host) == TURBOWASM_OK);
    }
    assert(turbowasm_linker_define_host_function(
        linker, module, drop_name,
        &drop_type, host_resource_drop, host) == TURBOWASM_OK);
}

static void test_own_round_trip(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_value argument = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    build_resource_graph(&graph, false, true);
    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_module_load_borrowed(
        &module, identity_i32_module,
        sizeof(identity_i32_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);

    assert(turbowasm_component_core_call_adapter_init_with_resources(
        &adapter, &graph, 3u,
        &instance, 0u, NULL, &table) == TURBOWASM_OK);

    argument.kind = TURBOWASM_COMPONENT_TYPE_OWN;
    argument.as.resource_rep = i32_rep(77);

    assert(turbowasm_component_core_call_invoke(
        &adapter, &argument, 1u, &result, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_OWN);
    assert(result.as.resource_rep.kind == TURBOWASM_VALUE_I32);
    assert(result.as.resource_rep.as.i32 == 77);
    assert(table.live_count == 0u);

    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_borrow_scope_is_explicitly_dropped(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_binding binding = {0};
    destructor_probe probe = {0};
    host_resource_context host = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_value argument = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    build_resource_graph(&graph, true, false);
    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_component_resource_binding_init(
        &binding, &graph, 0u, &table,
        resource_destructor, &probe) == TURBOWASM_OK);
    host.binding = &binding;

    assert(turbowasm_module_load_borrowed(
        &module, borrow_use_module,
        sizeof(borrow_use_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    define_resource_hosts(&linker, &host, true);
    assert(turbowasm_instance_create_linked(
        &instance, &module, &linker) == TURBOWASM_OK);

    assert(turbowasm_component_core_call_adapter_init_with_resources(
        &adapter, &graph, 3u,
        &instance, 2u, NULL, &table) == TURBOWASM_OK);

    argument.kind = TURBOWASM_COMPONENT_TYPE_BORROW;
    argument.as.resource_rep = i32_rep(123);

    assert(turbowasm_component_core_call_invoke(
        &adapter, &argument, 1u, &result, &trap) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(result.as.u32 == 123u);
    assert(table.live_count == 0u);
    assert(probe.calls == 0u);

    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_component_resource_binding_destroy(&binding);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_undropped_borrow_traps_and_cleans_up(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_value argument = {0};
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    build_resource_graph(&graph, true, false);
    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_module_load_borrowed(
        &module, identity_i32_module,
        sizeof(identity_i32_module)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(&instance, &module) == TURBOWASM_OK);
    assert(turbowasm_component_core_call_adapter_init_with_resources(
        &adapter, &graph, 3u,
        &instance, 0u, NULL, &table) == TURBOWASM_OK);

    argument.kind = TURBOWASM_COMPONENT_TYPE_BORROW;
    argument.as.resource_rep = i32_rep(9);

    assert(turbowasm_component_core_call_invoke(
        &adapter, &argument, 1u, &result, &trap) ==
        TURBOWASM_TRAPPED);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(table.live_count == 0u);

    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_owned_drop_runs_destructor(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_binding binding = {0};
    destructor_probe probe = {0};
    host_resource_context host = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_component_core_call_adapter adapter = {0};
    turbowasm_component_value argument = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_component_type_ref param;

    assert(turbowasm_component_type_graph_allocate(&graph, 3u));
    assert(turbowasm_component_type_graph_define_resource_full(
        &graph, 0u, UINT64_C(0x5001), 0x7fu, true, 0u));
    assert(turbowasm_component_type_graph_define_handle(
        &graph, 1u, TURBOWASM_COMPONENT_TYPE_OWN, 0u));
    param = turbowasm_component_type_ref_indexed(1u);
    assert(turbowasm_component_type_graph_define_function(
        &graph, 2u, &param, 1u, false,
        turbowasm_component_type_ref_inline(
            TURBOWASM_COMPONENT_TYPE_BOOL)));
    assert(turbowasm_component_type_graph_validate(&graph));

    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_component_resource_binding_init(
        &binding, &graph, 0u, &table,
        resource_destructor, &probe) == TURBOWASM_OK);
    host.binding = &binding;

    assert(turbowasm_module_load_borrowed(
        &module, owned_drop_module,
        sizeof(owned_drop_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    define_resource_hosts(&linker, &host, false);
    assert(turbowasm_instance_create_linked(
        &instance, &module, &linker) == TURBOWASM_OK);
    assert(turbowasm_component_core_call_adapter_init_with_resources(
        &adapter, &graph, 2u,
        &instance, 1u, NULL, &table) == TURBOWASM_OK);

    argument.kind = TURBOWASM_COMPONENT_TYPE_OWN;
    argument.as.resource_rep = i32_rep(456);

    assert(turbowasm_component_core_call_invoke(
        &adapter, &argument, 1u, NULL, &trap) == TURBOWASM_OK);
    assert(probe.calls == 1u);
    assert(probe.last_rep.kind == TURBOWASM_VALUE_I32);
    assert(probe.last_rep.as.i32 == 456);
    assert(table.live_count == 0u);

    turbowasm_component_core_call_adapter_destroy(&adapter);
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_component_resource_binding_destroy(&binding);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

static void test_resource_binding_rep_kind_and_identity(void) {
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_binding binding = {0};
    turbowasm_component_resource_handle handle = 0u;
    turbowasm_value rep = {0};

    assert(turbowasm_component_type_graph_allocate(&graph, 1u));
    assert(turbowasm_component_type_graph_define_resource_full(
        &graph, 0u, UINT64_C(0x5001), 0x7fu, false, UINT32_MAX));
    assert(turbowasm_component_type_graph_validate(&graph));
    assert(turbowasm_component_resource_table_init(&table, 2u));
    assert(turbowasm_component_resource_binding_init(
        &binding, &graph, 0u, &table,
        NULL, NULL) == TURBOWASM_OK);

    rep.kind = TURBOWASM_VALUE_I64;
    rep.as.i64 = 1;
    assert(turbowasm_component_resource_binding_new(
        &binding, rep, &handle) == TURBOWASM_TYPE_MISMATCH);

    rep = i32_rep(11);
    assert(turbowasm_component_resource_binding_new(
        &binding, rep, &handle) == TURBOWASM_OK);
    assert(turbowasm_component_resource_binding_rep(
        &binding, handle, &rep) == TURBOWASM_OK);
    assert(rep.as.i32 == 11);
    assert(turbowasm_component_resource_binding_drop(
        &binding, handle) == TURBOWASM_OK);

    turbowasm_component_resource_binding_destroy(&binding);
    turbowasm_component_resource_table_destroy(&table);
    turbowasm_component_type_graph_destroy(&graph);
}

int main(void) {
    test_own_round_trip();
    test_borrow_scope_is_explicitly_dropped();
    test_undropped_borrow_traps_and_cleans_up();
    test_owned_drop_runs_destructor();
    test_resource_binding_rep_kind_and_identity();
    return 0;
}
