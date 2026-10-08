#include "component_task.h"
#include "component_subtask.h"
#include "component_endpoint_builtin.h"
#include "execution_internal.h"
#include "module_internal.h"
#include "instance_internal.h"
#include "runtime_alloc.h"
#include <string.h>

typedef struct turbowasm_component_task_owned_set {
    turbowasm_component_waitable_set set;
    struct turbowasm_component_task_owned_set *next;
} turbowasm_component_task_owned_set;

static turbowasm_value_kind flat_kind(turbowasm_component_flat_type type) {
    static const turbowasm_value_kind kinds[] = {
        TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I64, TURBOWASM_VALUE_F32, TURBOWASM_VALUE_F64
    };
    return (unsigned)type < sizeof(kinds) / sizeof(kinds[0]) ? kinds[type] : (turbowasm_value_kind)0;
}

static bool signature_matches(turbowasm_instance *instance, uint32_t index,
    const turbowasm_component_flat_signature *expected) {
    const turbowasm_module *module = turbowasm_instance_module(instance);
    turbowasm_function_signature actual;
    uint32_t i;
    if (!turbowasm_module_function_signature_get(module, index, &actual) ||
        actual.param_count != expected->param_count || actual.result_count != expected->result_count)
        return false;
    for (i = 0; i < actual.param_count; ++i)
        if (!cmeta_type_equal(turbowasm_module_function_param_type(module, index, i),
                turbowasm_value_type_descriptor(flat_kind(expected->params[i]))))
            return false;
    for (i = 0; i < actual.result_count; ++i)
        if (!cmeta_type_equal(turbowasm_module_function_result_type(module, index, i),
                turbowasm_value_type_descriptor(flat_kind(expected->results[i]))))
            return false;
    return true;
}

bool turbowasm_component_task_domain_init(turbowasm_component_task_domain *domain,
    turbowasm_component_resource_table *table, bool *may_leave, uint32_t limit) {
    if (domain == NULL || domain->table != NULL || table == NULL || table->max_entries == 0u ||
        may_leave == NULL || limit == 0u)
        return false;
    memset(domain, 0, sizeof(*domain));
    domain->table = table; domain->may_leave = may_leave; domain->limit = limit;
    return true;
}

turbowasm_status turbowasm_component_task_domain_destroy(turbowasm_component_task_domain *domain) {
    if (domain == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (domain->count != 0u || domain->active != NULL || domain->exclusive != NULL || domain->auxiliary != NULL)
        return TURBOWASM_TRAPPED;
    turbowasm_component_endpoint_domain_collect(domain);
    if (domain->pairs != NULL) return TURBOWASM_TRAPPED;
    while (domain->sets != NULL) {
        turbowasm_status status = turbowasm_component_task_set_drop(domain, domain->sets->set.handle);
        if (status != TURBOWASM_OK) return status;
    }
    memset(domain, 0, sizeof(*domain));
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_task_set_new(turbowasm_component_task_domain *domain,
    turbowasm_component_resource_handle *out) {
    turbowasm_component_task_owned_set *owned;
    turbowasm_status status;
    if (domain == NULL || domain->table == NULL || out == NULL) return TURBOWASM_INVALID_ARGUMENT;
    owned = turbowasm_rt_calloc(1u, sizeof(*owned));
    if (owned == NULL) return TURBOWASM_OUT_OF_MEMORY;
    status = turbowasm_component_waitable_set_register(domain->table, &owned->set);
    if (status != TURBOWASM_OK) { turbowasm_rt_free(owned); return status; }
    owned->next = domain->sets; domain->sets = owned;
    *out = owned->set.handle;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_task_set_drop(turbowasm_component_task_domain *domain,
    turbowasm_component_resource_handle handle) {
    turbowasm_component_task_owned_set **link;
    turbowasm_status status;
    if (domain == NULL || domain->table == NULL) return TURBOWASM_INVALID_ARGUMENT;
    /* Find ownership before unregistering, which clears the set's handle. */
    for (link = &domain->sets; *link != NULL; link = &(*link)->next)
        if ((*link)->set.handle == handle) break;
    status = turbowasm_component_waitable_set_drop(domain->table, handle);
    if (status == TURBOWASM_OK && *link != NULL) {
        turbowasm_component_task_owned_set *owned = *link;
        *link = owned->next; turbowasm_rt_free(owned);
    }
    return status;
}

turbowasm_status turbowasm_component_task_backpressure(turbowasm_component_task_domain *domain, bool increment) {
    if (domain == NULL || domain->table == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (increment ? domain->backpressure == UINT16_MAX : domain->backpressure == 0u)
        return TURBOWASM_TRAPPED;
    if (increment) ++domain->backpressure;
    else --domain->backpressure;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_task_binding_validate(
    const turbowasm_component_task_binding *binding, turbowasm_component_flat_signature *out) {
    const turbowasm_component_type *type;
    turbowasm_component_flat_signature signature;
    turbowasm_status status;
    if (binding == NULL || binding->instance == NULL || binding->graph == NULL || out == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    type = turbowasm_component_type_graph_get(binding->graph, binding->function_type);
    if (type == NULL || type->kind != TURBOWASM_COMPONENT_TYPE_FUNCTION || !type->as.function.is_async)
        return TURBOWASM_TYPE_MISMATCH;
    if (binding->host_entry != NULL && (binding->callback_instance != NULL ||
        turbowasm_instance_module(binding->instance) == NULL)) return TURBOWASM_INVALID_ARGUMENT;
    status = turbowasm_component_canonical_flatten_function_abi(binding->graph, binding->function_type,
        binding->memory.pointer_type, TURBOWASM_COMPONENT_CANONICAL_LIFT,
        binding->callback_instance ? TURBOWASM_COMPONENT_ABI_ASYNC_CALLBACK : TURBOWASM_COMPONENT_ABI_ASYNC, &signature);
    if (status != TURBOWASM_OK) return status;
    if (binding->host_entry != NULL) {
        memset(out, 0, sizeof(*out));
        return TURBOWASM_OK;
    }
    if (!signature_matches(binding->instance, binding->function_index, &signature))
        return TURBOWASM_TYPE_MISMATCH;
    if (binding->callback_instance != NULL) {
        turbowasm_component_flat_signature callback = {0};
        callback.param_count = 3u; callback.result_count = 1u;
        if (!signature_matches(binding->callback_instance, binding->callback_index, &callback))
            return TURBOWASM_TYPE_MISMATCH;
    }
    *out = signature;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_task_create(turbowasm_component_task *task,
    turbowasm_component_task_domain *domain, const turbowasm_component_task_binding *binding) {
    turbowasm_component_flat_signature signature;
    turbowasm_status status;
    if (task == NULL || task->domain != NULL || domain == NULL || domain->table == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    status = turbowasm_component_task_binding_validate(binding, &signature);
    if (status != TURBOWASM_OK) return status;
    if (signature.param_count != 0u && binding->prepare == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if ((binding->resolve == NULL) != (binding->abandon == NULL))
        return TURBOWASM_INVALID_ARGUMENT;
    if (domain->count == domain->limit) return TURBOWASM_OUT_OF_MEMORY;
    memset(task, 0, sizeof(*task));
    task->domain = domain; task->binding = *binding; task->signature = signature;
    ++domain->count;
    return TURBOWASM_OK;
}

static bool resolved(const turbowasm_component_task *task) {
    return task->phase == TURBOWASM_COMPONENT_TASK_RETURNED || task->phase == TURBOWASM_COMPONENT_TASK_CANCELLED;
}

static void abandon_caller(turbowasm_component_task *task, turbowasm_status status) {
    turbowasm_component_task_abandon_fn abandon = task->binding.abandon;
    void *context = task->binding.caller_context;
    task->binding.resolve = NULL; task->binding.abandon = NULL; task->binding.caller_context = NULL;
    if (abandon != NULL) abandon(context, status);
}

static turbowasm_status resolve_caller(turbowasm_component_task *task,
    turbowasm_component_value *result, bool cancelled) {
    turbowasm_status status;
    if (task->binding.resolve == NULL) return TURBOWASM_OK;
    task->resolving = true;
    status = task->binding.resolve(task->binding.caller_context, result, cancelled);
    task->resolving = false;
    if (status == TURBOWASM_OK) {
        task->binding.resolve = NULL; task->binding.abandon = NULL; task->binding.caller_context = NULL;
        task->result_taken = !cancelled;
    }
    return status;
}

static turbowasm_status release_wait(turbowasm_component_task *task) {
    turbowasm_status status = TURBOWASM_OK;
    if (task->waiting_set != 0u) {
        status = turbowasm_component_waitable_set_wait_release(task->domain->table, task->waiting_set);
        task->waiting_set = 0u;
    }
    return status;
}

/* Core and wait pins must already be unwound. A failed dependency keeps the
 * task/caller alive: its counter address cannot be reclaimed while lent. */
static bool clear_borrow_scope(turbowasm_component_task *task, turbowasm_status *cleanup) {
    turbowasm_component_task *saved_active;
    turbowasm_status status;
    if (task->borrowed_handles == 0u) return true;
    saved_active = task->domain->active;
    task->domain->active = NULL;
    status = turbowasm_component_subtask_abort_children(task);
    task->domain->active = saved_active;
    if (*cleanup == TURBOWASM_OK) *cleanup = status;
    if (task->children != NULL) return false;
    status = turbowasm_component_resource_scope_clear(task->domain->table, &task->borrowed_handles);
    if (*cleanup == TURBOWASM_OK) *cleanup = status;
    return status == TURBOWASM_OK;
}

static turbowasm_status finish(turbowasm_component_task *task, turbowasm_status status) {
    turbowasm_status cleanup = release_wait(task);
    if (status == TURBOWASM_OK) status = cleanup;
    if (task->domain->exclusive == task) task->domain->exclusive = NULL;
    task->between_callbacks = false;
    task->status = status;
    if (status != TURBOWASM_OK) {
        /* An unresolved caller may release loans after abandonment. Unwind every
         * retained Core callback before allowing that boundary to detach. */
        task->destroying = true;
        turbowasm_execution_destroy(&task->core);
        if (clear_borrow_scope(task, &cleanup)) abandon_caller(task, status);
        task->destroying = false;
    }
    task->state = status == TURBOWASM_OK ? TURBOWASM_EXECUTION_COMPLETED
        : status == TURBOWASM_TRAPPED ? TURBOWASM_EXECUTION_TRAPPED
        : status == TURBOWASM_EXCEPTION ? TURBOWASM_EXECUTION_EXCEPTION : TURBOWASM_EXECUTION_FAILED;
    if (status == TURBOWASM_TRAPPED && task->trap == TURBOWASM_TRAP_NONE)
        task->trap = TURBOWASM_TRAP_UNREACHABLE;
    return status;
}

static bool can_start(const turbowasm_component_task *task) {
    return task->domain->backpressure == 0u &&
        (task->binding.callback_instance == NULL || task->domain->exclusive == NULL);
}

static turbowasm_status prepare_core(void *context, turbowasm_value *reserved, size_t reserved_count,
    turbowasm_jit_execution_control *control, turbowasm_trap *trap) {
    turbowasm_component_task *task = context;
    turbowasm_value arguments[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS] = {0};
    size_t count = 0u, i;
    turbowasm_status status;
    (void)control;
    task->phase = TURBOWASM_COMPONENT_TASK_STARTED;
    if (task->binding.prepare != NULL) {
        status = task->binding.prepare(task->binding.prepare_context, task, arguments,
            TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS, &count);
        if (status != TURBOWASM_OK) { *trap = task->trap; return status; }
    }
    if (count != task->signature.param_count || count != reserved_count) return TURBOWASM_TYPE_MISMATCH;
    for (i = 0u; i < count; ++i)
        if (arguments[i].kind != flat_kind(task->signature.params[i])) return TURBOWASM_TYPE_MISMATCH;
    if (count != 0u) memcpy(reserved, arguments, count * sizeof(*reserved));
    return TURBOWASM_OK;
}

static turbowasm_status run_host_entry(void *context, turbowasm_host_call *call, turbowasm_trap *trap) {
    turbowasm_component_task *task = context;
    turbowasm_status status = task->binding.host_entry(task->binding.host_context, task, call);
    if (task->trap != TURBOWASM_TRAP_NONE) *trap = task->trap;
    return status;
}

static turbowasm_status start_core(turbowasm_component_task *task) {
    turbowasm_value arguments[TURBOWASM_COMPONENT_MAX_LOWERED_PARAMS] = {0};
    turbowasm_status status;
    uint32_t i;
    /* Reserve numeric carrier storage before touching any caller values. The
     * preparation hook executes on the same coroutine as realloc and entry. */
    for (i = 0u; i < task->signature.param_count; ++i)
        arguments[i].kind = flat_kind(task->signature.params[i]);
    task->core_instance = task->binding.instance;
    status = task->binding.host_entry != NULL
        ? turbowasm_execution_create_host_entry(&task->core, task->core_instance, run_host_entry, task)
        : turbowasm_execution_create(&task->core, task->core_instance,
            task->binding.function_index, arguments, task->signature.param_count);
    if (status == TURBOWASM_OK)
        status = turbowasm_execution_set_prepare(&task->core, prepare_core, task);
    return status;
}

static turbowasm_status start_callback(turbowasm_component_task *task) {
    turbowasm_component_event event = {0};
    turbowasm_value arguments[3] = {0};
    turbowasm_status status;
    if (task->domain->exclusive != NULL) return TURBOWASM_YIELDED;
    if (turbowasm_component_task_deliver_cancel(task->domain)) {
        event.code = TURBOWASM_COMPONENT_EVENT_TASK_CANCELLED;
    } else if (task->waiting_set != 0u) {
        status = turbowasm_component_waitable_set_poll(task->domain->table, task->waiting_set, &event);
        if (status != TURBOWASM_OK) return status;
        if (event.code == TURBOWASM_COMPONENT_EVENT_NONE) return TURBOWASM_YIELDED;
    }
    status = release_wait(task);
    if (status != TURBOWASM_OK) return status;
    arguments[0].kind = arguments[1].kind = arguments[2].kind = TURBOWASM_VALUE_I32;
    arguments[0].as.i32 = (int32_t)event.code;
    arguments[1].as.i32 = (int32_t)event.handle;
    arguments[2].as.i32 = (int32_t)event.payload;
    task->core_instance = task->binding.callback_instance;
    status = turbowasm_execution_create(&task->core, task->core_instance,
        task->binding.callback_index, arguments, 3u);
    if (status == TURBOWASM_OK) {
        task->between_callbacks = false;
        task->domain->exclusive = task;
    }
    return status;
}

static turbowasm_status resume_active(turbowasm_component_task *task,
    const turbowasm_execution_options *options, turbowasm_jit_execution_control *parent) {
    turbowasm_status status;
    turbowasm_execution_options inherited = {0};
    uint32_t packed = 0u, code;
    if (task->phase == TURBOWASM_COMPONENT_TASK_INITIAL) {
        if (!can_start(task)) return TURBOWASM_YIELDED;
        if (task->binding.callback_instance != NULL) task->domain->exclusive = task;
        status = start_core(task);
    } else if (task->between_callbacks) {
        status = start_callback(task);
    } else {
        status = TURBOWASM_OK;
    }
    if (status == TURBOWASM_YIELDED) return status;
    if (status != TURBOWASM_OK) return finish(task, status);
    if (task->builtin_wait != TURBOWASM_COMPONENT_TASK_WAIT_NONE) {
        turbowasm_host_wait wait;
        bool ready = task->builtin_wait == TURBOWASM_COMPONENT_TASK_WAIT_YIELD;
        if (!ready) {
            if (task->builtin_wait == TURBOWASM_COMPONENT_TASK_WAIT_ENDPOINT) {
                bool can_unwind;
                status = turbowasm_component_endpoint_builtin_ready(task->domain, task->builtin_wait_set, &ready, &can_unwind);
            } else status = task->builtin_wait == TURBOWASM_COMPONENT_TASK_WAIT_SUBTASK
                    ? turbowasm_component_subtask_cancel_ready(task->domain->table, task->builtin_wait_set, &ready)
                    : turbowasm_component_waitable_set_ready(task->domain->table, task->builtin_wait_set, &ready);
            if (status != TURBOWASM_OK) return finish(task, status);
        }
        if (!ready) return TURBOWASM_YIELDED;
        if (!turbowasm_execution_pending_host_wait(&task->core, &wait) || wait.operation_token != (uintptr_t)task)
            return finish(task, TURBOWASM_TRAPPED);
        status = turbowasm_execution_complete_host_wait(&task->core, wait, 0);
        if (status != TURBOWASM_OK) return finish(task, status);
    }
    if (parent != NULL) {
        inherited.has_fuel_limit = parent->fuel_limited; inherited.fuel = parent->fuel_remaining;
        inherited.should_interrupt = parent->should_interrupt; inherited.interrupt_context = parent->interrupt_context;
        options = &inherited;
    }
    status = turbowasm_execution_resume(&task->core, options);
    if (parent != NULL && parent->fuel_limited)
        parent->fuel_remaining = turbowasm_execution_control_get(&task->core)->fuel_remaining;
    if (status == TURBOWASM_YIELDED) return status;
    if (status != TURBOWASM_OK) {
        task->trap = turbowasm_execution_trap(&task->core);
        /* Canonical calls trap on an uncaught Core exception; the exception
         * cannot escape into another Component's dynamic handler stack. */
        if (status == TURBOWASM_EXCEPTION) {
            ((turbowasm_instance_impl *)task->core_instance->impl)->pending_exception = NULL;
            task->trap = TURBOWASM_TRAP_UNREACHABLE;
            status = TURBOWASM_TRAPPED;
        }
        return finish(task, status);
    }
    if (task->binding.callback_instance != NULL) {
        const turbowasm_value *result = turbowasm_execution_result_at(&task->core, 0);
        if (result == NULL || result->kind != TURBOWASM_VALUE_I32 || turbowasm_execution_result_count(&task->core) != 1u)
            return finish(task, TURBOWASM_TYPE_MISMATCH);
        packed = (uint32_t)result->as.i32;
    }
    turbowasm_execution_destroy(&task->core);
    code = packed & 15u;
    if (code > 2u) return finish(task, TURBOWASM_TRAPPED);
    if (code == 0u) return finish(task, resolved(task) && task->borrowed_handles == 0u ? TURBOWASM_OK : TURBOWASM_TRAPPED);
    /* Pending cancellation wins before WAIT handle validation, as in canon lift. */
    if (code == 2u && !(!resolved(task) && task->cancellation_requested && !task->cancellation_delivered)) {
        status = turbowasm_component_waitable_set_wait_acquire(task->domain->table, packed >> 4u);
        if (status != TURBOWASM_OK) return finish(task, status);
        task->waiting_set = packed >> 4u;
    }
    task->between_callbacks = true;
    task->domain->exclusive = NULL;
    return TURBOWASM_YIELDED;
}

static turbowasm_status resume_task(turbowasm_component_task *task,
    const turbowasm_execution_options *options, turbowasm_jit_execution_control *parent) {
    const turbowasm_module_impl *module;
    turbowasm_runtime_scope scope;
    turbowasm_status status;
    turbowasm_component_task *previous;
    if (task == NULL || task->domain == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if ((task->domain->active != NULL && parent == NULL) ||
        task->state == TURBOWASM_EXECUTION_RUNNING || task->destroying) return TURBOWASM_TRAPPED;
    if (task->state >= TURBOWASM_EXECUTION_COMPLETED) return task->status;
    if (task->domain->auxiliary != NULL && task->domain->auxiliary != task) {
        task->state = TURBOWASM_EXECUTION_YIELDED;
        return TURBOWASM_YIELDED;
    }
    module = turbowasm_module_impl_get(turbowasm_instance_module(task->binding.instance));
    if (module == NULL) return TURBOWASM_INVALID_ARGUMENT;
    scope = turbowasm_runtime_scope_enter(&module->config);
    previous = task->domain->active;
    task->domain->active = task; task->state = TURBOWASM_EXECUTION_RUNNING;
    status = resume_active(task, options, parent);
    if (status == TURBOWASM_YIELDED) task->state = TURBOWASM_EXECUTION_YIELDED;
    task->domain->active = previous;
    turbowasm_runtime_scope_leave(scope);
    return status;
}

turbowasm_status turbowasm_component_task_resume(turbowasm_component_task *task,
    const turbowasm_execution_options *options) {
    return resume_task(task, options, NULL);
}

turbowasm_status turbowasm_component_task_resume_from_host(turbowasm_component_task *task,
    const turbowasm_host_call *caller) {
    turbowasm_jit_execution_control *parent = turbowasm_host_call_control(caller);
    if (caller == NULL || caller->impl == NULL || task == NULL ||
        (task->phase != TURBOWASM_COMPONENT_TASK_INITIAL && !task->between_callbacks)) return TURBOWASM_INVALID_ARGUMENT;
    return resume_task(task, NULL, parent);
}

turbowasm_status turbowasm_component_task_request_cancel(turbowasm_component_task *task) {
    if (task == NULL || task->domain == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (task->destroying || task->resolving || resolved(task) || task->cancellation_requested || task->state >= TURBOWASM_EXECUTION_COMPLETED)
        return TURBOWASM_TRAPPED;
    task->cancellation_requested = true;
    if (task->phase == TURBOWASM_COMPONENT_TASK_INITIAL) {
        turbowasm_status status = resolve_caller(task, NULL, true);
        if (status != TURBOWASM_OK) return finish(task, status);
        task->phase = TURBOWASM_COMPONENT_TASK_CANCELLED;
        return finish(task, TURBOWASM_OK);
    }
    /* Driver resumes callback turns to deliver the request; an outstanding
     * Core host wait remains owned by its actual I/O adapter. */
    return TURBOWASM_OK;
}

bool turbowasm_component_task_deliver_cancel(turbowasm_component_task_domain *domain) {
    turbowasm_component_task *task = domain != NULL ? domain->active : NULL;
    if (task == NULL || task->destroying || resolved(task) || !task->cancellation_requested || task->cancellation_delivered)
        return false;
    task->cancellation_delivered = true;
    return true;
}

static turbowasm_component_task *returning_task(turbowasm_component_task_domain *domain) {
    turbowasm_component_task *task;
    if (domain == NULL || domain->may_leave == NULL || !*domain->may_leave || domain->synchronous_depth != 0u) return NULL;
    task = domain->active;
    return task != NULL && !task->destroying && !task->resolving && task->phase == TURBOWASM_COMPONENT_TASK_STARTED && task->borrowed_handles == 0u
        ? task : NULL;
}

turbowasm_status turbowasm_component_task_cancel(turbowasm_component_task_domain *domain) {
    turbowasm_component_task *task = returning_task(domain);
    turbowasm_status status;
    if (task == NULL || !task->cancellation_delivered) return TURBOWASM_TRAPPED;
    status = resolve_caller(task, NULL, true);
    if (status != TURBOWASM_OK) return status;
    task->phase = TURBOWASM_COMPONENT_TASK_CANCELLED;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_task_return(turbowasm_component_task_domain *domain,
    turbowasm_component_value *result) {
    turbowasm_component_task *task = returning_task(domain);
    const turbowasm_component_type *type;
    turbowasm_status status;
    if (task == NULL) return TURBOWASM_TRAPPED;
    type = turbowasm_component_type_graph_get(task->binding.graph, task->binding.function_type);
    if (type->as.function.has_result) {
        if (result == NULL) return TURBOWASM_TYPE_MISMATCH;
        status = turbowasm_component_canonical_validate_value(task->binding.graph, type->as.function.result, result);
        if (status != TURBOWASM_OK) return status;
    } else if (result != NULL) return TURBOWASM_TYPE_MISMATCH;
    status = resolve_caller(task, result, false);
    if (status != TURBOWASM_OK) return status;
    if (!task->result_taken && result != NULL) {
        task->result = *result; memset(result, 0, sizeof(*result));
    }
    task->phase = TURBOWASM_COMPONENT_TASK_RETURNED;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_task_take_result(turbowasm_component_task *task,
    turbowasm_component_value *out) {
    if (task == NULL || task->domain == NULL || out == NULL || out->kind != 0) return TURBOWASM_INVALID_ARGUMENT;
    if (task->domain->active != NULL || task->result_taken) return TURBOWASM_TRAPPED;
    if (task->state >= TURBOWASM_EXECUTION_COMPLETED && task->status != TURBOWASM_OK) return task->status;
    if (!resolved(task)) return TURBOWASM_YIELDED;
    if (task->phase == TURBOWASM_COMPONENT_TASK_CANCELLED) return TURBOWASM_INTERRUPTED;
    *out = task->result; memset(&task->result, 0, sizeof(task->result)); task->result_taken = true;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_component_task_return_flat(turbowasm_component_task_domain *domain,
    const turbowasm_component_type_graph *graph, bool has_result,
    turbowasm_component_type_ref result_type, const turbowasm_component_canonical_memory *memory,
    const turbowasm_value *arguments, size_t argument_count) {
    turbowasm_component_task *task = returning_task(domain);
    turbowasm_component_canonical_memory empty_memory = {0};
    const turbowasm_component_type *function;
    const turbowasm_component_canonical_memory *expected;
    turbowasm_component_flat_signature signature;
    turbowasm_component_value result = {0};
    turbowasm_status status;
    size_t i;
    if (task == NULL) return TURBOWASM_TRAPPED;
    if (memory == NULL) memory = &empty_memory;
    expected = &task->binding.memory;
    function = turbowasm_component_type_graph_get(task->binding.graph, task->binding.function_type);
    if (has_result != function->as.function.has_result ||
        (has_result && !turbowasm_component_value_type_equal(graph, result_type,
            task->binding.graph, function->as.function.result)) ||
        ((memory->instance != NULL || expected->instance != NULL) &&
            (memory->instance == NULL || expected->instance == NULL ||
             !turbowasm_instance_memory_same(memory->instance->impl, memory->memory_index,
                 expected->instance->impl, expected->memory_index))) ||
        memory->pointer_type != expected->pointer_type || memory->string_encoding != expected->string_encoding)
        return TURBOWASM_TRAPPED;
    status = turbowasm_component_canonical_flatten_task_return(graph, has_result, result_type,
        memory->pointer_type, &signature);
    if (status != TURBOWASM_OK) return status;
    if (argument_count != signature.param_count || (argument_count != 0u && arguments == NULL))
        return TURBOWASM_TYPE_MISMATCH;
    for (i = 0; i < argument_count; ++i)
        if (arguments[i].kind != flat_kind(signature.params[i])) return TURBOWASM_TYPE_MISMATCH;
    if (has_result) {
        if (signature.params_indirect) {
            uint64_t address = memory->pointer_type == TURBOWASM_COMPONENT_POINTER_I64
                ? (uint64_t)arguments[0].as.i64 : (uint32_t)arguments[0].as.i32;
            status = turbowasm_component_canonical_lift_value(graph, result_type, memory, address, &result);
        } else {
            status = turbowasm_component_canonical_lift_flat_value(graph, result_type,
                memory, arguments, (uint32_t)argument_count, &result);
        }
        if (status != TURBOWASM_OK) return status;
    }
    status = turbowasm_component_task_return(domain, has_result ? &result : NULL);
    if (status != TURBOWASM_OK) (void)turbowasm_component_value_destroy(&result);
    return status;
}

turbowasm_status turbowasm_component_task_destroy(turbowasm_component_task *task) {
    turbowasm_component_task_domain *domain;
    turbowasm_status status, result_status;
    if (task == NULL) return TURBOWASM_INVALID_ARGUMENT;
    domain = task->domain;
    if (domain == NULL) return TURBOWASM_OK;
    /* An eager lower may retire an already exited, empty child while its parent
     * is active. This path cannot execute guest code or a cleanup callback. */
    if (domain->active != NULL && domain->active != task &&
        task->state >= TURBOWASM_EXECUTION_COMPLETED && task->core.impl == NULL &&
        task->binding.resolve == NULL && task->binding.abandon == NULL &&
        task->result.kind == TURBOWASM_COMPONENT_TYPE_UNDEFINED && task->result.release == NULL &&
        task->borrowed_handles == 0u && task->waiting_set == 0u &&
        task->builtin_wait == TURBOWASM_COMPONENT_TASK_WAIT_NONE && !task->destroying && !task->resolving &&
        domain->exclusive != task && domain->auxiliary != task) {
        turbowasm_component_subtask_detach_children(task);
        --domain->count;
        memset(task, 0, sizeof(*task));
        return TURBOWASM_OK;
    }
    if (domain->active != NULL || task->destroying ||
        (task->resolving && task->state != TURBOWASM_EXECUTION_YIELDED)) return TURBOWASM_TRAPPED;
    if (task->builtin_wait == TURBOWASM_COMPONENT_TASK_WAIT_ENDPOINT) {
        bool ready, can_unwind;
        status = turbowasm_component_endpoint_builtin_ready(domain, task->builtin_wait_set, &ready, &can_unwind);
        if (status != TURBOWASM_OK || !can_unwind) return status != TURBOWASM_OK ? status : TURBOWASM_TRAPPED;
    }
    /* Result lowering may be suspended in guest realloc. Unwind its retained
     * resolve frame before detaching the caller; active reentry remains barred. */
    domain->active = task; task->destroying = true;
    turbowasm_execution_destroy(&task->core);
    status = release_wait(task);
    if (!clear_borrow_scope(task, &status)) {
        task->destroying = false; domain->active = NULL;
        task->state = TURBOWASM_EXECUTION_FAILED; task->status = TURBOWASM_INTERRUPTED;
        return status != TURBOWASM_OK ? status : TURBOWASM_TRAPPED;
    }
    abandon_caller(task, task->status != TURBOWASM_OK ? task->status : TURBOWASM_INTERRUPTED);
    result_status = turbowasm_component_value_destroy(&task->result);
    if (status == TURBOWASM_OK) status = result_status;
    if (domain->exclusive == task) domain->exclusive = NULL;
    turbowasm_component_subtask_detach_children(task);
    domain->active = NULL;
    --domain->count;
    memset(task, 0, sizeof(*task));
    return status;
}
