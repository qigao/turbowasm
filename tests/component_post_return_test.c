#include <turbowasm/component.h>
#include "component_exec.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include "fixtures/component_post_return.h"
#include "fixtures/component_post_return_leave.h"
#include "fixtures/component_post_return_resources.h"
#include "fixtures/component_post_return_memory64.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_RESUMES = 512, TEXT_ADDRESS = 32, TEXT_SIZE = 5 };
/* (type (func)), import f, then canonical definitions. */
#define OPTIONS_PREFIX \
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00, \
    0x07,0x05,0x01,0x40,0x00,0x01,0x00, \
    0x0a,0x06,0x01,0x00,0x01,0x66,0x01,0x00
static turbowasm_component_binary binary;
static turbowasm_component_exec exec;
static turbowasm_component_exec_call call;
static turbowasm_component component;
static turbowasm_component_instance instance;
static turbowasm_component_call public_call;
static turbowasm_component_value result;
static turbowasm_component_host_value public_result;
static struct { size_t live, attempts, fail_at; } allocations;
static unsigned notifications;

static void *allocate(void *context, size_t size) {
    void *p;
    (void)context;
    if (++allocations.attempts == allocations.fail_at)
        return NULL;
    p = malloc(size);
    if (p != NULL) ++allocations.live;
    return p;
}
static void deallocate(void *context, void *p) {
    (void)context;
    if (p != NULL) { check_true(allocations.live != 0u); --allocations.live; }
    free(p);
}
static turbowasm_name name(const char *s) {
    return (turbowasm_name){(const uint8_t *)s, (uint32_t)strlen(s)};
}
static void setup_public(const uint8_t *bytes, size_t size) {
    turbowasm_runtime_config config;
    turbowasm_runtime_config_init(&config);
    config.allocator.allocate = allocate;
    config.allocator.deallocate = deallocate;
    check_equal(turbowasm_component_load_borrowed_with_config(
        &component, bytes, size, &config), TURBOWASM_OK);
    check_equal(turbowasm_component_instance_create(&instance, &component), TURBOWASM_OK);
}
static uint32_t drops(void) {
    turbowasm_component_host_value out = {0}; size_t count; turbowasm_trap trap;
    check_equal(turbowasm_component_instance_invoke(&instance, name("drops"),
        NULL, 0u, &out, 1u, &count, &trap), TURBOWASM_OK);
    check_equal(count, 1u); return out.as.u32;
}
static void setup_fixture(const uint8_t *bytes, size_t size) {
    turbowasm_runtime_config config;
    turbowasm_runtime_config_init(&config);
    config.allocator.allocate = allocate;
    config.allocator.deallocate = deallocate;
    check_equal(turbowasm_component_binary_load_with_config(&binary,
        bytes, size, &config), TURBOWASM_OK);
    check_equal(turbowasm_component_exec_init(&exec, &binary), TURBOWASM_OK);
}
static void setup(void) {
    setup_fixture(component_post_return_bytes, sizeof(component_post_return_bytes));
}
static void attach_core_backends(void) {
#ifdef TURBOWASM_TEST_MIR
    uint32_t i;
    for (i = 0u; i < exec.core_instance_count; ++i) {
        turbowasm_jit_backend backend = {0};
        if (exec.core_instances[i].impl == NULL) continue;
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(exec.core_instances[i].impl,
            &backend, 1u), TURBOWASM_OK);
    }
#endif
}
static turbowasm_status invoke(const char *export_name, turbowasm_component_value *out) {
    turbowasm_trap trap;
    turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&binary.config);
    turbowasm_status status = turbowasm_component_exec_invoke_export(&exec,
        (const uint8_t *)export_name, (uint32_t)strlen(export_name), NULL, 0u, out, &trap);
    turbowasm_runtime_scope_leave(scope);
    return status;
}
static uint32_t number(const char *export_name) {
    turbowasm_component_value out = {0};
    check_equal(invoke(export_name, &out), TURBOWASM_OK);
    check_equal(out.kind, TURBOWASM_COMPONENT_TYPE_U32);
    return out.as.u32;
}
static void create_call(const char *export_name) {
    check_equal(turbowasm_component_exec_call_create(&call, &exec,
        (const uint8_t *)export_name, (uint32_t)strlen(export_name), NULL, 0u), TURBOWASM_OK);
}
static turbowasm_status resume_one(void) {
    turbowasm_execution_options options = {0};
    options.has_fuel_limit = true;
    options.fuel = 1u;
    return turbowasm_component_exec_call_resume(&call, &options);
}
static bool can_bind(void *context, turbowasm_component_name instance_name,
    turbowasm_component_name function_name, const turbowasm_component_type_graph *graph,
    turbowasm_component_type_id type) {
    (void)context; (void)instance_name; (void)function_name; (void)graph; (void)type;
    return true;
}
static turbowasm_status notify_host(void *context, turbowasm_host_call *host,
    turbowasm_component_name instance_name, turbowasm_component_name function_name,
    const turbowasm_component_type_graph *graph, turbowasm_component_type_id type,
    const turbowasm_component_value *args, size_t count,
    turbowasm_component_value *out, turbowasm_trap *trap) {
    (void)context; (void)host; (void)instance_name; (void)function_name;
    (void)graph; (void)type; (void)args; (void)out;
    check_equal(count, 0u); ++notifications; *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

spec("canonical post-return") {
    before_each() { memset(&allocations, 0, sizeof(allocations)); notifications = 0u; }
    after_each() {
        allocations.fail_at = 0u;
        check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
        check_equal(turbowasm_component_host_value_destroy(&public_result), TURBOWASM_OK);
        turbowasm_component_exec_call_destroy(&call);
        turbowasm_component_exec_destroy(&exec);
        turbowasm_component_binary_destroy(&binary);
        turbowasm_component_call_destroy(&public_call);
        turbowasm_component_instance_destroy(&instance);
        turbowasm_component_destroy(&component);
        check_equal(allocations.live, 0u);
    }
    it("copies indirect results before cleanup overwrites guest memory") {
        uint8_t bytes[TEXT_SIZE], zeros[TEXT_SIZE] = {0};
        setup();
        check_equal(invoke("text", &result), TURBOWASM_OK);
        check_equal(result.as.string.size, TEXT_SIZE);
        check_equal(memcmp(result.as.string.data, "hello", TEXT_SIZE), 0);
        check_equal(number("calls"), 1u); check_equal(number("last"), 0u);
        check_equal(turbowasm_instance_memory_read_bytes(exec.core_instances[0].impl,
            0u, TEXT_ADDRESS, 0u, bytes, sizeof(bytes)), TURBOWASM_OK);
        check_equal(memcmp(bytes, zeros, sizeof(bytes)), 0);
    }
    it("passes exact scalar bits and calls cleanup for empty results") {
        setup();
        check_equal(invoke("scalar", &result), TURBOWASM_OK); check_equal(result.as.u32, 42u);
        check_equal(number("last"), 42u);
        check_equal(invoke("wide", &result), TURBOWASM_OK);
        check_equal(result.as.u64, UINT64_C(0x123456789abcdef0)); check_equal(number("last"), 64u);
        check_equal(invoke("float", &result), TURBOWASM_OK); check_equal(result.as.f64, -3.5);
        check_equal(number("last"), 65u);
        check_equal(invoke("nothing", NULL), TURBOWASM_OK); check_equal(number("last"), 99u);
        check_equal(number("calls"), 4u);
    }
    it("resolves cleanup in a different Core instance") {
        setup(); check_equal(invoke("cross", &result), TURBOWASM_OK);
        check_equal(result.as.u32, 42u); check_equal(number("other-last"), 42u);
        check_equal(number("calls"), 0u);
    }
    it("skips cleanup when the Core call or result lifting fails") {
        setup();
        check_equal(invoke("fail", &result), TURBOWASM_TRAPPED); check_equal(number("calls"), 0u);
        check_not_equal(invoke("bad-text", &result), TURBOWASM_OK); check_equal(number("calls"), 0u);
        check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
    }
    it("discards copied results on cleanup trap or unhandled Core exception") {
        const char *names[] = {"trap", "throw"}; size_t i;
        setup();
        for (i = 0u; i < sizeof(names) / sizeof(*names); ++i) {
            check_equal(invoke(names[i], &result), TURBOWASM_TRAPPED);
            check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
            check_true(exec.may_leave); check_equal(number("calls"), i + 1u);
        }
    }
    it("keeps cleanup in the original fuel budget and runs it once") {
        turbowasm_status status; unsigned resumes = 0u; bool in_cleanup = false;
        setup(); create_call("text");
        do {
            status = resume_one(); check_true(++resumes < MAX_RESUMES);
            if (!exec.may_leave) {
                in_cleanup = true;
                check_equal(status, TURBOWASM_YIELDED);
                check_equal(turbowasm_component_exec_call_result_count(&call), 0u);
                check_equal(turbowasm_component_exec_call_yield_reason_get(&call), TURBOWASM_YIELD_FUEL);
            }
        } while (status == TURBOWASM_YIELDED);
        check_equal(status, TURBOWASM_OK); check_true(in_cleanup); check_true(exec.may_leave);
        check_equal(turbowasm_component_exec_call_take_result(&call, &result), TURBOWASM_OK);
        check_equal(memcmp(result.as.string.data, "hello", TEXT_SIZE), 0);
        check_equal(number("calls"), 1u);
        check_equal(resume_one(), TURBOWASM_INVALID_ARGUMENT); check_equal(number("calls"), 1u);
    }
    it("cancels a suspended cleanup and releases its private copied result") {
        unsigned resumes = 0u; size_t baseline;
        setup(); baseline = allocations.live; create_call("text");
        do { check_equal(resume_one(), TURBOWASM_YIELDED); check_true(++resumes < MAX_RESUMES); }
        while (exec.may_leave);
        turbowasm_component_exec_call_destroy(&call);
        check_true(exec.may_leave); check_equal(allocations.live, baseline);
        check_equal(invoke("scalar", &result), TURBOWASM_OK);
    }
    it("blocks canonical leave before invoking its host provider") {
        turbowasm_component_exec_imports imports = {0};
        turbowasm_component_value argument = {0}; turbowasm_trap trap;
        check_equal(turbowasm_component_binary_load(&binary, component_post_return_leave_bytes,
            sizeof(component_post_return_leave_bytes)), TURBOWASM_OK);
        imports.can_bind = can_bind; imports.invoke = notify_host;
        check_equal(turbowasm_component_exec_init_with_imports(&exec, &binary, &imports), TURBOWASM_OK);
        check_equal(invoke("run", NULL), TURBOWASM_TRAPPED); check_equal(notifications, 1u);
        check_true(exec.may_leave);
        create_call("run");
        check_equal(turbowasm_component_exec_call_resume(&call, NULL), TURBOWASM_TRAPPED);
        check_equal(notifications, 2u); check_true(exec.may_leave);
        turbowasm_component_exec_call_destroy(&call);
        check_equal(invoke("direct", NULL), TURBOWASM_TRAPPED);
        check_equal(notifications, 3u); check_true(exec.may_leave);
        create_call("direct");
        check_equal(turbowasm_component_exec_call_resume(&call, NULL), TURBOWASM_TRAPPED);
        check_equal(notifications, 4u); check_true(exec.may_leave);
        turbowasm_component_exec_call_destroy(&call);
        argument.kind = TURBOWASM_COMPONENT_TYPE_STRING;
        argument.as.string.data = (uint8_t *)"hello"; argument.as.string.size = TEXT_SIZE;
        check_equal(turbowasm_component_exec_invoke_export(&exec, (const uint8_t *)"text",
            4u, &argument, 1u, NULL, &trap), TURBOWASM_TRAPPED);
        check_equal(notifications, 4u); check_true(exec.may_leave);
        check_equal(turbowasm_component_exec_call_create(&call, &exec, (const uint8_t *)"text",
            4u, &argument, 1u), TURBOWASM_TRAPPED);
        check_equal(notifications, 4u); check_true(exec.may_leave);
    }
    it("traps direct and indirect resource mutations before cleanup side effects") {
        const char *names[] = {"direct-drop", "indirect-drop", "indirect-new"};
        size_t i; uint32_t destroyed = 0u;
        setup_fixture(component_post_return_resources_bytes, sizeof(component_post_return_resources_bytes));
        attach_core_backends();
        for (i = 0u; i < sizeof(names) / sizeof(*names); ++i) {
            check_equal(invoke(names[i], &result), TURBOWASM_TRAPPED);
            check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
            check_equal(exec.resource_table.live_count, 1u);
            check_equal(number("drops"), destroyed);
            check_true(exec.may_leave);
            check_equal(invoke("cleanup", NULL), TURBOWASM_OK); ++destroyed;
            check_equal(exec.resource_table.live_count, 0u);
            create_call(names[i]);
            check_equal(turbowasm_component_exec_call_resume(&call, NULL), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_exec_call_result_count(&call), 0u);
            check_equal(exec.resource_table.live_count, 1u);
            check_equal(number("drops"), destroyed);
            turbowasm_component_exec_call_destroy(&call);
            check_equal(invoke("cleanup", NULL), TURBOWASM_OK); ++destroyed;
            check_equal(number("drops"), destroyed);
        }
    }
    it("permits resource.rep inside cleanup without changing ownership") {
        setup_fixture(component_post_return_resources_bytes, sizeof(component_post_return_resources_bytes));
        attach_core_backends();
        check_equal(invoke("indirect-rep", &result), TURBOWASM_OK);
        check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_U32);
        check_equal(exec.resource_table.live_count, 1u); check_equal(number("drops"), 0u);
        check_equal(invoke("cleanup", NULL), TURBOWASM_OK); check_equal(number("drops"), 1u);
    }
    it("rejects direct new and rep cleanup targets because they return values") {
        uint32_t i, checked = 0u;
        check_equal(turbowasm_component_binary_load(&binary, component_post_return_resources_bytes,
            sizeof(component_post_return_resources_bytes)), TURBOWASM_OK);
        for (i = 0u; i < binary.resource_builtin_count; ++i) {
            if (binary.resource_builtins[i].kind == TURBOWASM_COMPONENT_RESOURCE_BUILTIN_DROP)
                continue;
            binary.canon_lifts[0].post_return_function_index = binary.resource_builtins[i].core_function_index;
            check_equal(turbowasm_component_exec_init(&exec, &binary), TURBOWASM_TYPE_MISMATCH);
            ++checked;
        }
        check_equal(checked, 2u);
    }
    it("validates the cleanup signature before instantiation succeeds") {
        check_equal(turbowasm_component_binary_load(&binary, component_post_return_bytes,
            sizeof(component_post_return_bytes)), TURBOWASM_OK);
        /* The scalar producer has a result, so it cannot serve as post-return. */
        binary.canon_lifts[0].post_return_function_index = binary.canon_lifts[2].core_function_index;
        check_equal(turbowasm_component_exec_init(&exec, &binary), TURBOWASM_TYPE_MISMATCH);
    }
    it("rejects duplicate, missing and out-of-range cleanup indices and lower options") {
        static const uint8_t duplicate[] = {OPTIONS_PREFIX,
            0x08,0x0e,0x02,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x05,0x00,0x05,0x00,0x00};
        static const uint8_t invalid[] = {OPTIONS_PREFIX,
            0x08,0x0c,0x02,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x05,0x01,0x00};
        static const uint8_t missing[] = {OPTIONS_PREFIX,
            0x08,0x0a,0x02,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x05};
        static const uint8_t lower[] = {OPTIONS_PREFIX,
            0x08,0x07,0x01,0x01,0x00,0x00,0x01,0x05,0x00};
        check_equal(turbowasm_component_binary_load(&binary, duplicate, sizeof(duplicate)), TURBOWASM_MALFORMED_MODULE);
        check_equal(turbowasm_component_binary_load(&binary, invalid, sizeof(invalid)), TURBOWASM_MALFORMED_MODULE);
        check_equal(turbowasm_component_binary_load(&binary, missing, sizeof(missing)), TURBOWASM_MALFORMED_MODULE);
        check_equal(turbowasm_component_binary_load(&binary, lower, sizeof(lower)), TURBOWASM_MALFORMED_MODULE);
    }
    it("retains public instance owners until cleanup and result publication finish") {
        turbowasm_execution_options options = {0}; turbowasm_status status; unsigned resumes = 0u;
        check_equal(turbowasm_component_load_borrowed(&component, component_post_return_bytes,
            sizeof(component_post_return_bytes)), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_create(&instance, &component), TURBOWASM_OK);
        check_equal(turbowasm_component_call_create(&public_call, &instance, name("text"), NULL, 0u), TURBOWASM_OK);
        turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
        options.has_fuel_limit = true; options.fuel = 1u;
        do { status = turbowasm_component_call_resume(&public_call, &options); check_true(++resumes < MAX_RESUMES); }
        while (status == TURBOWASM_YIELDED);
        check_equal(status, TURBOWASM_OK);
        check_equal(turbowasm_component_call_take_result(&public_call, &public_result), TURBOWASM_OK);
        check_equal(memcmp(public_result.as.string.data, "hello", TEXT_SIZE), 0);
    }
    it("uses i64 indirect result pointers for memory64 cleanup") {
        size_t count; turbowasm_trap trap;
        setup_public(component_post_return_memory64_bytes, sizeof(component_post_return_memory64_bytes));
        check_equal(turbowasm_component_instance_invoke(&instance, name("text"), NULL, 0u,
            &public_result, 1u, &count, &trap), TURBOWASM_OK);
        check_equal(count, 1u); check_equal(public_result.as.string.size, TEXT_SIZE);
        check_equal(memcmp(public_result.as.string.data, "hello", TEXT_SIZE), 0);
        check_equal(turbowasm_component_host_value_destroy(&public_result), TURBOWASM_OK);
        check_equal(turbowasm_component_instance_invoke(&instance, name("posts"), NULL, 0u,
            &public_result, 1u, &count, &trap), TURBOWASM_OK);
        check_equal(public_result.as.u64, 1u);
    }
    it("releases a lifted owned resource when cleanup fails without publishing it") {
        size_t count; turbowasm_trap trap;
        setup_public(component_post_return_resources_bytes, sizeof(component_post_return_resources_bytes));
        check_equal(turbowasm_component_instance_invoke(&instance, name("trap"), NULL, 0u,
            &public_result, 1u, &count, &trap), TURBOWASM_TRAPPED);
        check_equal(count, 0u); check_equal(drops(), 1u);
        check_equal(turbowasm_component_call_create(&public_call, &instance, name("trap"), NULL, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_call_resume(&public_call, NULL), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_call_result_count(&public_call), 0u);
        check_equal(drops(), 2u);
    }
    it("keeps returned resources alive beyond successful cleanup and call destruction") {
        setup_public(component_post_return_resources_bytes, sizeof(component_post_return_resources_bytes));
        check_equal(turbowasm_component_call_create(&public_call, &instance, name("make"), NULL, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_call_resume(&public_call, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_call_take_result(&public_call, &public_result), TURBOWASM_OK);
        turbowasm_component_call_destroy(&public_call);
        check_equal(drops(), 0u);
        check_equal(turbowasm_component_host_value_destroy(&public_result), TURBOWASM_OK);
        check_equal(drops(), 1u);
    }
    it("balances allocations on every invocation and cleanup allocation failure") {
        size_t distance; bool reached_success = false;
        setup();
        /* Runtime invocation and string lifting share the tracked allocator. */
        for (distance = 1u; distance < MAX_RESUMES; ++distance) {
            size_t baseline = allocations.live;
            turbowasm_status status;
            allocations.fail_at = allocations.attempts + distance;
            status = invoke("text", &result);
            allocations.fail_at = 0u;
            if (status == TURBOWASM_OK) {
                check_equal(turbowasm_component_value_destroy(&result), TURBOWASM_OK);
                reached_success = true; break;
            }
            check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            check_equal(result.kind, TURBOWASM_COMPONENT_TYPE_UNDEFINED);
            check_equal(allocations.live, baseline);
            check_true(exec.may_leave);
        }
        check_true(reached_success);
    }
#ifdef TURBOWASM_TEST_MIR
    it("suspends generated cleanup frames and preserves the lifted result") {
        turbowasm_jit_backend backend = {0};
        turbowasm_instance_impl *core;
        const turbowasm_component_core_call_adapter *adapter;
        turbowasm_status status; unsigned resumes = 0u; bool native_yield = false;
        setup(); core = exec.core_instances[0].impl; adapter = &exec.functions[0];
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(core, &backend, 1u), TURBOWASM_OK);
        create_call("text");
        do {
            status = resume_one(); check_true(++resumes < MAX_RESUMES);
            if (!exec.may_leave) {
                check_equal(core->jit_functions[adapter->function_index].state, TURBOWASM_JIT_COMPILED);
                check_equal(core->jit_functions[adapter->post_return_function_index].state, TURBOWASM_JIT_COMPILED);
                native_yield = true;
            }
        } while (status == TURBOWASM_YIELDED);
        check_equal(status, TURBOWASM_OK); check_true(native_yield);
        check_equal(turbowasm_component_exec_call_take_result(&call, &result), TURBOWASM_OK);
        check_equal(memcmp(result.as.string.data, "hello", TEXT_SIZE), 0);
        check_equal(number("calls"), 1u);
    }
#endif
}
