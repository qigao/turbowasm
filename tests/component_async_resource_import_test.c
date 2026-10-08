#include "component_exec.h"
#include "runtime_alloc.h"
#include "instance_internal.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include "fixtures/component_async_lower.h"
#include "fixtures/component_async_resource_import.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif

static turbowasm_component_binary binaries[2];
static turbowasm_component_exec execs[3];
static turbowasm_component_task root;
static turbowasm_component_value value;
static turbowasm_runtime_config config;
static turbowasm_runtime_scope scope;
static size_t live, allowance;
static bool other_resource_provider;
static bool failing_resource;
static const turbowasm_component_exec_async_limits limits = {8u, 32u};

static void *allocate(void *context, size_t size) {
    void *p; (void)context;
    if (allowance == 0u) return NULL;
    if (allowance != SIZE_MAX) --allowance;
    p = malloc(size); if (p != NULL) ++live; return p;
}
static void deallocate(void *context, void *p) { (void)context; if (p != NULL) --live; free(p); }
static bool name_is(turbowasm_component_name name, const char *text) {
    return name.size == strlen(text) && memcmp(name.bytes, text, name.size) == 0;
}
static bool can_bind(void *context, turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type) {
    (void)context; (void)function; (void)graph; (void)type; return name_is(instance, "provider");
}
static turbowasm_status target(void *context, turbowasm_component_name instance, turbowasm_component_name function,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    turbowasm_component_exec **provider, uint32_t *adapter) {
    const turbowasm_component_task_binding *binding = NULL;
    const char *override = failing_resource && name_is(function, "resource-result") ? "resource-result-trap" : NULL;
    turbowasm_status status; (void)graph; (void)type;
    if (!name_is(instance, "provider")) return TURBOWASM_TYPE_MISMATCH;
    *provider = context;
    status = turbowasm_component_exec_async_export(*provider,
        override != NULL ? (const uint8_t *)override : function.bytes,
        override != NULL ? (uint32_t)strlen(override) : function.size, &binding);
    if (status == TURBOWASM_OK) *adapter = (uint32_t)(binding - (*provider)->async_functions);
    return status;
}
static turbowasm_status resource_target(void *context, turbowasm_component_name instance,
    turbowasm_component_name resource, turbowasm_component_exec **provider, uint32_t *type) {
    turbowasm_component_exec *exec = other_resource_provider ? &execs[2] : context; uint32_t i;
    if (!name_is(instance, "provider")) return TURBOWASM_TYPE_MISMATCH;
    for (i = 0u; i < exec->binary->export_count; ++i) {
        const turbowasm_component_export *item = &exec->binary->exports[i];
        if (item->kind == TURBOWASM_COMPONENT_EXTERN_TYPE && item->name.size == resource.size &&
            memcmp(item->name.bytes, resource.bytes, resource.size) == 0) {
            *provider = exec; *type = item->item_index; return TURBOWASM_OK;
        }
    }
    return TURBOWASM_TYPE_MISMATCH;
}
static turbowasm_component_exec_imports imports(unsigned provider) {
    turbowasm_component_exec_imports result = {0};
    result.context = &execs[provider]; result.can_bind = can_bind;
    result.async_target = target; result.resource_target = resource_target; return result;
}
static void attach(unsigned index) {
#ifdef TURBOWASM_TEST_MIR
    uint32_t i;
    for (i = 0u; i < execs[index].core_instance_count; ++i) {
        turbowasm_jit_backend backend = {0}; if (execs[index].core_instances[i].impl == NULL) continue;
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(execs[index].core_instances[i].impl, &backend, 1u), TURBOWASM_OK);
    }
#else
    (void)index;
#endif
}
static const turbowasm_component_task_binding *binding(unsigned index, const char *name) {
    const turbowasm_component_task_binding *out = NULL;
    check_equal(turbowasm_component_exec_async_export(&execs[index], (const uint8_t *)name,
        (uint32_t)strlen(name), &out), TURBOWASM_OK); return out;
}
static void compiled(unsigned index, const char *name) {
#ifdef TURBOWASM_TEST_MIR
    const turbowasm_component_task_binding *b = binding(index, name);
    check_equal(((turbowasm_instance_impl *)b->instance->impl)->jit_functions[b->function_index].state, TURBOWASM_JIT_COMPILED);
#else
    (void)index; (void)name;
#endif
}
static void compiled_destructor(void) {
#ifdef TURBOWASM_TEST_MIR
    uint32_t i;
    for (i = 0u; i < execs[0].binary->type_graph.count; ++i) {
        const turbowasm_component_type *type = &execs[0].binary->type_graph.types[i];
        if (type->kind == TURBOWASM_COMPONENT_TYPE_RESOURCE && !type->as.resource.identity_alias && type->as.resource.has_destructor) {
            const turbowasm_component_exec_core_function *function = &execs[0].core_functions[type->as.resource.destructor_index];
            check_equal(((turbowasm_instance_impl *)execs[0].core_instances[function->instance_index].impl)->jit_functions[function->function_index].state,
                TURBOWASM_JIT_COMPILED);
        }
    }
#endif
}
static void create(unsigned index, const char *name) {
    check_equal(turbowasm_component_task_create(&root, &execs[index].task_domain, binding(index, name)), TURBOWASM_OK);
}
static turbowasm_status drive(const turbowasm_execution_options *options) {
    unsigned turns = 0u; turbowasm_status status;
    do {
        unsigned i;
        for (i = 1u; i < 3u; ++i) if (execs[i].initialized) {
            uint32_t pending;
            status = turbowasm_component_exec_async_poll(&execs[i], 8u, options, &pending);
            if (status != TURBOWASM_OK && status != TURBOWASM_YIELDED) return status;
        }
        status = turbowasm_component_task_resume(&root, options);
        check_less(++turns, 10000u);
    } while (status == TURBOWASM_YIELDED);
    return status;
}
static uint32_t destructions(void) {
    turbowasm_component_value count = {0}; const turbowasm_component_task_binding *b = binding(0u, "payload-read");
    check_equal(turbowasm_component_canonical_lift_value(&execs[0].binary->type_graph,
        turbowasm_component_type_ref_inline(TURBOWASM_COMPONENT_TYPE_U32), &b->memory, 400u, &count), TURBOWASM_OK);
    return count.as.u32;
}
static turbowasm_status release_rep(void *context, uint64_t identity, turbowasm_value rep) {
    return turbowasm_component_exec_resource_release(context, identity, rep);
}
static void clear_handles(unsigned index) {
    uint32_t i; turbowasm_component_exec *exec = &execs[index];
    for (i = 0u; i < exec->resource_table.capacity; ++i) {
        uint32_t handle; turbowasm_component_handle_kind kind; void *object;
        if (!turbowasm_component_handle_at(&exec->resource_table, i, &handle, &kind, &object)) continue;
        if (kind == TURBOWASM_COMPONENT_HANDLE_RESOURCE)
            check_equal(turbowasm_component_resource_drop(&exec->resource_table, handle,
                exec->resource_table.entries[i].resource_identity, release_rep, exec), TURBOWASM_OK);
        else if (kind == TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET)
            check_equal(turbowasm_component_task_set_drop(&exec->task_domain, handle), TURBOWASM_OK);
        else check_true(false);
    }
}
static void cleanup(void) {
    int i;
    check_equal(turbowasm_component_task_destroy(&root), TURBOWASM_OK);
    check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
    for (i = 2; i >= 0; --i) if (execs[i].initialized) {
        check_equal(turbowasm_component_exec_async_abort(&execs[i], TURBOWASM_INTERRUPTED), TURBOWASM_OK);
        clear_handles((unsigned)i);
    }
}
static void number(unsigned index, const char *name, uint32_t expected, const turbowasm_execution_options *options) {
    create(index, name); check_equal(drive(options), TURBOWASM_OK);
    check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
    check_equal(value.as.u32, expected); compiled(index, name); cleanup();
}
static void suspend_destructor(void) {
    turbowasm_execution_options options = {0}; unsigned turns = 0u;
    options.has_fuel_limit = true; options.fuel = 4u; create(1u, "roundtrip");
    while (execs[0].task_domain.synchronous_depth == 0u) {
        uint32_t pending; turbowasm_status status;
        status = turbowasm_component_exec_async_poll(&execs[1], 8u, &options, &pending);
        check_true(status == TURBOWASM_OK || status == TURBOWASM_YIELDED);
        check_equal(turbowasm_component_task_resume(&root, &options), TURBOWASM_YIELDED);
        check_less(++turns, 10000u);
    }
    check_not_null(execs[0].task_domain.auxiliary);
}

spec("owned resource imports between async Component instances") {
    before_each() {
        turbowasm_component_exec_imports set;
        live = 0u; allowance = SIZE_MAX; other_resource_provider = false; failing_resource = false;
        turbowasm_runtime_config_init(&config); config.allocator.allocate = allocate; config.allocator.deallocate = deallocate;
        scope = turbowasm_runtime_scope_enter(&config);
        check_equal(turbowasm_component_binary_decode_async_metadata(&binaries[0], component_async_lower_bytes,
            sizeof(component_async_lower_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_binary_decode_async_metadata(&binaries[1], component_async_resource_import_bytes,
            sizeof(component_async_resource_import_bytes), &config), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_init_async(&execs[0], &binaries[0], &limits), TURBOWASM_OK);
        set = imports(0u);
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[1], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        attach(0u); attach(1u);
    }
    after_each() {
        int i; allowance = SIZE_MAX; cleanup();
        for (i = 2; i >= 0; --i) check_equal(turbowasm_component_exec_destroy(&execs[i]), TURBOWASM_OK);
        turbowasm_component_binary_destroy(&binaries[1]); turbowasm_component_binary_destroy(&binaries[0]);
        turbowasm_runtime_scope_leave(scope); check_equal(live, 0u);
    }
    it("round trips owned resources using destination IDs and the defining destructor") {
        uint32_t i; bool checked = false;
        for (i = 0u; i < execs[1].type_view->identity_count; ++i) {
            const turbowasm_component_resource_identity *identity = &execs[1].type_view->identities[i];
            if (identity->provider != NULL) {
                check_not_equal(identity->declaration, identity->provider_declaration); checked = true;
            }
        }
        check_true(checked);
        check_equal(execs[0].async_import_owners, 4u);
        check_equal(turbowasm_component_exec_destroy(&execs[0]), TURBOWASM_TRAPPED);
        number(1u, "roundtrip", 42u, NULL); check_equal(destructions(), 1u);
        compiled(0u, "resource-result"); compiled(0u, "resource-own-child");
        compiled_destructor();
    }
    it("retains both instances while the host owns an imported result") {
        create(1u, "resource-result"); check_equal(drive(NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
        check_equal(turbowasm_component_task_destroy(&root), TURBOWASM_OK);
        check_equal(value.as.resource_rep.as.i32, 42); check_not_null(value.resource_instance_key);
        check_equal(turbowasm_component_exec_destroy(&execs[1]), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_exec_destroy(&execs[0]), TURBOWASM_TRAPPED);
        check_equal(destructions(), 0u); check_equal(turbowasm_component_value_destroy(&value), TURBOWASM_OK);
        check_equal(destructions(), 1u);
    }
    it("transfers resources with strings across memory32 and memory64 under small fuel") {
        turbowasm_execution_options options = {0}; options.has_fuel_limit = true; options.fuel = 4u;
        number(1u, "text", 5u, &options); check_equal(destructions(), 1u); compiled(0u, "resource-text-child");
    }
    it("retains transitive providers and returns resources through an importing instance") {
        turbowasm_component_exec_imports set = imports(1u);
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        attach(2u); check_equal(execs[1].async_import_owners, 4u);
        number(2u, "roundtrip", 42u, NULL); check_equal(destructions(), 1u);
        compiled(1u, "resource-result"); compiled(1u, "resource-own-child");
    }
    it("rejects ambiguous resource resolvers without retaining a provider") {
        turbowasm_component_exec_imports sets[2] = {imports(0u), imports(0u)};
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, sets, 2u), TURBOWASM_TYPE_MISMATCH);
        check_null(execs[2].binary); check_equal(execs[0].async_import_owners, 4u);
    }
    it("rejects a function provider whose resource belongs to another instance of the same binary") {
        turbowasm_component_exec_imports set = imports(0u);
        check_equal(turbowasm_component_exec_destroy(&execs[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_exec_init_async(&execs[2], &binaries[0], &limits), TURBOWASM_OK);
        other_resource_provider = true;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[1], &binaries[1], &limits, &set, 1u), TURBOWASM_TYPE_MISMATCH);
        other_resource_provider = false;
        check_null(execs[1].binary); check_equal(execs[0].async_import_owners, 0u); check_equal(execs[2].async_import_owners, 0u);
    }
    it("destroys the uncommitted result when the destination resource table is full") {
        uint32_t occupied;
        execs[1].resource_table.max_entries = 1u;
        check_equal(turbowasm_component_task_set_new(&execs[1].task_domain, &occupied), TURBOWASM_OK);
        create(1u, "roundtrip"); check_equal(drive(NULL), TURBOWASM_OUT_OF_MEMORY);
        cleanup(); check_equal(destructions(), 1u);
        check_equal(execs[0].async_resource_owners, 0u); check_equal(execs[1].async_resource_owners, 0u);
    }
    it("unwinds a resource reservation suspended in cross-instance result realloc") {
        turbowasm_execution_options options = {0}; unsigned turns = 0u;
        options.has_fuel_limit = true; options.fuel = 4u;
        create(1u, "text");
        while (execs[1].task_domain.auxiliary == NULL) {
            uint32_t pending; turbowasm_status status;
            status = turbowasm_component_task_resume(&root, &options);
            check_equal(status, TURBOWASM_YIELDED);
            status = turbowasm_component_exec_async_poll(&execs[1], 8u, &options, &pending);
            check_true(status == TURBOWASM_OK || status == TURBOWASM_YIELDED);
            check_less(++turns, 10000u);
        }
        check_greater(execs[0].async_resource_owners, 0u);
        cleanup(); check_equal(destructions(), 1u);
        check_null(execs[0].task_domain.auxiliary); check_null(execs[1].task_domain.auxiliary);
    }
    it("shares caller fuel with a foreign resource destructor and resumes without replay") {
        turbowasm_component_task sibling = {0};
        suspend_destructor();
        check_equal(turbowasm_component_task_create(&sibling, &execs[0].task_domain, binding(0u, "resource-result")), TURBOWASM_OK);
        check_equal(turbowasm_component_task_resume(&sibling, NULL), TURBOWASM_YIELDED);
        check_equal(drive(NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_task_destroy(&sibling), TURBOWASM_OK);
        check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
        check_equal(value.as.u32, 42u); cleanup(); check_equal(destructions(), 1u);
        check_equal(execs[0].task_domain.synchronous_depth, 0u); check_null(execs[0].task_domain.auxiliary);
    }
    it("unwinds a foreign destructor and clears its temporary task when the caller is destroyed") {
        suspend_destructor(); cleanup();
        check_true(destructions() <= 1u);
        check_equal(execs[0].task_domain.synchronous_depth, 0u); check_null(execs[0].task_domain.auxiliary);
        check_null(execs[0].task_domain.active);
    }
    it("propagates the foreign destructor trap after consuming the owned handle") {
        turbowasm_component_exec_imports set = imports(0u);
        check_equal(turbowasm_component_exec_destroy(&execs[1]), TURBOWASM_OK); failing_resource = true;
        check_equal(turbowasm_component_exec_init_async_with_import_sets(&execs[1], &binaries[1], &limits, &set, 1u), TURBOWASM_OK);
        failing_resource = false; attach(1u);
        create(1u, "roundtrip"); check_equal(drive(NULL), TURBOWASM_TRAPPED);
        check_equal(root.trap, TURBOWASM_TRAP_UNREACHABLE); cleanup(); check_equal(destructions(), 1u);
        check_equal(execs[0].resource_table.live_count, 0u); check_equal(execs[1].resource_table.live_count, 0u);
    }
    it("rolls back resource and function provider references after every constructor allocation failure") {
        size_t baseline = live, budget; turbowasm_component_exec_imports set = imports(0u);
        for (budget = 0u; budget < 2048u; ++budget) {
            turbowasm_status status; allowance = budget;
            status = turbowasm_component_exec_init_async_with_import_sets(&execs[2], &binaries[1], &limits, &set, 1u);
            allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                check_equal(execs[0].async_import_owners, 8u);
                check_equal(turbowasm_component_exec_destroy(&execs[2]), TURBOWASM_OK);
                check_equal(execs[0].async_import_owners, 4u); check_equal(live, baseline); break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY); check_null(execs[2].binary);
            check_equal(execs[0].async_import_owners, 4u); check_equal(live, baseline);
        }
        check_greater(budget, 0u); check_less(budget, 2048u);
    }
    it("rolls back partial mixed-value transfers and releases each resource exactly once") {
        size_t baseline, budget;
        number(1u, "text", 5u, NULL); baseline = live;
        for (budget = 0u; budget < 1024u; ++budget) {
            turbowasm_status status; uint32_t before = destructions();
            create(1u, "text"); allowance = budget; status = drive(NULL); allowance = SIZE_MAX;
            if (status == TURBOWASM_OK) {
                check_equal(turbowasm_component_task_take_result(&root, &value), TURBOWASM_OK);
                check_equal(value.as.u32, 5u);
            } else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            cleanup(); check_equal(live, baseline); check_equal(execs[0].async_resource_owners, 0u);
            check_equal(execs[1].async_resource_owners, 0u);
            check_true(destructions() - before <= 1u);
            if (status == TURBOWASM_OK) check_equal(destructions() - before, 1u);
            if (status == TURBOWASM_OK) break;
        }
        check_greater(budget, 0u); check_less(budget, 1024u);
    }
}
