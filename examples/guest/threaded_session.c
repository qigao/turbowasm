#include "threaded_session.h"
#include <string.h>

static bool enter(threaded_session *s) {
    bool expected = false;
    return s && atomic_compare_exchange_strong_explicit(&s->busy, &expected, true,
        memory_order_acquire, memory_order_relaxed);
}
static void leave(threaded_session *s) {
    atomic_store_explicit(&s->busy, false, memory_order_release);
}
static bool function(const turbowasm_module *module, const char *name, uint32_t *index) {
    size_t length = strlen(name);
    for (size_t i = 0; i < turbowasm_module_export_count(module); ++i) {
        const turbowasm_export_desc *e = turbowasm_module_export_at(module, i);
        if (e->kind == TURBOWASM_EXTERN_FUNCTION && e->name.size == length &&
            !memcmp(e->name.bytes, name, length)) { *index = e->item_index; return true; }
    }
    return false;
}
static bool business(const char *name) {
    return strcmp(name, "_initialize") && strcmp(name, "_start") && strcmp(name, "wasi_thread_start");
}
static void failed(threaded_session *s) {
    s->state = THREADED_FAILED;
    /* Root failure must also wake/stop children. Existing first-terminal group
     * status/exit code is preserved by the capability. No guest cleanup here. */
    turbowasm_wasi_threads_proc_exit(s->threads, &s->instance, 1);
}
static turbowasm_status invoke(threaded_session *s, uint32_t index,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    turbowasm_status status = turbowasm_instance_invoke_with_options(&s->instance,
        index, args, argc, results, capacity, count, trap, &s->options);
    if (status != TURBOWASM_OK) failed(s);
    return status;
}
turbowasm_status threaded_session_open(threaded_session *s,
    const turbowasm_module *module, const turbowasm_linker *linker,
    turbowasm_wasi_threads *threads, bool command,
    const turbowasm_execution_options *options, turbowasm_trap *trap) {
    if (!module || !module->impl || !linker || !linker->impl || !threads || !threads->impl ||
        !options || !options->has_fuel_limit || !options->fuel || !trap || !enter(s))
        return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    uint32_t index;
    turbowasm_function_signature signature;
    *trap = TURBOWASM_TRAP_NONE;
    if (s->state != THREADED_EMPTY) goto done;
    if (!function(module, command ? "_start" : "_initialize", &index) ||
        !turbowasm_module_function_signature_get(module, index, &signature) ||
        signature.param_count || signature.result_count) {
        status = TURBOWASM_UNSUPPORTED; goto done;
    }
    s->module = module; s->threads = threads; s->options = *options;
    s->state = THREADED_INITIALIZING;
    if (!turbowasm_wasi_threads_execution_policy_init(&s->policy, threads) ||
        !turbowasm_wasi_threads_execution_policy_apply(&s->policy, &s->options)) {
        failed(s); goto done;
    }
    status = turbowasm_instance_create_linked_with_options(&s->instance, module,
        linker, &s->options, trap);
    if (status == TURBOWASM_OK) {
        size_t count = 0;
        status = invoke(s, index, NULL, 0, NULL, 0, &count, trap);
    }
    if (status == TURBOWASM_OK) s->state = command ? THREADED_CLOSED : THREADED_READY;
    else failed(s);
done:
    leave(s);
    return status;
}
turbowasm_status threaded_session_call(threaded_session *s, const char *name,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    if (!name || !count || !trap || !enter(s)) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    uint32_t index;
    if (s->state == THREADED_READY && business(name) && function(s->module, name, &index))
        status = invoke(s, index, args, argc, results, capacity, count, trap);
    leave(s);
    return status;
}
turbowasm_status threaded_session_close(threaded_session *s, const char *name,
    const turbowasm_value *args, size_t argc, int32_t *application_status,
    turbowasm_trap *trap) {
    if (!name || !application_status || !trap || !enter(s)) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    turbowasm_function_signature signature;
    uint32_t index;
    if ((s->state != THREADED_READY && s->state != THREADED_CLOSING) || !business(name) ||
        !function(s->module, name, &index) ||
        !turbowasm_module_function_signature_get(s->module, index, &signature) ||
        signature.param_count != argc || signature.result_count != 1 ||
        !cmeta_type_equal(turbowasm_module_function_result_type(s->module, index, 0),
            turbowasm_value_type_descriptor(TURBOWASM_VALUE_I32))) goto done;
    s->state = THREADED_CLOSING;
    turbowasm_value result = {0}; size_t count = 0;
    status = invoke(s, index, args, argc, &result, 1, &count, trap);
    if (status == TURBOWASM_OK) {
        *application_status = result.as.i32;
        if (result.as.i32 == 0) s->state = THREADED_CLOSED;
    }
done:
    leave(s);
    return status;
}
bool threaded_session_destroy(threaded_session *s) {
    if (!enter(s)) return false;
    bool safe = !s->threads || turbowasm_wasi_threads_active(s->threads) == 0;
    if (safe) {
        turbowasm_instance_destroy(&s->instance);
        s->module = NULL; s->threads = NULL; s->state = THREADED_CLOSED;
    }
    leave(s);
    return safe;
}
