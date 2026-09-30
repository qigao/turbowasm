#include "../src/component_exec.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct import_probe {
    const char *instance_name;
    uint64_t value;
    uint32_t invoke_calls;

    bool resource_owner;
    uint32_t lower_calls;
    uint32_t lift_calls;
    uint32_t drop_calls;
    uint64_t drop_identity;
    uint32_t drop_handle;
} import_probe;

static bool name_is(
    turbowasm_component_name name,
    const char *text) {
    size_t size;

    if (text == NULL)
        return false;
    size = strlen(text);
    return name.size == size &&
           (size == 0u ||
            (name.bytes != NULL &&
             memcmp(name.bytes, text, size) == 0));
}

static bool probe_can_bind(
    void *context,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type) {
    import_probe *probe = (import_probe *)context;

    (void)graph;
    (void)function_type;

    return probe != NULL &&
           name_is(instance_name, probe->instance_name) &&
           name_is(function_name, "now");
}

static turbowasm_status probe_invoke(
    void *context,
    turbowasm_host_call *call,
    turbowasm_component_name instance_name,
    turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id function_type,
    const turbowasm_component_value *arguments,
    size_t argument_count,
    turbowasm_component_value *out_result,
    turbowasm_trap *trap) {
    import_probe *probe = (import_probe *)context;

    (void)call;

    if (probe == NULL || out_result == NULL ||
        trap == NULL || arguments != NULL ||
        argument_count != 0u ||
        !probe_can_bind(
            context,
            instance_name,
            function_name,
            graph,
            function_type))
        return TURBOWASM_TYPE_MISMATCH;

    ++probe->invoke_calls;
    memset(out_result, 0, sizeof(*out_result));
    out_result->kind = TURBOWASM_COMPONENT_TYPE_U64;
    out_result->as.u64 = probe->value;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status probe_resource_lower(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    const turbowasm_component_value *value,
    uint32_t *out_handle) {
    import_probe *probe = (import_probe *)context;

    (void)graph;
    (void)type;
    (void)value;

    if (probe == NULL || !probe->resource_owner)
        return TURBOWASM_TYPE_MISMATCH;
    if (out_handle == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    ++probe->lower_calls;
    *out_handle = 77u;
    return TURBOWASM_OK;
}

static turbowasm_status probe_resource_lift(
    void *context,
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref type,
    uint32_t handle,
    turbowasm_component_value *out) {
    import_probe *probe = (import_probe *)context;

    (void)graph;
    (void)type;

    if (probe == NULL || !probe->resource_owner)
        return TURBOWASM_TYPE_MISMATCH;
    if (out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    ++probe->lift_calls;
    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_COMPONENT_TYPE_U32;
    out->as.u32 = handle;
    return TURBOWASM_OK;
}

static turbowasm_status probe_resource_drop(
    void *context,
    uint64_t resource_identity,
    uint32_t handle) {
    import_probe *probe = (import_probe *)context;

    if (probe == NULL || !probe->resource_owner)
        return TURBOWASM_TYPE_MISMATCH;

    ++probe->drop_calls;
    probe->drop_identity = resource_identity;
    probe->drop_handle = handle;
    return TURBOWASM_OK;
}

/*
 * import wasi:clocks/monotonic-clock@0.2.8.now -> canon lower -> Core import
 * -> canon lift -> exported "run".
 */
static const uint8_t monotonic_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,

    0x01,0x23,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x05,0x01,0x60,0x00,0x01,0x7e,
      0x02,0x09,0x01,
        0x01,'p',
        0x03,'n','o','w',
        0x00,0x00,
      0x07,0x07,0x01,
        0x03,'r','u','n',
        0x00,0x00,

    0x07,0x14,0x02,
      0x42,0x02,
        0x01,0x40,0x00,0x00,0x77,
        0x04,0x00,0x03,'n','o','w',0x01,0x00,
      0x40,0x00,0x00,0x77,

    0x0a,0x26,0x01,
      0x00,0x21,
      'w','a','s','i',':','c','l','o','c','k','s','/',
      'm','o','n','o','t','o','n','i','c','-',
      'c','l','o','c','k','@','0','.','2','.','8',
      0x05,0x00,

    0x06,0x08,0x01,
      0x01,0x00,0x00,0x03,'n','o','w',

    0x08,0x05,0x01,
      0x01,0x00,0x00,0x00,

    0x02,0x10,0x02,
      0x01,0x01,
        0x03,'n','o','w',0x00,0x00,
      0x00,0x00,0x01,
        0x01,'p',0x12,0x00,

    0x06,0x09,0x01,
      0x00,0x00,0x01,0x01,0x03,'r','u','n',

    0x08,0x06,0x01,
      0x00,0x00,0x01,0x00,0x01,

    0x0b,0x09,0x01,
      0x00,0x03,'r','u','n',
      0x01,0x01,0x00
};

static turbowasm_component_exec_imports make_imports(
    import_probe *probe) {
    turbowasm_component_exec_imports imports = {0};

    imports.context = probe;
    imports.can_bind = probe_can_bind;
    imports.invoke = probe_invoke;
    imports.resource_lower = probe_resource_lower;
    imports.resource_lift = probe_resource_lift;
    imports.resource_drop = probe_resource_drop;
    return imports;
}

static void test_disjoint_sets_route_one_import(void) {
    static const char actual[] =
        "wasi:clocks/monotonic-clock@0.2.8";
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_component_exec_imports sets[2];
    turbowasm_component_value result = {0};
    turbowasm_component_value resource_value = {0};
    turbowasm_component_type_ref resource_type = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    import_probe other = {
        "wasi:test/other@0.2.8",
        UINT64_C(11),
        0u,
        false
    };
    import_probe clock = {
        actual,
        UINT64_C(424242),
        0u,
        true
    };
    uint32_t handle = 0u;

    sets[0] = make_imports(&other);
    sets[1] = make_imports(&clock);

    assert(turbowasm_component_binary_load(
               &binary,
               monotonic_component,
               sizeof(monotonic_component)) == TURBOWASM_OK);

    assert(turbowasm_component_exec_init_with_import_sets(
               &exec,
               &binary,
               sets,
               2u) == TURBOWASM_OK);
    assert(exec.import_set_count == 2u);
    assert(exec.import_sets != sets);

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"run",
               3u,
               NULL,
               0u,
               &result,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U64);
    assert(result.as.u64 == UINT64_C(424242));
    assert(other.invoke_calls == 0u);
    assert(clock.invoke_calls == 1u);
    turbowasm_component_value_destroy(&result);

    assert(exec.imports.resource_lower != NULL);
    assert(exec.imports.resource_lift != NULL);
    assert(exec.imports.resource_drop != NULL);

    resource_value.kind = TURBOWASM_COMPONENT_TYPE_U32;
    resource_value.as.u32 = 9u;
    assert(exec.imports.resource_lower(
               exec.imports.context,
               &binary.type_graph,
               resource_type,
               &resource_value,
               &handle) == TURBOWASM_OK);
    assert(handle == 77u);
    assert(other.lower_calls == 0u);
    assert(clock.lower_calls == 1u);

    memset(&resource_value, 0, sizeof(resource_value));
    assert(exec.imports.resource_lift(
               exec.imports.context,
               &binary.type_graph,
               resource_type,
               88u,
               &resource_value) == TURBOWASM_OK);
    assert(resource_value.kind == TURBOWASM_COMPONENT_TYPE_U32);
    assert(resource_value.as.u32 == 88u);
    assert(other.lift_calls == 0u);
    assert(clock.lift_calls == 1u);

    assert(exec.imports.resource_drop(
               exec.imports.context,
               UINT64_C(0x1234),
               99u) == TURBOWASM_OK);
    assert(other.drop_calls == 0u);
    assert(clock.drop_calls == 1u);
    assert(clock.drop_identity == UINT64_C(0x1234));
    assert(clock.drop_handle == 99u);

    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&binary);
}

static void test_duplicate_claim_fails_closed(void) {
    static const char actual[] =
        "wasi:clocks/monotonic-clock@0.2.8";
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_component_exec_imports sets[2];
    import_probe first = {
        actual,
        UINT64_C(1),
        0u,
        false
    };
    import_probe second = {
        actual,
        UINT64_C(2),
        0u,
        false
    };

    sets[0] = make_imports(&first);
    sets[1] = make_imports(&second);

    assert(turbowasm_component_binary_load(
               &binary,
               monotonic_component,
               sizeof(monotonic_component)) == TURBOWASM_OK);

    assert(turbowasm_component_exec_init_with_import_sets(
               &exec,
               &binary,
               sets,
               2u) == TURBOWASM_LINK_ERROR);
    assert(exec.binary == NULL);
    assert(exec.import_sets == NULL);
    assert(exec.import_set_count == 0u);
    assert(first.invoke_calls == 0u);
    assert(second.invoke_calls == 0u);

    turbowasm_component_binary_destroy(&binary);
}

static void test_single_import_wrapper_still_works(void) {
    static const char actual[] =
        "wasi:clocks/monotonic-clock@0.2.8";
    turbowasm_component_binary binary = {0};
    turbowasm_component_exec exec = {0};
    turbowasm_component_exec_imports imports;
    turbowasm_component_value result = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    import_probe clock = {
        actual,
        UINT64_C(7),
        0u,
        false
    };

    imports = make_imports(&clock);

    assert(turbowasm_component_binary_load(
               &binary,
               monotonic_component,
               sizeof(monotonic_component)) == TURBOWASM_OK);
    assert(turbowasm_component_exec_init_with_imports(
               &exec,
               &binary,
               &imports) == TURBOWASM_OK);
    assert(exec.import_set_count == 1u);

    assert(turbowasm_component_exec_invoke_export(
               &exec,
               (const uint8_t *)"run",
               3u,
               NULL,
               0u,
               &result,
               &trap) == TURBOWASM_OK);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_U64);
    assert(result.as.u64 == UINT64_C(7));
    assert(clock.invoke_calls == 1u);

    turbowasm_component_value_destroy(&result);
    turbowasm_component_exec_destroy(&exec);
    turbowasm_component_binary_destroy(&binary);
}

int main(void) {
    test_disjoint_sets_route_one_import();
    test_duplicate_claim_fails_closed();
    test_single_import_wrapper_still_works();
    return 0;
}
