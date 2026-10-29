#include <tinytest.h>
#include "clock_host.h"
#include "reactor_session.h"
#include <salts/fs.h>
#include <stdlib.h>
#include <string.h>
#ifdef TURBOWASM_TEST_MIR
#include "instance_internal.h"
#include "jit/mir_backend.h"
#endif

typedef struct clock_probe {
    uint64_t value;
    size_t calls;
    bool fail;
} clock_probe;

static turbowasm_module module, multi_module, provider_module;
static salts_fs_buf_t program, multi_program;
static reactor_session first, second;
static turbowasm_instance provider;
static turbowasm_linker linker;
static app_clock first_clock, second_clock;
static clock_probe first_probe, second_probe;
static turbowasm_trap trap;
static size_t allocation_calls, fail_at, live_allocations;

static turbowasm_name name(const char *text) {
    return (turbowasm_name){(const uint8_t *)text, (uint32_t)strlen(text)};
}
static bool read_clock(void *context, uint64_t *out) {
    clock_probe *probe = context;
    ++probe->calls;
    if (probe->fail) return false;
    *out = probe->value;
    return true;
}
static void load(const char *path, turbowasm_module *out, salts_fs_buf_t *bytes) {
    check_equal(salts_fs_read_file(path, bytes), 0);
    check_equal(turbowasm_module_load_borrowed(out,
        (const uint8_t *)bytes->base, bytes->len), TURBOWASM_OK);
}
static void open_session(reactor_session *session, const turbowasm_module *code) {
    check_equal(reactor_session_open(session, code, &linker, 1000000, &trap), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
    turbowasm_jit_backend backend = {0};
    check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
    check_equal(turbowasm_jit_instance_attach_backend(session->instance.impl, &backend, 1), TURBOWASM_OK);
#endif
}
static int64_t call(reactor_session *session, const char *symbol) {
    turbowasm_value result = {0};
    size_t count = 0;
    check_equal(reactor_session_call(session, symbol, NULL, 0,
        &result, 1, &count, &trap), TURBOWASM_OK);
    check_equal(trap, TURBOWASM_TRAP_NONE);
    check_equal(count, (size_t)1);
    check_equal(result.kind, TURBOWASM_VALUE_I64);
#ifdef TURBOWASM_TEST_MIR
    uint32_t index = UINT32_MAX;
    for (size_t i = 0; i < turbowasm_module_export_count(session->module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(session->module, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == strlen(symbol) &&
            !memcmp(e->name.bytes, symbol, e->name.size)) index = e->item_index;
    }
    check_not_equal(index, UINT32_MAX);
    turbowasm_instance_impl *impl = session->instance.impl;
    check_equal(impl->jit_functions[index].state, TURBOWASM_JIT_COMPILED);
#endif
    return result.as.i64;
}
/* A separate test module with the same function name. Its value/state must
 * never be confused with app:clock/v1's binding. */
static turbowasm_status other_now(void *context, turbowasm_host_call *host_call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *out_trap) {
    (void)args;
    check_not_null(turbowasm_host_call_instance(host_call));
    check_equal(argc, (size_t)0);
    check_greater_equal(capacity, (size_t)1);
    clock_probe *probe = context;
    ++probe->calls;
    results[0] = (turbowasm_value){.kind = TURBOWASM_VALUE_I64, .as.i64 = (int64_t)probe->value};
    *count = 1;
    *out_trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}
static turbowasm_status define_other(const char *space, const char *field,
    turbowasm_value_kind kind) {
    const turbowasm_host_function_type type = {NULL, 0, &kind, 1};
    return turbowasm_linker_define_host_function(&linker, name(space), name(field),
        &type, other_now, &second_probe);
}
static void *allocate(void *context, size_t size) {
    (void)context;
    if (++allocation_calls == fail_at) return NULL;
    void *pointer = malloc(size);
    if (pointer) ++live_allocations;
    return pointer;
}
static void deallocate(void *context, void *pointer) {
    (void)context;
    if (pointer) --live_allocations;
    free(pointer);
}

spec("module-scoped compiled guest host calls") {
    before_all() {
        load(GUEST_CLOCK_PATH, &module, &program);
        load(GUEST_CLOCK_MODULES_PATH, &multi_module, &multi_program);
    }
    after_all() {
        turbowasm_module_destroy(&multi_module);
        turbowasm_module_destroy(&module);
        salts_fs_buf_free(&multi_program);
        salts_fs_buf_free(&program);
    }
    before_each() {
        memset(&first, 0, sizeof(first));
        memset(&second, 0, sizeof(second));
        first_probe = (clock_probe){.value = 123};
        second_probe = (clock_probe){.value = 987};
        first_clock = (app_clock){read_clock, &first_probe};
        second_clock = (app_clock){read_clock, &second_probe};
        trap = TURBOWASM_TRAP_NONE;
        allocation_calls = fail_at = live_allocations = 0;
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
    }
    after_each() {
        check_true(reactor_session_destroy(&second));
        check_true(reactor_session_destroy(&first));
        turbowasm_instance_destroy(&provider);
        turbowasm_module_destroy(&provider_module);
        turbowasm_linker_destroy(&linker);
        check_equal(live_allocations, (size_t)0);
    }

    it("binds during initialization and retains independent instance contexts after linker destruction") {
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_OK);
        check_equal(first_probe.calls, (size_t)0);
        open_session(&first, &module);
        check_equal(first_probe.calls, (size_t)1);
        turbowasm_linker_destroy(&linker);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(app_clock_define(&linker, &second_clock), TURBOWASM_OK);
        open_session(&second, &module);
        turbowasm_linker_destroy(&linker);
        first_probe.value = 456;
        second_probe.value = 654;
        check_equal(call(&first, "clock_initial"), INT64_C(123));
        check_equal(call(&second, "clock_initial"), INT64_C(987));
        check_equal(call(&first, "clock_sample"), INT64_C(456));
        check_equal(call(&second, "clock_sample"), INT64_C(654));
        check_equal(first_probe.calls, (size_t)2);
        check_equal(second_probe.calls, (size_t)2);
    }
    it("routes identical function names through distinct modules") {
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_OK);
        check_equal(define_other("test:clock/v1", "now_ns", TURBOWASM_VALUE_I64), TURBOWASM_OK);
        open_session(&first, &multi_module);
        turbowasm_linker_destroy(&linker);
        check_equal(call(&first, "clock_sample"), INT64_C(123));
        check_equal(call(&first, "other_sample"), INT64_C(987));
        check_equal(first_probe.calls, (size_t)2);
        check_equal(second_probe.calls, (size_t)1);
    }
    it("rejects a guest requesting an ungranted module before initialization") {
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_OK);
        check_equal(reactor_session_open(&first, &multi_module, &linker, 1000000, &trap), TURBOWASM_LINK_ERROR);
        check_null(first.instance.impl);
        check_equal(first_probe.calls, (size_t)0);
    }
    it("rejects a missing field and does not substitute another namespace") {
        check_equal(define_other("app:clock/v1", "different", TURBOWASM_VALUE_I64), TURBOWASM_OK);
        check_equal(define_other("app:clock/v2", "now_ns", TURBOWASM_VALUE_I64), TURBOWASM_OK);
        check_equal(reactor_session_open(&first, &module, &linker, 1000000, &trap), TURBOWASM_LINK_ERROR);
        check_null(first.instance.impl);
        check_equal(second_probe.calls, (size_t)0);
    }
    it("rejects an inexact host result signature without invoking the provider") {
        check_equal(define_other("app:clock/v1", "now_ns", TURBOWASM_VALUE_I32), TURBOWASM_OK);
        check_equal(reactor_session_open(&first, &module, &linker, 1000000, &trap), TURBOWASM_TYPE_MISMATCH);
        check_null(first.instance.impl);
        check_equal(second_probe.calls, (size_t)0);
    }
    it("rejects duplicates without replacing the original context") {
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_OK);
        check_equal(app_clock_define(&linker, &second_clock), TURBOWASM_LINK_ERROR);
        open_session(&first, &module);
        check_equal(call(&first, "clock_sample"), INT64_C(123));
        check_equal(second_probe.calls, (size_t)0);
    }
    it("rejects a Wasm provider namespace collision in either registration order") {
        static const uint8_t empty[] = {0, 97, 115, 109, 1, 0, 0, 0};
        check_equal(turbowasm_module_load_borrowed(&provider_module, empty, sizeof(empty)), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&provider, &provider_module), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&linker, name("app:clock/v1"), &provider), TURBOWASM_OK);
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_LINK_ERROR);
        turbowasm_linker_destroy(&linker);
        check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_OK);
        check_equal(turbowasm_linker_define_instance(&linker, name("app:clock/v1"), &provider), TURBOWASM_LINK_ERROR);
    }
    it("makes the context available to a bounded Wasm start and preserves its effects on failure") {
        /* (module (import "app:clock/v1" "now_ns" (func $now (result i64)))
         *   (func $start (if (i64.lt_s (call $now) (i64.const 0))
         *     (then unreachable))) (start $start)) */
        static const uint8_t start[] = {
            0, 97, 115, 109, 1, 0, 0, 0,
            1, 8, 2, 0x60, 0, 1, 0x7e, 0x60, 0, 0,
            2, 23, 1, 12, 'a','p','p',':','c','l','o','c','k','/','v','1',
            6, 'n','o','w','_','n','s', 0, 0,
            3, 2, 1, 1, 8, 1, 1,
            10, 13, 1, 11, 0, 0x10, 0, 0x42, 0, 0x53, 4, 0x40, 0, 11, 11
        };
        turbowasm_execution_options options = {.fuel = 1000, .has_fuel_limit = true};
        check_equal(turbowasm_module_load_borrowed(&provider_module, start, sizeof(start)), TURBOWASM_OK);
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_OK);
        check_equal(turbowasm_instance_create_linked_with_options(&provider,
            &provider_module, &linker, &options, &trap), TURBOWASM_OK);
        check_equal(first_probe.calls, (size_t)1);
        turbowasm_instance_destroy(&provider);
        first_probe.fail = true;
        check_equal(turbowasm_instance_create_linked_with_options(&provider,
            &provider_module, &linker, &options, &trap), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_UNREACHABLE);
        check_null(provider.impl);
        check_equal(first_probe.calls, (size_t)2);
    }
    it("keeps business failures and clock overflow in the guest result channel") {
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_OK);
        open_session(&first, &module);
        first_probe.value = 0;
        check_equal(call(&first, "clock_sample"), INT64_C(0));
        first_probe.value = INT64_MAX;
        check_equal(call(&first, "clock_sample"), INT64_MAX);
        first_probe.value = (uint64_t)INT64_MAX + 1;
        check_equal(call(&first, "clock_sample"), -INT64_C(1));
        first_probe.value = UINT64_MAX;
        check_equal(call(&first, "clock_sample"), -INT64_C(1));
        first_probe.fail = true;
        check_equal(call(&first, "clock_sample"), -INT64_C(1));
        first_probe.fail = false;
        first_probe.value = 7;
        check_equal(call(&first, "clock_sample"), INT64_C(7));
        check_equal(first.state, REACTOR_READY);
    }
    it("propagates initialization traps and allows host teardown without guest cleanup") {
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_OK);
        first_probe.fail = true;
        check_equal(reactor_session_open(&first, &module, &linker, 1000000, &trap), TURBOWASM_TRAPPED);
        check_equal(trap, TURBOWASM_TRAP_UNREACHABLE);
        check_equal(first.state, REACTOR_FAILED);
        check_equal(first_probe.calls, (size_t)1);
        check_true(reactor_session_destroy(&first));
        check_equal(first_probe.calls, (size_t)1);
    }
    it("rejects invalid native configuration before adding a binding") {
        app_clock invalid = {0};
        check_equal(app_clock_define(&linker, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(app_clock_define(&linker, &invalid), TURBOWASM_INVALID_ARGUMENT);
        check_equal(app_clock_define(NULL, &first_clock), TURBOWASM_INVALID_ARGUMENT);
        check_equal(app_clock_define(&linker, &first_clock), TURBOWASM_OK);
        open_session(&first, &module);
        check_equal(call(&first, "clock_sample"), INT64_C(123));
    }
    it("releases a partly composed linker at every clock registration allocation failure") {
        turbowasm_runtime_config config = {0};
        config.allocator.allocate = allocate;
        config.allocator.deallocate = deallocate;
        bool reached_success = false;
        turbowasm_linker_destroy(&linker);
        for (size_t offset = 1; offset <= 32; ++offset) {
            allocation_calls = fail_at = 0;
            check_equal(turbowasm_linker_init_with_config(&linker, &config), TURBOWASM_OK);
            check_equal(define_other("test:clock/v1", "now_ns", TURBOWASM_VALUE_I64), TURBOWASM_OK);
            fail_at = allocation_calls + offset;
            turbowasm_status status = app_clock_define(&linker, &first_clock);
            if (status == TURBOWASM_OK) reached_success = true;
            else check_equal(status, TURBOWASM_OUT_OF_MEMORY);
            /* No consumer is created from a failed composition. */
            turbowasm_linker_destroy(&linker);
            check_equal(live_allocations, (size_t)0);
            check_equal(first_probe.calls, (size_t)0);
            check_equal(second_probe.calls, (size_t)0);
            if (reached_success) break;
        }
        check_true(reached_success);
    }
}
