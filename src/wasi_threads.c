#include <turbowasm/wasi_threads.h>

#include "instance_internal.h"

#include <salts/thread.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    TURBOWASM_WASI_THREADS_TID_LIMIT = 1u << 29
};

typedef struct turbowasm_wasi_threads_impl
    turbowasm_wasi_threads_impl;

typedef struct turbowasm_wasi_thread_slot {
    turbowasm_wasi_threads_impl *owner;
    turbowasm_instance child;
    uint32_t start_function_index;
    uint32_t start_arg;
    int32_t tid;
    bool active;
} turbowasm_wasi_thread_slot;

struct turbowasm_wasi_threads_impl {
    cflow_executor *executor;
    turbowasm_wasi_thread_slot *slots;
    size_t capacity;
    size_t active;
    uint32_t next_tid;
    const turbowasm_module *module;
    uint32_t start_function_index;
    atomic_bool fatal;
    turbowasm_status fatal_status;
    turbowasm_trap fatal_trap;
    salts_mutex_t mutex;
    bool mutex_initialized;
};

static bool turbowasm_wasi_threads_should_interrupt(
    void *context) {
    turbowasm_wasi_threads_impl *impl =
        (turbowasm_wasi_threads_impl *)context;

    return impl == NULL ||
           atomic_load_explicit(
               &impl->fatal, memory_order_acquire);
}

static bool turbowasm_wasi_threads_policy_should_interrupt(
    void *context) {
    turbowasm_wasi_threads_execution_policy *policy =
        (turbowasm_wasi_threads_execution_policy *)context;
    turbowasm_wasi_threads_impl *impl;

    if (policy == NULL || policy->threads == NULL ||
        policy->threads->impl == NULL)
        return true;

    impl = (turbowasm_wasi_threads_impl *)policy->threads->impl;
    if (atomic_load_explicit(
            &impl->fatal, memory_order_acquire))
        return true;

    return policy->chained_interrupt != NULL &&
           policy->chained_interrupt(policy->chained_context);
}

static void turbowasm_wasi_threads_publish_fatal(
    turbowasm_wasi_threads_impl *impl,
    turbowasm_instance *source,
    turbowasm_status status,
    turbowasm_trap trap) {
    bool first = false;

    if (impl == NULL)
        return;

    salts_mutex_lock(&impl->mutex);
    if (!atomic_load_explicit(
            &impl->fatal, memory_order_relaxed)) {
        impl->fatal_status = status;
        impl->fatal_trap = trap;
        atomic_store_explicit(
            &impl->fatal, true, memory_order_release);
        first = true;
    }
    salts_mutex_unlock(&impl->mutex);

    /*
     * Shared imported memories resolve to the same provider waiter registry
     * across the whole group. Signaling through one sibling therefore wakes
     * root/child waiters without turning the wake into a Wasm notify result.
     */
    if (first && source != NULL && source->impl != NULL) {
        turbowasm_instance_interrupt_waiters(
            (turbowasm_instance_impl *)source->impl);
    }
}

static turbowasm_name turbowasm_wasi_threads_name(
    const char *text) {
    turbowasm_name name = {0};
    if (text != NULL) {
        name.bytes = (const uint8_t *)text;
        name.size = (uint32_t)strlen(text);
    }
    return name;
}

static bool turbowasm_wasi_threads_name_equal(
    turbowasm_name name,
    const char *text) {
    size_t length;

    if (text == NULL)
        return false;
    length = strlen(text);
    return length == name.size &&
           (length == 0u ||
            memcmp(name.bytes, text, length) == 0);
}

static bool turbowasm_wasi_threads_i32_type(
    const turbowasm_module *module,
    uint32_t function_index,
    uint32_t parameter_index) {
    const cmeta_type_desc *actual =
        turbowasm_module_function_param_type(
            module, function_index, parameter_index);
    const cmeta_type_desc *expected =
        turbowasm_value_type_descriptor(TURBOWASM_VALUE_I32);

    return actual != NULL && expected != NULL &&
           (actual == expected || cmeta_type_equal(actual, expected));
}

static bool turbowasm_wasi_threads_validate_module(
    const turbowasm_module *module,
    uint32_t *out_start_function) {
    size_t index;
    turbowasm_function_signature signature;

    if (module == NULL || module->impl == NULL ||
        out_start_function == NULL)
        return false;

    for (index = 0u;
         index < turbowasm_module_memory_count(module);
         ++index) {
        turbowasm_memory_desc memory = {0};
        if (!turbowasm_module_memory_at(module, index, &memory))
            return false;

        /*
         * A fresh sibling can only preserve shared-memory identity when the
         * shared memory is an imported object. Defined shared memories would
         * allocate a distinct backing in each child and are therefore rejected.
         */
        if (memory.shared && !memory.imported)
            return false;
    }

    for (index = 0u;
         index < turbowasm_module_export_count(module);
         ++index) {
        const turbowasm_export_desc *export_desc =
            turbowasm_module_export_at(module, index);

        if (export_desc == NULL ||
            export_desc->kind != TURBOWASM_EXTERN_FUNCTION ||
            !turbowasm_wasi_threads_name_equal(
                export_desc->name, "wasi_thread_start"))
            continue;

        if (!turbowasm_module_function_signature_get(
                module, export_desc->item_index, &signature))
            return false;

        if (signature.param_count != 2u ||
            signature.result_count != 0u ||
            !turbowasm_wasi_threads_i32_type(
                module, export_desc->item_index, 0u) ||
            !turbowasm_wasi_threads_i32_type(
                module, export_desc->item_index, 1u))
            return false;

        *out_start_function = export_desc->item_index;
        return true;
    }

    return false;
}

static void turbowasm_wasi_threads_release_slot(
    turbowasm_wasi_thread_slot *slot) {
    turbowasm_wasi_threads_impl *owner;

    if (slot == NULL || slot->owner == NULL)
        return;

    owner = slot->owner;
    turbowasm_instance_destroy(&slot->child);

    salts_mutex_lock(&owner->mutex);
    slot->active = false;
    slot->start_arg = 0u;
    slot->tid = 0;
    if (owner->active != 0u)
        --owner->active;
    salts_mutex_unlock(&owner->mutex);
}

static void turbowasm_wasi_threads_run(void *user) {
    turbowasm_wasi_thread_slot *slot =
        (turbowasm_wasi_thread_slot *)user;
    turbowasm_value arguments[2] = {{0}};
    turbowasm_execution_options options = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_status status;

    if (slot == NULL || slot->owner == NULL || !slot->active)
        return;

    arguments[0].kind = TURBOWASM_VALUE_I32;
    arguments[0].as.i32 = slot->tid;
    arguments[1].kind = TURBOWASM_VALUE_I32;
    arguments[1].as.i32 = (int32_t)slot->start_arg;

    options.should_interrupt =
        turbowasm_wasi_threads_should_interrupt;
    options.interrupt_context = slot->owner;

    status = turbowasm_instance_invoke_with_options(
        &slot->child,
        slot->start_function_index,
        arguments, 2u,
        NULL, 0u,
        &result_count, &trap,
        &options);

    if (status == TURBOWASM_TRAPPED) {
        turbowasm_wasi_threads_publish_fatal(
            slot->owner, &slot->child, status, trap);
    }
}

static void turbowasm_wasi_threads_cancel(void *user) {
    (void)user;
}

static void turbowasm_wasi_threads_finalize(void *user) {
    turbowasm_wasi_threads_release_slot(
        (turbowasm_wasi_thread_slot *)user);
}

static int32_t turbowasm_wasi_threads_reserve(
    turbowasm_wasi_threads_impl *impl,
    size_t *out_slot_index) {
    size_t index;
    int32_t result;

    if (impl == NULL || out_slot_index == NULL)
        return TURBOWASM_WASI_THREADS_SPAWN_BAD_MODULE;

    salts_mutex_lock(&impl->mutex);
    if (atomic_load_explicit(
            &impl->fatal, memory_order_acquire)) {
        salts_mutex_unlock(&impl->mutex);
        return TURBOWASM_WASI_THREADS_SPAWN_GROUP_TERMINATED;
    }
    if (impl->active >= impl->capacity) {
        salts_mutex_unlock(&impl->mutex);
        return TURBOWASM_WASI_THREADS_SPAWN_CAPACITY;
    }
    if (impl->next_tid >= TURBOWASM_WASI_THREADS_TID_LIMIT) {
        salts_mutex_unlock(&impl->mutex);
        return TURBOWASM_WASI_THREADS_SPAWN_TID_EXHAUSTED;
    }

    for (index = 0u; index < impl->capacity; ++index) {
        if (!impl->slots[index].active)
            break;
    }
    if (index == impl->capacity) {
        salts_mutex_unlock(&impl->mutex);
        return TURBOWASM_WASI_THREADS_SPAWN_CAPACITY;
    }

    result = (int32_t)impl->next_tid++;
    impl->slots[index].active = true;
    impl->slots[index].tid = result;
    ++impl->active;
    *out_slot_index = index;
    salts_mutex_unlock(&impl->mutex);
    return result;
}

static void turbowasm_wasi_threads_rollback(
    turbowasm_wasi_thread_slot *slot) {
    if (slot == NULL || slot->owner == NULL)
        return;

    turbowasm_instance_destroy(&slot->child);
    salts_mutex_lock(&slot->owner->mutex);
    if (slot->active) {
        slot->active = false;
        if (slot->owner->active != 0u)
            --slot->owner->active;
    }
    slot->start_arg = 0u;
    slot->tid = 0;
    salts_mutex_unlock(&slot->owner->mutex);
}

static turbowasm_status turbowasm_wasi_threads_spawn(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    turbowasm_wasi_threads_impl *impl =
        (turbowasm_wasi_threads_impl *)context;
    turbowasm_instance *caller;
    const turbowasm_module *module;
    uint32_t start_function_index = 0u;
    size_t slot_index = 0u;
    int32_t spawn_result;
    turbowasm_wasi_thread_slot *slot;
    cflow_executor_task task = {0};
    cflow_admission_status admission;

    if (impl == NULL || call == NULL ||
        arguments == NULL || argument_count != 1u ||
        arguments[0].kind != TURBOWASM_VALUE_I32 ||
        results == NULL || result_capacity < 1u ||
        result_count == NULL || trap == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    caller = turbowasm_host_call_instance(call);
    module = turbowasm_instance_module(caller);
    if (caller == NULL || module == NULL ||
        !turbowasm_wasi_threads_validate_module(
            module, &start_function_index)) {
        spawn_result = TURBOWASM_WASI_THREADS_SPAWN_BAD_MODULE;
        goto done;
    }

    salts_mutex_lock(&impl->mutex);
    if (impl->module == NULL) {
        impl->module = module;
        impl->start_function_index = start_function_index;
    } else if (impl->module != module ||
               impl->start_function_index != start_function_index) {
        salts_mutex_unlock(&impl->mutex);
        spawn_result = TURBOWASM_WASI_THREADS_SPAWN_BAD_MODULE;
        goto done;
    }
    salts_mutex_unlock(&impl->mutex);

    spawn_result = turbowasm_wasi_threads_reserve(
        impl, &slot_index);
    if (spawn_result < 0)
        goto done;

    slot = &impl->slots[slot_index];
    slot->start_function_index = start_function_index;
    slot->start_arg = (uint32_t)arguments[0].as.i32;

    if (turbowasm_instance_create_sibling_internal(
            &slot->child, caller) != TURBOWASM_OK) {
        turbowasm_wasi_threads_rollback(slot);
        spawn_result = TURBOWASM_WASI_THREADS_SPAWN_INSTANTIATE;
        goto done;
    }

    task.run = turbowasm_wasi_threads_run;
    task.cancel = turbowasm_wasi_threads_cancel;
    task.finalize = turbowasm_wasi_threads_finalize;
    task.user = slot;

    admission = cflow_executor_try_post_task(
        impl->executor, &task);
    if (admission != CFLOW_ADMISSION_ACCEPTED) {
        turbowasm_wasi_threads_rollback(slot);
        spawn_result =
            admission == CFLOW_ADMISSION_FULL
                ? TURBOWASM_WASI_THREADS_SPAWN_CAPACITY
                : TURBOWASM_WASI_THREADS_SPAWN_EXECUTOR;
    }

done:
    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = spawn_result;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

bool turbowasm_wasi_threads_execution_policy_init(
    turbowasm_wasi_threads_execution_policy *policy,
    turbowasm_wasi_threads *threads) {
    if (policy == NULL || threads == NULL ||
        threads->impl == NULL)
        return false;

    policy->threads = threads;
    policy->chained_interrupt = NULL;
    policy->chained_context = NULL;
    return true;
}

bool turbowasm_wasi_threads_execution_policy_apply(
    turbowasm_wasi_threads_execution_policy *policy,
    turbowasm_execution_options *options) {
    if (policy == NULL || policy->threads == NULL ||
        policy->threads->impl == NULL || options == NULL)
        return false;

    if (options->should_interrupt !=
            turbowasm_wasi_threads_policy_should_interrupt ||
        options->interrupt_context != policy) {
        policy->chained_interrupt = options->should_interrupt;
        policy->chained_context = options->interrupt_context;
    }

    options->should_interrupt =
        turbowasm_wasi_threads_policy_should_interrupt;
    options->interrupt_context = policy;
    return true;
}

turbowasm_status turbowasm_wasi_threads_init(
    turbowasm_wasi_threads *threads,
    const turbowasm_wasi_threads_config *config) {
    turbowasm_wasi_threads_impl *impl;
    size_t index;

    if (threads == NULL || threads->impl != NULL ||
        config == NULL || config->executor == NULL ||
        config->capacity == 0u ||
        !cflow_executor_has(
            config->executor, CMETA_EXEC_CAP_CONCURRENT))
        return TURBOWASM_INVALID_ARGUMENT;

    if (config->capacity >
        SIZE_MAX / sizeof(turbowasm_wasi_thread_slot))
        return TURBOWASM_OUT_OF_MEMORY;

    impl = (turbowasm_wasi_threads_impl *)calloc(
        1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->slots = (turbowasm_wasi_thread_slot *)calloc(
        config->capacity, sizeof(*impl->slots));
    if (impl->slots == NULL) {
        free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    salts_mutex_init(&impl->mutex);
    if (impl->mutex == NULL) {
        free(impl->slots);
        free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }
    impl->mutex_initialized = true;
    atomic_init(&impl->fatal, false);
    impl->fatal_status = TURBOWASM_OK;
    impl->fatal_trap = TURBOWASM_TRAP_NONE;
    impl->executor = config->executor;
    impl->capacity = config->capacity;
    impl->next_tid = 1u;

    for (index = 0u; index < impl->capacity; ++index)
        impl->slots[index].owner = impl;

    threads->impl = impl;
    return TURBOWASM_OK;
}

bool turbowasm_wasi_threads_destroy(
    turbowasm_wasi_threads *threads) {
    turbowasm_wasi_threads_impl *impl;

    if (threads == NULL || threads->impl == NULL)
        return false;

    impl = (turbowasm_wasi_threads_impl *)threads->impl;
    salts_mutex_lock(&impl->mutex);
    if (impl->active != 0u) {
        salts_mutex_unlock(&impl->mutex);
        return false;
    }
    salts_mutex_unlock(&impl->mutex);

    if (impl->mutex_initialized)
        salts_mutex_destroy(&impl->mutex);
    free(impl->slots);
    free(impl);
    threads->impl = NULL;
    return true;
}

turbowasm_status turbowasm_wasi_threads_define(
    turbowasm_wasi_threads *threads,
    turbowasm_linker *linker) {
    static const turbowasm_value_kind params[] = {
        TURBOWASM_VALUE_I32
    };
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type type = {
        params, 1u, results, 1u
    };

    if (threads == NULL || threads->impl == NULL ||
        linker == NULL || linker->impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_linker_define_host_function(
        linker,
        turbowasm_wasi_threads_name("wasi"),
        turbowasm_wasi_threads_name("thread-spawn"),
        &type,
        turbowasm_wasi_threads_spawn,
        threads->impl);
}

size_t turbowasm_wasi_threads_active(
    const turbowasm_wasi_threads *threads) {
    turbowasm_wasi_threads_impl *impl;
    size_t active;

    if (threads == NULL || threads->impl == NULL)
        return 0u;

    impl = (turbowasm_wasi_threads_impl *)threads->impl;
    salts_mutex_lock(&impl->mutex);
    active = impl->active;
    salts_mutex_unlock(&impl->mutex);
    return active;
}

bool turbowasm_wasi_threads_group_fatal(
    const turbowasm_wasi_threads *threads,
    turbowasm_status *out_status,
    turbowasm_trap *out_trap) {
    turbowasm_wasi_threads_impl *impl;

    if (threads == NULL || threads->impl == NULL)
        return false;

    impl = (turbowasm_wasi_threads_impl *)threads->impl;
    if (!atomic_load_explicit(
            &impl->fatal, memory_order_acquire))
        return false;

    salts_mutex_lock(&impl->mutex);
    if (out_status != NULL)
        *out_status = impl->fatal_status;
    if (out_trap != NULL)
        *out_trap = impl->fatal_trap;
    salts_mutex_unlock(&impl->mutex);
    return true;
}
