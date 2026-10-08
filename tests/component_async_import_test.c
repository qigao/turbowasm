#include "component_exec.h"
#include "component_endpoint_builtin.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_async_provider.h"
#include "fixtures/component_async_import.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif

static turbowasm_component_binary binaries[2];
static turbowasm_component_exec execs[2];
static turbowasm_component_task root, sibling;
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static turbowasm_component_exec_async_limits limits = {8,32};
static turbowasm_component_exec_imports imports;
static size_t live, allowance;
static bool wrong_target;
static turbowasm_component_type_graph admission_graph;
static turbowasm_component_task_binding *overridden_binding, saved_binding;

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0u) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live; return p;
}
static void deallocate(void *context, void *p) { (void)context; if (p != NULL) --live; free(p); }
static bool can_bind(void *context, turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type) {
    (void)context; (void)function; (void)graph; (void)type;
    return instance.size == 8u && memcmp(instance.bytes, "provider", 8u) == 0;
}
static turbowasm_status target(void *context, turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    turbowasm_component_exec **provider, uint32_t *adapter) {
    const turbowasm_component_task_binding *binding = NULL;
    turbowasm_status status;
    bool mismatch = wrong_target && function.size == 5u && memcmp(function.bytes, "mixed", 5u) == 0;
    (void)instance; (void)graph; (void)type;
    *provider = context;
    status = turbowasm_component_exec_async_export(*provider,
        mismatch ? (const uint8_t *)"text" : function.bytes, mismatch ? 4u : function.size, &binding);
    if (status == TURBOWASM_OK) *adapter = (uint32_t)(binding - (*provider)->async_functions);
    return status;
}
static const turbowasm_component_task_binding *binding(unsigned exec, const char *name) {
    const turbowasm_component_task_binding *out = NULL;
    check_equal(turbowasm_component_exec_async_export(&execs[exec], (const uint8_t *)name, (uint32_t)strlen(name), &out), TURBOWASM_OK);
    return out;
}
static void attach(unsigned index) {
#ifdef TURBOWASM_TEST_MIR
    uint32_t i;
    for (i = 0; i < execs[index].core_instance_count; ++i) {
        turbowasm_jit_backend backend = {0};
        if (execs[index].core_instances[i].impl == NULL) continue;
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(execs[index].core_instances[i].impl, &backend, 1), TURBOWASM_OK);
    }
#else
    (void)index;
#endif
}
static void compiled_provider(const char *name) {
#ifdef TURBOWASM_TEST_MIR
    const turbowasm_component_task_binding *target = binding(0, name);
    check_equal(((turbowasm_instance_impl *)target->instance->impl)->jit_functions[target->function_index].state,
        TURBOWASM_JIT_COMPILED);
    if (target->callback_instance != NULL)
        check_equal(((turbowasm_instance_impl *)target->callback_instance->impl)->jit_functions[target->callback_index].state,
            TURBOWASM_JIT_COMPILED);
#else
    (void)name;
#endif
}
static void create(const char *name) {
    check_equal(turbowasm_component_task_create(&root, &execs[1].task_domain, binding(1, name)), TURBOWASM_OK);
}
static void result(uint32_t expected) {
    turbowasm_component_value value = {0};
    check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
    check_equal(value.kind, TURBOWASM_COMPONENT_TYPE_U32); check_equal(value.as.u32, expected);
    check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)root.binding.instance->impl)->jit_functions[root.binding.function_index].state,
        TURBOWASM_JIT_COMPILED);
#endif
}
static void finish(uint32_t expected, const turbowasm_execution_options *options) {
    unsigned turns = 0; uint32_t pending;
    turbowasm_status status;
    do {
        status = turbowasm_component_exec_async_poll(&execs[1], 8, options, &pending);
        check_true(status == TURBOWASM_OK || status == TURBOWASM_YIELDED);
        status = turbowasm_component_task_resume(&root, options);
        check_true(status == TURBOWASM_OK || status == TURBOWASM_YIELDED);
        check_less(++turns, 10000u);
    } while (status == TURBOWASM_YIELDED);
    result(expected);
    check_equal(turbowasm_component_task_destroy(&root), TURBOWASM_OK);
    do {
        status = turbowasm_component_exec_async_poll(&execs[1], 8, options, &pending);
        check_true(status == TURBOWASM_OK || status == TURBOWASM_YIELDED); check_less(++turns, 10000u);
    } while (pending != 0);
    check_equal(execs[0].task_domain.count, 0u); check_equal(execs[1].task_domain.count, 0u);
}
static void clear_handles(unsigned index) {
    uint32_t i;
    for (i = 0; i < execs[index].resource_table.capacity; ++i) {
        uint32_t handle; turbowasm_component_handle_kind kind; void *object;
        if (!turbowasm_component_handle_at(&execs[index].resource_table, i, &handle, &kind, &object)) continue;
        if (kind == TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET)
            check_equal(turbowasm_component_task_set_drop(&execs[index].task_domain, handle), TURBOWASM_OK);
        else {
            turbowasm_component_endpoint *end = turbowasm_component_endpoint_get(&execs[index].resource_table, handle, kind);
            check_not_null(end); check_equal(turbowasm_component_endpoint_close(end), TURBOWASM_OK);
        }
    }
    turbowasm_component_endpoint_domain_collect(&execs[index].task_domain);
}
static void abort_calls(void) {
    check_equal(turbowasm_component_task_destroy(&sibling), TURBOWASM_OK);
    check_equal(turbowasm_component_task_destroy(&root), TURBOWASM_OK);
    check_equal(turbowasm_component_exec_async_abort(&execs[1], TURBOWASM_INTERRUPTED), TURBOWASM_OK);
    clear_handles(1); clear_handles(0);
    /* Provider handle closure can release a pair allocated by the consumer. */
    turbowasm_component_endpoint_domain_collect(&execs[1].task_domain);
}
static void suspend_result(const char *name) {
    turbowasm_execution_options options = {0}; uint32_t pending; unsigned turns = 0;
    options.has_fuel_limit = true; options.fuel = 4;
    create(name);
    check_equal(turbowasm_component_task_resume(&root, &options), TURBOWASM_YIELDED);
    while (execs[1].task_domain.auxiliary == NULL) {
        turbowasm_status status = turbowasm_component_exec_async_poll(&execs[1], 1, &options, &pending);
        check_true(status == TURBOWASM_OK || status == TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_resume(&root, &options), TURBOWASM_YIELDED);
        check_less(++turns, 1000u);
    }
    check_true(execs[0].task_domain.auxiliary == execs[1].task_domain.auxiliary);
    check_false(execs[1].may_leave); check_equal(execs[1].async_call_count, 1u);
}
static bool interrupt(void *context) { ++*(unsigned *)context; return true; }

spec("retained async instance imports") {
    before_each() {
        live = 0; allowance = SIZE_MAX; wrong_target = false;
        turbowasm_runtime_config_init(&config); config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
        check_equal(turbowasm_component_binary_decode_async_metadata(&binaries[0], component_async_provider_bytes,
            sizeof(component_async_provider_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_binary_decode_async_metadata(&binaries[1], component_async_import_bytes,
            sizeof(component_async_import_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_init_async(&execs[0], &binaries[0], &limits), TURBOWASM_OK); attach(0);
        memset(&imports, 0, sizeof(imports)); imports.context = &execs[0]; imports.can_bind = can_bind; imports.async_target = target;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[1], &binaries[1], &limits, &imports, 1), TURBOWASM_OK);
        attach(1); check_equal(execs[0].async_import_owners, 6u);
    }
    after_each() {
        if (overridden_binding != NULL) { *overridden_binding = saved_binding; overridden_binding = NULL; }
        turbowasm_component_type_graph_destroy(&admission_graph);
        allowance = SIZE_MAX; abort_calls();
        check_equal(turbowasm_component_exec_destroy(&execs[1]), TURBOWASM_OK);
        check_equal(execs[0].async_import_owners, 0u);
        check_equal(turbowasm_component_exec_destroy(&execs[0]), TURBOWASM_OK);
        turbowasm_component_binary_destroy(&binaries[1]); turbowasm_component_binary_destroy(&binaries[0]);
        turbowasm_runtime_scope_leave(scope); check_equal(live, 0u);
    }
    it("retains the provider binding and completes an eager scalar across instances") {
        check_equal(turbowasm_component_exec_destroy(&execs[0]), TURBOWASM_TRAPPED);
        create("scalar"); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_OK);
        finish(41, NULL); check_equal(execs[1].async_call_count, 0u);
        compiled_provider("scalar");
    }
    it("drives deferred provider callbacks on the consumer's bounded progress list") {
        create("slow"); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_YIELDED);
        check_equal(execs[0].task_domain.count, 1u); check_equal(execs[0].async_call_count, 0u);
        check_equal(turbowasm_component_exec_async_abort(&execs[0], TURBOWASM_INTERRUPTED), TURBOWASM_TRAPPED);
        finish(44, NULL);
        compiled_provider("slow");
    }
    it("retains the provider task after early return until its callback exits") {
        create("early"); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_OK);
        check_equal(execs[0].task_domain.count, 1u); finish(43, NULL);
    }
    it("converts strings through memory64 and back into memory32 with isolated contexts") {
        create("text"); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_OK); finish(5, NULL);
        compiled_provider("text");
    }
    it("moves nested endpoints through both canonical tables and closes the returned pair") {
        create("mixed"); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_OK); finish(5, NULL);
        check_equal(execs[0].resource_table.live_count, 0u); check_equal(execs[1].resource_table.live_count, 0u);
        compiled_provider("mixed");
    }
    it("preserves both domains through fuel suspension in consumer result realloc") {
        suspend_result("text");
        check_equal(turbowasm_component_task_create(&sibling, &execs[1].task_domain, binding(1, "scalar")), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&sibling, NULL), TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_destroy(&sibling), TURBOWASM_OK);
        finish(5, NULL); check_true(execs[1].may_leave);
        check_null(execs[0].task_domain.auxiliary); check_null(execs[1].task_domain.auxiliary);
    }
    it("unwinds cross-instance result realloc before releasing endpoint reservations") {
        suspend_result("mixed"); abort_calls();
        check_true(execs[1].may_leave); check_null(execs[0].task_domain.auxiliary); check_null(execs[1].task_domain.auxiliary);
        check_equal(execs[0].resource_table.live_count, 0u); check_equal(execs[1].resource_table.live_count, 0u);
    }
    it("yields consumer realloc on the provider task's interrupt policy and resumes without replay") {
        turbowasm_execution_options options = {0}; unsigned checks = 0u; uint32_t pending;
        suspend_result("mixed"); options.should_interrupt = interrupt; options.interrupt_context = &checks;
        check_equal(turbowasm_component_exec_async_poll(&execs[1], 1u, &options, &pending), TURBOWASM_YIELDED);
        check_greater(checks, 0u); check_false(execs[1].may_leave);
        check_not_null(execs[0].task_domain.auxiliary);
        check_true(execs[0].task_domain.auxiliary == execs[1].task_domain.auxiliary);
        check_equal(turbowasm_execution_yield_reason_get(&execs[0].task_domain.auxiliary->core), TURBOWASM_YIELD_INTERRUPTION);
        finish(5, NULL); check_true(execs[1].may_leave);
        check_null(execs[0].task_domain.auxiliary); check_null(execs[1].task_domain.auxiliary);
        check_equal(execs[0].resource_table.live_count, 0u); check_equal(execs[1].resource_table.live_count, 0u);
    }
    it("acknowledges cancellation through the provider's callback") {
        create("cancel"); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_OK); finish(49, NULL);
    }
    it("propagates provider traps and reclaims unpublished foreign tasks on abort") {
        create("trap"); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_TRAPPED);
        abort_calls(); check_equal(execs[0].task_domain.count, 0u); check_equal(execs[1].async_call_count, 0u);
    }
    it("charges provider tasks separately from the consumer call quota") {
        execs[0].task_domain.limit = 1;
        check_equal(turbowasm_component_task_create(&sibling, &execs[0].task_domain, binding(0, "slow")), TURBOWASM_OK);
        create("scalar"); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_OUT_OF_MEMORY);
        check_equal(execs[1].async_call_count, 0u); check_equal(execs[0].task_domain.count, 1u);
    }
    it("allows normal return to race cancellation under small fuel budgets") {
        turbowasm_execution_options options = {0}; options.has_fuel_limit = true; options.fuel = 4;
        create("cancel"); check_equal(turbowasm_component_task_resume(&root, &options), TURBOWASM_YIELDED);
        finish(49, &options);
    }
    it("rolls back allocation failures after foreign task and endpoint admission") {
        size_t baseline, budget; bool succeeded = false;
        create("mixed"); check_equal(turbowasm_component_task_resume(&root, NULL), TURBOWASM_OK); finish(5, NULL);
        clear_handles(1); clear_handles(0); baseline = live;
        for (budget = 0; budget < 300; ++budget) {
            turbowasm_status status;
            create("mixed"); allowance = budget;
            status = turbowasm_component_task_resume(&root, NULL); allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) { result(5); succeeded = true; }
            else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            abort_calls(); check_equal(live, baseline);
            check_equal(execs[0].async_import_owners, 6u); check_equal(execs[0].task_domain.count, 0u);
            if (succeeded) break;
        }
        check_true(succeeded); check_greater(budget, 0u);
    }
    it("rejects ambiguous and incompatible targets without leaking provider references") {
        turbowasm_component_exec rejected = {0}; turbowasm_component_exec_imports duplicate[2] = {imports, imports};
        size_t baseline = live;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&rejected, &binaries[1], &limits, duplicate, 2), TURBOWASM_LINK_ERROR);
        check_null(rejected.binary); check_equal(execs[0].async_import_owners, 6u); check_equal(live, baseline);
        wrong_target = true;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&rejected, &binaries[1], &limits, &imports, 1), TURBOWASM_TYPE_MISMATCH);
        check_null(rejected.binary); check_equal(execs[0].async_import_owners, 6u); check_equal(live, baseline);
    }
    it("rejects nominal resource imports even when hidden in an endpoint with matching numeric type IDs") {
        turbowasm_component_type_ref stream = turbowasm_component_type_ref_indexed(2u);
        turbowasm_component_exec_canon_lower_context lower = {0};
        check_true(turbowasm_component_type_graph_allocate(&admission_graph, 4u));
        check_true(turbowasm_component_type_graph_define_resource(&admission_graph, 0u, 1u));
        check_true(turbowasm_component_type_graph_define_handle(&admission_graph, 1u, TURBOWASM_COMPONENT_TYPE_OWN, 0u));
        check_true(turbowasm_component_type_graph_define_async_value(&admission_graph, 2u,
            TURBOWASM_COMPONENT_TYPE_STREAM, true, turbowasm_component_type_ref_indexed(1u)));
        check_true(turbowasm_component_type_graph_define_function(&admission_graph, 3u, &stream, 1u, true, stream));
        admission_graph.types[3].as.function.is_async = true;
        overridden_binding = &execs[0].async_functions[0]; saved_binding = *overridden_binding;
        overridden_binding->graph = &admission_graph; overridden_binding->function_type = 3u;
        lower.exec = &execs[1]; lower.graph = &admission_graph; lower.function_type = 3u; lower.is_async = true;
        check_equal(turbowasm_component_exec_async_bind(&lower, &execs[0], 0u), TURBOWASM_UNSUPPORTED);
        check_null(lower.async_provider); check_equal(execs[0].async_import_owners, 6u);
    }
    it("releases partially bound providers on every instantiation allocation failure") {
        size_t budget, baseline = live; bool succeeded = false;
        for (budget = 0; budget < 1000; ++budget) {
            turbowasm_component_exec candidate = {0}; turbowasm_status status;
            allowance = budget;
            status = turbowasm_component_exec_init_async_with_import_sets(&candidate, &binaries[1], &limits, &imports, 1);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                check_equal(execs[0].async_import_owners, 12u);
                check_equal(turbowasm_component_exec_destroy(&candidate), TURBOWASM_OK); succeeded = true;
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_null(candidate.binary); check_equal(execs[0].async_import_owners, 6u); check_equal(live, baseline);
            if (succeeded) break;
        }
        check_true(succeeded); check_greater(budget, 0u);
    }
}
