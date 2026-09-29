#include <turbowasm/native_io.h>

#include <salts/error_codes.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbowasm_native_io_slot {
    bool active;
    bool terminal;
    bool abandoned;

    turbowasm_host_call *call;
    turbowasm_host_wait wait;
    native_io_request request;
    native_io_completion *out_completion;
    uintptr_t original_user_data;
} turbowasm_native_io_slot;

typedef struct turbowasm_native_io_bridge_impl {
    native_io_backend *backend;
    turbowasm_native_io_slot *slots;
    uint32_t *free_indices;
    uint32_t capacity;
    uint32_t free_count;
} turbowasm_native_io_bridge_impl;

static turbowasm_native_io_bridge_impl *
turbowasm_native_io_impl_mut(
    turbowasm_native_io_bridge *bridge) {
    return bridge == NULL
        ? NULL
        : (turbowasm_native_io_bridge_impl *)bridge->impl;
}

static const turbowasm_native_io_bridge_impl *
turbowasm_native_io_impl_get(
    const turbowasm_native_io_bridge *bridge) {
    return bridge == NULL
        ? NULL
        : (const turbowasm_native_io_bridge_impl *)bridge->impl;
}

static bool turbowasm_native_io_request_equal(
    native_io_request left,
    native_io_request right) {
    return left.slot == right.slot &&
           left.generation == right.generation;
}

static turbowasm_native_io_slot *
turbowasm_native_io_reserve(
    turbowasm_native_io_bridge_impl *impl) {
    uint32_t index;
    turbowasm_native_io_slot *slot;

    if (impl == NULL || impl->free_count == 0u)
        return NULL;

    index = impl->free_indices[--impl->free_count];
    slot = &impl->slots[index];
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    return slot;
}

static void turbowasm_native_io_release(
    turbowasm_native_io_bridge_impl *impl,
    turbowasm_native_io_slot *slot) {
    uint32_t index;

    if (impl == NULL || slot == NULL || !slot->active)
        return;

    index = (uint32_t)(slot - impl->slots);
    memset(slot, 0, sizeof(*slot));
    impl->free_indices[impl->free_count++] = index;
}

static turbowasm_native_io_slot *
turbowasm_native_io_slot_from_token(
    turbowasm_native_io_bridge_impl *impl,
    uintptr_t token) {
    uintptr_t base;
    uintptr_t address;
    uintptr_t offset;
    size_t index;

    if (impl == NULL || impl->slots == NULL ||
        impl->capacity == 0u || token == 0u)
        return NULL;

    base = (uintptr_t)impl->slots;
    address = token;
    if (address < base)
        return NULL;

    offset = address - base;
    if (offset % sizeof(*impl->slots) != 0u)
        return NULL;

    index = (size_t)(offset / sizeof(*impl->slots));
    if (index >= impl->capacity)
        return NULL;

    return &impl->slots[index];
}

static int turbowasm_native_io_map_runtime_status(
    turbowasm_status status) {
    switch (status) {
        case TURBOWASM_OK:
            return SALTS_OK;
        case TURBOWASM_UNSUPPORTED:
            return SALTS_ENOTSUP;
        case TURBOWASM_OUT_OF_MEMORY:
            return SALTS_ENOMEM;
        case TURBOWASM_INVALID_ARGUMENT:
            return SALTS_EINVAL;
        default:
            return SALTS_EPROTO;
    }
}

int turbowasm_native_io_bridge_init(
    turbowasm_native_io_bridge *bridge,
    native_io_backend *backend,
    size_t capacity) {
    turbowasm_native_io_bridge_impl *impl;
    uint32_t index;

    if (bridge == NULL || bridge->impl != NULL ||
        backend == NULL || backend->impl == NULL ||
        capacity == 0u || capacity > UINT32_MAX ||
        capacity > SIZE_MAX / sizeof(turbowasm_native_io_slot) ||
        capacity > SIZE_MAX / sizeof(uint32_t))
        return SALTS_EINVAL;

    impl = (turbowasm_native_io_bridge_impl *)calloc(
        1u, sizeof(*impl));
    if (impl == NULL)
        return SALTS_ENOMEM;

    impl->slots = (turbowasm_native_io_slot *)calloc(
        capacity, sizeof(*impl->slots));
    impl->free_indices = (uint32_t *)malloc(
        capacity * sizeof(*impl->free_indices));
    if (impl->slots == NULL || impl->free_indices == NULL) {
        free(impl->free_indices);
        free(impl->slots);
        free(impl);
        return SALTS_ENOMEM;
    }

    impl->backend = backend;
    impl->capacity = (uint32_t)capacity;
    impl->free_count = (uint32_t)capacity;
    for (index = 0u; index < impl->capacity; ++index)
        impl->free_indices[index] =
            impl->capacity - 1u - index;

    bridge->impl = impl;
    return SALTS_OK;
}

int turbowasm_native_io_bridge_destroy(
    turbowasm_native_io_bridge *bridge) {
    turbowasm_native_io_bridge_impl *impl =
        turbowasm_native_io_impl_mut(bridge);

    if (bridge == NULL)
        return SALTS_EINVAL;
    if (impl == NULL)
        return SALTS_OK;
    if (impl->free_count != impl->capacity)
        return SALTS_EBUSY;

    free(impl->free_indices);
    free(impl->slots);
    free(impl);
    bridge->impl = NULL;
    return SALTS_OK;
}

int turbowasm_native_io_await(
    turbowasm_native_io_bridge *bridge,
    turbowasm_host_call *call,
    const native_io_operation *operation,
    native_io_completion *out_completion) {
    turbowasm_native_io_bridge_impl *impl =
        turbowasm_native_io_impl_mut(bridge);
    turbowasm_native_io_slot *slot;
    native_io_operation submitted;
    native_io_request request = {0};
    int bridge_status = SALTS_OK;
    int status;
    turbowasm_status runtime_status;

    if (impl == NULL || call == NULL || operation == NULL ||
        out_completion == NULL)
        return SALTS_EINVAL;
    if (!turbowasm_host_call_can_wait(call))
        return SALTS_ENOTSUP;

    slot = turbowasm_native_io_reserve(impl);
    if (slot == NULL)
        return SALTS_ENOBUFS;

    submitted = *operation;
    slot->call = call;
    slot->out_completion = out_completion;
    slot->original_user_data = operation->user_data;
    submitted.user_data = (uintptr_t)slot;
    memset(out_completion, 0, sizeof(*out_completion));

    status = native_io_backend_submit(
        impl->backend, &submitted, &request);
    if (status != SALTS_OK) {
        turbowasm_native_io_release(impl, slot);
        return status;
    }

    slot->request = request;
    runtime_status = turbowasm_host_call_wait(
        call,
        (uintptr_t)slot,
        &slot->wait,
        &bridge_status);

    if (runtime_status != TURBOWASM_OK) {
        /*
         * NativeIO already owns the request. Keep the route alive until its
         * terminal completion is observed, but do not dereference the host
         * call after this callback unwinds.
         */
        slot->call = NULL;
        slot->out_completion = NULL;
        slot->abandoned = true;
        return turbowasm_native_io_map_runtime_status(runtime_status);
    }

    if (!slot->terminal || bridge_status != SALTS_OK) {
        slot->call = NULL;
        slot->out_completion = NULL;
        slot->abandoned = true;
        return SALTS_EPROTO;
    }

    turbowasm_native_io_release(impl, slot);
    return SALTS_OK;
}

int turbowasm_native_io_bridge_complete(
    turbowasm_native_io_bridge *bridge,
    const native_io_completion *completion) {
    turbowasm_native_io_bridge_impl *impl =
        turbowasm_native_io_impl_mut(bridge);
    turbowasm_native_io_slot *slot;
    turbowasm_status runtime_status;

    if (impl == NULL || completion == NULL)
        return SALTS_EINVAL;

    slot = turbowasm_native_io_slot_from_token(
        impl, completion->user_data);
    if (slot == NULL || !slot->active)
        return SALTS_ENOENT;
    if (slot->terminal)
        return SALTS_EALREADY;
    if (!turbowasm_native_io_request_equal(
            slot->request, completion->request))
        return SALTS_ENOENT;

    if (slot->abandoned) {
        slot->terminal = true;
        turbowasm_native_io_release(impl, slot);
        return SALTS_OK;
    }

    if (slot->call == NULL ||
        slot->out_completion == NULL ||
        slot->wait.generation == 0u)
        return SALTS_EPROTO;

    *slot->out_completion = *completion;
    slot->out_completion->user_data =
        slot->original_user_data;
    slot->terminal = true;

    runtime_status = turbowasm_host_call_complete_wait(
        slot->call, slot->wait, SALTS_OK);
    if (runtime_status != TURBOWASM_OK)
        return turbowasm_native_io_map_runtime_status(
            runtime_status);

    return SALTS_OK;
}

bool turbowasm_native_io_pending_request(
    const turbowasm_native_io_bridge *bridge,
    const turbowasm_execution *execution,
    native_io_request *out_request) {
    const turbowasm_native_io_bridge_impl *impl =
        turbowasm_native_io_impl_get(bridge);
    turbowasm_native_io_slot *slot;
    turbowasm_host_wait wait = {0};

    if (impl == NULL || execution == NULL ||
        out_request == NULL ||
        !turbowasm_execution_pending_host_wait(
            execution, &wait))
        return false;

    slot = turbowasm_native_io_slot_from_token(
        (turbowasm_native_io_bridge_impl *)impl,
        wait.operation_token);
    if (slot == NULL || !slot->active ||
        slot->wait.generation != wait.generation ||
        slot->terminal)
        return false;

    *out_request = slot->request;
    return true;
}

int turbowasm_native_io_cancel_execution(
    turbowasm_native_io_bridge *bridge,
    const turbowasm_execution *execution) {
    turbowasm_native_io_bridge_impl *impl =
        turbowasm_native_io_impl_mut(bridge);
    native_io_request request = {0};

    if (impl == NULL || execution == NULL)
        return SALTS_EINVAL;
    if (!turbowasm_native_io_pending_request(
            bridge, execution, &request))
        return SALTS_ENOENT;

    return native_io_backend_cancel(
        impl->backend, request);
}
