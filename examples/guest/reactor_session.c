#include "reactor_session.h"
#include <string.h>

static bool enter(reactor_session *s) {
    bool expected = false;
    return s && atomic_compare_exchange_strong_explicit(&s->busy, &expected, true,
        memory_order_acquire, memory_order_relaxed);
}
static void leave(reactor_session *s) {
    atomic_store_explicit(&s->busy, false, memory_order_release);
}
static bool function(const turbowasm_module *module, const char *name, uint32_t *out) {
    if (!module || !name) return false;
    size_t length = strlen(name);
    for (size_t i = 0; i < turbowasm_module_export_count(module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(module, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == length &&
            !memcmp(e->name.bytes, name, length)) {
            *out = e->item_index;
            return true;
        }
    }
    return false;
}
static turbowasm_status invoke(reactor_session *s, uint32_t index,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    turbowasm_execution_options options = {.fuel = s->fuel, .has_fuel_limit = true};
    turbowasm_status status = turbowasm_instance_invoke_with_options(&s->instance,
        index, args, argc, results, capacity, count, trap, &options);
    if (status != TURBOWASM_OK) s->state = REACTOR_FAILED;
    return status;
}
turbowasm_status reactor_session_open(reactor_session *s,
    const turbowasm_module *module, const turbowasm_linker *linker,
    uint64_t fuel, turbowasm_trap *trap) {
    if (!module || !module->impl || !fuel || !trap || !enter(s))
        return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    uint32_t index;
    turbowasm_module_summary summary;
    turbowasm_function_signature signature;
    if (s->state != REACTOR_EMPTY) goto done;
    if (!turbowasm_module_summary_get(module, &summary) || summary.has_start ||
        !function(module, "_initialize", &index) ||
        !turbowasm_module_function_signature_get(module, index, &signature) ||
        signature.param_count || signature.result_count) {
        status = TURBOWASM_UNSUPPORTED;
        goto done;
    }
    s->state = REACTOR_INITIALIZING;
    s->module = module;
    s->fuel = fuel;
    status = linker ? turbowasm_instance_create_linked(&s->instance, module, linker)
                    : turbowasm_instance_create(&s->instance, module);
    if (status == TURBOWASM_OK) {
        size_t count = 0;
        status = invoke(s, index, NULL, 0, NULL, 0, &count, trap);
    }
    s->state = status == TURBOWASM_OK ? REACTOR_READY : REACTOR_FAILED;
done:
    leave(s);
    return status;
}
turbowasm_status reactor_session_call(reactor_session *s, const char *name,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    if (!name || !count || !trap || !enter(s)) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    uint32_t index;
    /* The CRT entry is not a business export. It must never reset a session. */
    if (s->state == REACTOR_READY && strcmp(name, "_initialize") &&
        function(s->module, name, &index))
        status = invoke(s, index, args, argc, results, capacity, count, trap);
    leave(s);
    return status;
}
turbowasm_status reactor_session_close(reactor_session *s,
    const char *name, int32_t *application_status, turbowasm_trap *trap) {
    if (!name || !application_status || !trap || !enter(s))
        return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    uint32_t index;
    turbowasm_function_signature signature;
    if (s->state != REACTOR_READY || !strcmp(name, "_initialize") ||
        !function(s->module, name, &index) ||
        !turbowasm_module_function_signature_get(s->module, index, &signature) ||
        signature.param_count || signature.result_count != 1 ||
        !cmeta_type_equal(turbowasm_module_function_result_type(s->module, index, 0),
            turbowasm_value_type_descriptor(TURBOWASM_VALUE_I32))) goto done;
    s->state = REACTOR_CLOSING;
    turbowasm_value result = {0};
    size_t count = 0;
    status = invoke(s, index, NULL, 0, &result, 1, &count, trap);
    if (status == TURBOWASM_OK) {
        if (count != 1 || result.kind != TURBOWASM_VALUE_I32)
            status = TURBOWASM_TYPE_MISMATCH;
        else
            *application_status = result.as.i32;
    }
    turbowasm_instance_destroy(&s->instance);
    s->module = NULL;
    s->state = REACTOR_CLOSED;
done:
    leave(s);
    return status;
}
bool reactor_session_destroy(reactor_session *s) {
    if (!enter(s)) return false;
    turbowasm_instance_destroy(&s->instance);
    s->module = NULL;
    s->state = REACTOR_CLOSED;
    leave(s);
    return true;
}
