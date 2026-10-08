#include "component_exec.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_async_lower.h"

static turbowasm_component_binary source;
static turbowasm_component_type_view *views[2];
static turbowasm_component_exec instances[2];
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t live, allowance;

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0u) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live; return p;
}
static void deallocate(void *context, void *p) {
    (void)context; if (p != NULL) --live; free(p);
}
static turbowasm_component_type_ref indexed(uint32_t i) {
    return turbowasm_component_type_ref_indexed(i);
}
static void nested_metadata(void) {
    turbowasm_component_instance_type *nested = turbowasm_rt_calloc(1u, sizeof(*nested));
    turbowasm_component_type_graph *graph = &source.type_graph;
    turbowasm_component_record_field field = {(const uint8_t *)"value", 5u, indexed(1u)};
    check_not_null(nested);
    check_true(turbowasm_component_type_graph_allocate(&nested->type_graph, 2u));
    check_true(turbowasm_component_type_graph_define_resource(&nested->type_graph, 0u, 41u));
    check_true(turbowasm_component_type_graph_define_handle(&nested->type_graph, 1u, TURBOWASM_COMPONENT_TYPE_OWN, 0u));
    nested->exports = turbowasm_rt_calloc(1u, sizeof(*nested->exports));
    check_not_null(nested->exports); nested->export_count = 1u;
    nested->exports[0].name = (const uint8_t *)"resource"; nested->exports[0].name_size = 8u;
    nested->exports[0].kind = TURBOWASM_COMPONENT_INSTANCE_EXPORT_TYPE;
    check_true(turbowasm_component_type_graph_allocate(graph, 7u));
    check_true(turbowasm_component_type_graph_define_resource_alias(graph, 0u, 41u, 0x7f, false, 0u));
    check_true(turbowasm_component_type_graph_define_handle(graph, 1u, TURBOWASM_COMPONENT_TYPE_OWN, 0u));
    check_true(turbowasm_component_type_graph_define_instance(graph, 2u, nested));
    check_true(turbowasm_component_type_graph_define_record(graph, 3u, &field, 1u));
    check_true(turbowasm_component_type_graph_define_resource(graph, 4u, 42u));
    check_true(turbowasm_component_type_graph_define_handle(graph, 5u, TURBOWASM_COMPONENT_TYPE_OWN, 4u));
    check_true(turbowasm_component_type_graph_define_async_value(graph, 6u, TURBOWASM_COMPONENT_TYPE_STREAM, true, indexed(1u)));
    check_true(turbowasm_component_type_graph_validate(graph));
}

spec("generative Component instance resource types") {
    before_each() {
        live = 0u; allowance = SIZE_MAX;
        turbowasm_runtime_config_init(&config);
        config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config); source.config = config;
    }
    after_each() {
        uint32_t i; allowance = SIZE_MAX;
        for (i = 0u; i < 2u; ++i) {
            check_equal(turbowasm_component_exec_destroy(&instances[i]), TURBOWASM_OK);
            turbowasm_component_type_view_destroy(views[i]); views[i] = NULL;
        }
        turbowasm_component_binary_destroy(&source);
        turbowasm_runtime_scope_leave(scope); check_equal(live, 0u);
    }
    it("shares alias identities within an instance and separates independent instantiations") {
        const turbowasm_component_type_graph *a, *b, *nested;
        nested_metadata();
        check_equal(turbowasm_component_type_view_create(&source, &views[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_type_view_create(&source, &views[1]), TURBOWASM_OK);
        a = &views[0]->binary.type_graph; b = &views[1]->binary.type_graph;
        nested = &a->types[2].as.instance->type_graph;
        check_equal(a->types[0].as.resource.identity, 41u);
        check_not_null(a->types[0].as.resource.instance_key);
        check_true(turbowasm_component_value_type_equal(a, indexed(1u), nested, indexed(1u)));
        check_false(turbowasm_component_value_type_equal(a, indexed(1u), a, indexed(5u)));
        check_false(turbowasm_component_value_type_equal(a, indexed(3u), b, indexed(3u)));
        check_false(turbowasm_component_value_type_equal(a, indexed(6u), b, indexed(6u)));
        check_false(turbowasm_component_value_type_equal(a, indexed(1u), &source.type_graph, indexed(1u)));
        check_false(turbowasm_component_value_type_equal(&source.type_graph, indexed(1u), a, indexed(1u)));
        check_null(source.type_graph.types[0].as.resource.instance_key);
        check_null(source.type_graph.types[2].as.instance->type_graph.types[0].as.resource.instance_key);
        check_not_equal(a->types[2].as.instance, source.type_graph.types[2].as.instance);
        check_equal(a->types[2].as.instance->exports, source.type_graph.types[2].as.instance->exports);
        check_equal(a->types[3].as.record.fields, source.type_graph.types[3].as.record.fields);
        turbowasm_component_type_view_destroy(views[0]); views[0] = NULL;
        check_true(turbowasm_component_type_graph_validate(&source.type_graph));
        check_true(turbowasm_component_type_graph_validate(b));
    }
    it("rolls back every allocation failure without releasing borrowed source metadata") {
        size_t budget, baseline;
        nested_metadata(); baseline = live;
        for (budget = 0u; budget < 32u; ++budget) {
            turbowasm_status status;
            allowance = budget; status = turbowasm_component_type_view_create(&source, &views[0]);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                turbowasm_component_type_view_destroy(views[0]); views[0] = NULL;
                check_equal(live, baseline); break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_null(views[0]);
            check_equal(live, baseline);
            check_true(turbowasm_component_type_graph_validate(&source.type_graph));
            check_null(source.type_graph.types[0].as.resource.instance_key);
        }
        check_greater(budget, 0u); check_less(budget, 32u);
    }
    it("keeps resource-free metadata borrowed without allocating") {
        size_t baseline;
        check_true(turbowasm_component_type_graph_allocate(&source.type_graph, 1u));
        check_true(turbowasm_component_type_graph_define_string(&source.type_graph, 0u));
        baseline = live; allowance = 0u;
        check_equal(turbowasm_component_type_view_create(&source, &views[0]), TURBOWASM_OK);
        check_null(views[0]); check_equal(live, baseline);
    }
    it("releases instance views on failures throughout Core instantiation") {
        turbowasm_component_exec_async_limits limits = {5u, 16u};
        size_t budget, baseline;
        check_equal(turbowasm_component_binary_decode_async_metadata(&source, component_async_lower_bytes,
            sizeof(component_async_lower_bytes), &config), TURBOWASM_OK);
        baseline = live;
        for (budget = 0u; budget < 4096u; ++budget) {
            turbowasm_status status;
            allowance = budget;
            status = turbowasm_component_exec_init_async(&instances[0], &source, &limits);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                check_not_null(instances[0].type_view);
                check_equal(turbowasm_component_exec_destroy(&instances[0]), TURBOWASM_OK);
                check_equal(live, baseline); break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_null(instances[0].binary); check_null(instances[0].type_view);
            check_equal(live, baseline);
        }
        check_greater(budget, 0u); check_less(budget, 4096u);
        check_true(turbowasm_component_type_graph_validate(&source.type_graph));
    }
    it("rejects resource endpoint transfer between two instances of the same binary") {
        turbowasm_component_exec_async_limits limits = {5u, 16u};
        turbowasm_component_endpoint ends[2] = {0};
        turbowasm_component_endpoint_codec codec = {0};
        turbowasm_component_value value = {0};
        uint32_t i, type = UINT32_MAX, handle = UINT32_MAX;
        const turbowasm_component_type_graph *a, *b;
        check_equal(turbowasm_component_binary_decode_async_metadata(&source, component_async_lower_bytes,
            sizeof(component_async_lower_bytes), &config), TURBOWASM_OK);
        for (i = 0u; i < 2u; ++i)
            check_equal(turbowasm_component_exec_init_async(&instances[i], &source, &limits), TURBOWASM_OK);
        a = &instances[0].binary->type_graph; b = &instances[1].binary->type_graph;
        for (i = 0u; i < a->count; ++i) {
            uint32_t features;
            if (a->types[i].kind == TURBOWASM_COMPONENT_TYPE_STREAM && a->types[i].as.async_value.has_payload &&
                turbowasm_component_transfer_type_features(a, a->types[i].as.async_value.payload, &features) &&
                features == TURBOWASM_COMPONENT_VALUE_RESOURCES) { type = i; break; }
        }
        check_not_equal(type, UINT32_MAX);
        check_equal(turbowasm_component_endpoint_pair_open(a, type, NULL, NULL, &ends[0], &ends[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_into_value(&ends[0], &value), TURBOWASM_OK);
        codec.table = &instances[1].resource_table;
        check_equal(turbowasm_component_endpoint_codec_lower(&codec, b, indexed(type), &value, &handle), TURBOWASM_TYPE_MISMATCH);
        check_equal(handle, UINT32_MAX); check_null(codec.lower_head);
        codec.table = &instances[0].resource_table;
        check_equal(turbowasm_component_endpoint_codec_lower(&codec, a, indexed(type), &value, &handle), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_codec_rollback(&codec), TURBOWASM_OK);
        check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
        check_equal(turbowasm_component_endpoint_close(&ends[1]), TURBOWASM_OK);
        for (i = 0u; i < source.type_graph.count; ++i)
            if (source.type_graph.types[i].kind == TURBOWASM_COMPONENT_TYPE_RESOURCE)
                check_null(source.type_graph.types[i].as.resource.instance_key);
    }
}
