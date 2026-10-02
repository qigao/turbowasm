#include "wasi02_poll_native_io.h"

#include "runtime_alloc.h"

#include <salts/error_codes.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct turbowasm_wasi02_native_io_poll_slot {
    bool active;
    bool terminal;
    bool abandoned;
    uint32_t generation;
    native_io_request request;
} turbowasm_wasi02_native_io_poll_slot;

typedef struct turbowasm_wasi02_native_io_route {
    bool active;
    turbowasm_host_call *call;
    turbowasm_host_wait wait;
    uint64_t *members;
    size_t member_count;
} turbowasm_wasi02_native_io_route;

typedef struct turbowasm_wasi02_native_io_poll_impl {
    native_io_backend *backend;
    turbowasm_wasi02_native_io_cancel_fn cancel_fn;

    turbowasm_wasi02_native_io_poll_slot *pollables;
    uint32_t *pollable_free;
    uint32_t pollable_capacity;
    uint32_t pollable_free_count;

    turbowasm_wasi02_native_io_route *routes;
    uint32_t *route_free;
    uint64_t *route_members;
    uint32_t route_capacity;
    uint32_t route_free_count;
    size_t max_members_per_route;
} turbowasm_wasi02_native_io_poll_impl;

static turbowasm_wasi02_native_io_poll_impl *impl_mut(
    turbowasm_wasi02_native_io_poll *adapter) {
    return adapter == NULL
        ? NULL
        : (turbowasm_wasi02_native_io_poll_impl *)adapter->impl;
}

static bool request_equal(
    native_io_request left,
    native_io_request right) {
    return left.slot == right.slot &&
           left.generation == right.generation;
}

static bool request_has_other_live_consumer(
    const turbowasm_wasi02_native_io_poll_impl *impl,
    const turbowasm_wasi02_native_io_poll_slot *self) {
    uint32_t i;

    if (impl == NULL || self == NULL)
        return false;
    for (i = 0u; i < impl->pollable_capacity; ++i) {
        const turbowasm_wasi02_native_io_poll_slot *slot =
            &impl->pollables[i];
        if (slot == self || !slot->active ||
            slot->terminal || slot->abandoned)
            continue;
        if (request_equal(slot->request, self->request))
            return true;
    }
    return false;
}

static turbowasm_status map_native_status(int status) {
    if (status == SALTS_OK)
        return TURBOWASM_OK;
    if (status == SALTS_ENOMEM ||
        status == SALTS_ENOBUFS)
        return TURBOWASM_OUT_OF_MEMORY;
    if (status == SALTS_ENOTSUP)
        return TURBOWASM_UNSUPPORTED;
    if (status == SALTS_EINVAL)
        return TURBOWASM_INVALID_ARGUMENT;
    return TURBOWASM_TRAPPED;
}

static uint64_t pack_pollable(
    uint32_t index,
    uint32_t generation) {
    return ((uint64_t)generation << 32u) |
           (uint64_t)index;
}

static bool unpack_pollable(
    turbowasm_value rep,
    uint32_t *out_index,
    uint32_t *out_generation) {
    uint64_t packed;

    if (out_index == NULL || out_generation == NULL ||
        rep.kind != TURBOWASM_VALUE_I64)
        return false;

    packed = (uint64_t)rep.as.i64;
    *out_index = (uint32_t)packed;
    *out_generation = (uint32_t)(packed >> 32u);
    return *out_generation != 0u;
}

static turbowasm_wasi02_native_io_poll_slot *slot_from_rep(
    turbowasm_wasi02_native_io_poll_impl *impl,
    turbowasm_value rep) {
    uint32_t index;
    uint32_t generation;
    turbowasm_wasi02_native_io_poll_slot *slot;

    if (impl == NULL ||
        !unpack_pollable(rep, &index, &generation) ||
        index >= impl->pollable_capacity)
        return NULL;

    slot = &impl->pollables[index];
    if (!slot->active ||
        slot->generation != generation)
        return NULL;
    return slot;
}

static uint64_t slot_handle(
    const turbowasm_wasi02_native_io_poll_impl *impl,
    const turbowasm_wasi02_native_io_poll_slot *slot) {
    uint32_t index;

    if (impl == NULL || slot == NULL ||
        slot < impl->pollables ||
        slot >= impl->pollables + impl->pollable_capacity)
        return 0u;
    index = (uint32_t)(slot - impl->pollables);
    return pack_pollable(index, slot->generation);
}

static turbowasm_wasi02_native_io_poll_slot *reserve_slot(
    turbowasm_wasi02_native_io_poll_impl *impl) {
    uint32_t index;
    uint32_t generation;
    turbowasm_wasi02_native_io_poll_slot *slot;

    if (impl == NULL || impl->pollable_free_count == 0u)
        return NULL;

    index =
        impl->pollable_free[--impl->pollable_free_count];
    slot = &impl->pollables[index];
    generation = slot->generation + 1u;
    if (generation == 0u)
        generation = 1u;

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->generation = generation;
    return slot;
}

static void release_slot(
    turbowasm_wasi02_native_io_poll_impl *impl,
    turbowasm_wasi02_native_io_poll_slot *slot) {
    uint32_t index;
    uint32_t generation;

    if (impl == NULL || slot == NULL || !slot->active)
        return;

    index = (uint32_t)(slot - impl->pollables);
    generation = slot->generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
    impl->pollable_free[impl->pollable_free_count++] = index;
}

static turbowasm_wasi02_native_io_route *reserve_route(
    turbowasm_wasi02_native_io_poll_impl *impl,
    turbowasm_host_call *call) {
    uint32_t index;
    turbowasm_wasi02_native_io_route *route;
    uint64_t *members;

    if (impl == NULL || call == NULL ||
        impl->route_free_count == 0u)
        return NULL;

    index = impl->route_free[--impl->route_free_count];
    route = &impl->routes[index];
    members =
        &impl->route_members[
            (size_t)index * impl->max_members_per_route];

    memset(route, 0, sizeof(*route));
    route->active = true;
    route->call = call;
    route->members = members;
    return route;
}

static void release_route(
    turbowasm_wasi02_native_io_poll_impl *impl,
    turbowasm_wasi02_native_io_route *route) {
    uint32_t index;
    uint64_t *members;

    if (impl == NULL || route == NULL || !route->active)
        return;

    index = (uint32_t)(route - impl->routes);
    members =
        &impl->route_members[
            (size_t)index * impl->max_members_per_route];
    memset(
        members, 0,
        impl->max_members_per_route * sizeof(*members));
    memset(route, 0, sizeof(*route));
    route->members = members;
    impl->route_free[impl->route_free_count++] = index;
}

static turbowasm_wasi02_native_io_route *route_from_token(
    turbowasm_wasi02_native_io_poll_impl *impl,
    uintptr_t token) {
    uintptr_t base;
    uintptr_t address;
    uintptr_t offset;
    size_t index;

    if (impl == NULL || impl->routes == NULL ||
        impl->route_capacity == 0u || token == 0u)
        return NULL;

    base = (uintptr_t)impl->routes;
    address = token;
    if (address < base)
        return NULL;

    offset = address - base;
    if (offset % sizeof(*impl->routes) != 0u)
        return NULL;
    index = (size_t)(offset / sizeof(*impl->routes));
    if (index >= impl->route_capacity)
        return NULL;
    return &impl->routes[index];
}

static bool route_contains(
    const turbowasm_wasi02_native_io_route *route,
    uint64_t pollable) {
    size_t i;

    if (route == NULL || !route->active)
        return false;
    for (i = 0u; i < route->member_count; ++i) {
        if (route->members[i] == pollable)
            return true;
    }
    return false;
}

static turbowasm_status provider_ready(
    void *context,
    turbowasm_value rep,
    bool *out_ready) {
    turbowasm_wasi02_native_io_poll_impl *impl =
        (turbowasm_wasi02_native_io_poll_impl *)context;
    turbowasm_wasi02_native_io_poll_slot *slot;

    if (out_ready == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    slot = slot_from_rep(impl, rep);
    if (slot == NULL || slot->abandoned)
        return TURBOWASM_TRAPPED;

    *out_ready = slot->terminal;
    return TURBOWASM_OK;
}

static turbowasm_status arm_route(
    turbowasm_wasi02_native_io_poll_impl *impl,
    const turbowasm_value *reps,
    size_t rep_count,
    turbowasm_host_call *call,
    uintptr_t *out_operation_token,
    turbowasm_host_wait **out_wait_storage) {
    turbowasm_wasi02_native_io_route *route;
    size_t i;

    if (impl == NULL || reps == NULL ||
        rep_count == 0u ||
        rep_count > impl->max_members_per_route ||
        call == NULL ||
        out_operation_token == NULL ||
        out_wait_storage == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    route = reserve_route(impl, call);
    if (route == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    for (i = 0u; i < rep_count; ++i) {
        turbowasm_wasi02_native_io_poll_slot *slot =
            slot_from_rep(impl, reps[i]);
        uint64_t handle;

        if (slot == NULL || slot->abandoned || slot->terminal) {
            release_route(impl, route);
            return TURBOWASM_TRAPPED;
        }

        handle = slot_handle(impl, slot);
        if (handle == 0u) {
            release_route(impl, route);
            return TURBOWASM_TRAPPED;
        }
        route->members[i] = handle;
    }

    route->member_count = rep_count;
    *out_operation_token = (uintptr_t)route;
    *out_wait_storage = &route->wait;
    return TURBOWASM_OK;
}

static turbowasm_status provider_arm_routed(
    void *context,
    turbowasm_value rep,
    turbowasm_host_call *call,
    uintptr_t *out_operation_token,
    turbowasm_host_wait **out_wait_storage) {
    return arm_route(
        (turbowasm_wasi02_native_io_poll_impl *)context,
        &rep, 1u, call,
        out_operation_token, out_wait_storage);
}

static turbowasm_status provider_arm_many_routed(
    void *context,
    const turbowasm_value *reps,
    size_t rep_count,
    turbowasm_host_call *call,
    uintptr_t *out_operation_token,
    turbowasm_host_wait **out_wait_storage) {
    return arm_route(
        (turbowasm_wasi02_native_io_poll_impl *)context,
        reps, rep_count, call,
        out_operation_token, out_wait_storage);
}

static turbowasm_status provider_drop(
    void *context,
    turbowasm_value rep) {
    turbowasm_wasi02_native_io_poll_impl *impl =
        (turbowasm_wasi02_native_io_poll_impl *)context;
    turbowasm_wasi02_native_io_poll_slot *slot;
    int status;

    slot = slot_from_rep(impl, rep);
    if (slot == NULL)
        return TURBOWASM_TRAPPED;

    if (slot->terminal) {
        release_slot(impl, slot);
        return TURBOWASM_OK;
    }
    if (slot->abandoned)
        return TURBOWASM_OK;

    /*
     * Multiple WASI pollables may alias one NativeIO request. Consuming one
     * logical alias must not cancel shared work while another live alias still
     * observes the same generation-safe request.
     */
    slot->abandoned = true;
    if (request_has_other_live_consumer(impl, slot))
        return TURBOWASM_OK;

    status = impl->cancel_fn(
        impl->backend, slot->request);
    if (status != SALTS_OK &&
        status != SALTS_EALREADY) {
        slot->abandoned = false;
        return map_native_status(status);
    }

    /*
     * NativeIO cancellation is not terminal. All abandoned aliases remain
     * retained until the authoritative terminal packet is observed.
     */
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_native_io_poll_init(
    turbowasm_wasi02_native_io_poll *adapter,
    native_io_backend *backend,
    size_t pollable_capacity,
    size_t route_capacity,
    size_t max_members_per_route,
    turbowasm_wasi02_native_io_cancel_fn cancel_fn) {
    turbowasm_wasi02_native_io_poll_impl *impl;
    size_t route_member_count;
    uint32_t i;

    if (adapter == NULL || adapter->impl != NULL ||
        backend == NULL || backend->impl == NULL ||
        pollable_capacity == 0u ||
        pollable_capacity > UINT32_MAX ||
        route_capacity == 0u ||
        route_capacity > UINT32_MAX ||
        max_members_per_route == 0u ||
        max_members_per_route > UINT32_MAX ||
        route_capacity >
            SIZE_MAX / max_members_per_route)
        return TURBOWASM_INVALID_ARGUMENT;

    route_member_count =
        route_capacity * max_members_per_route;
    if (pollable_capacity >
            SIZE_MAX / sizeof(*impl->pollables) ||
        pollable_capacity >
            SIZE_MAX / sizeof(*impl->pollable_free) ||
        route_capacity >
            SIZE_MAX / sizeof(*impl->routes) ||
        route_capacity >
            SIZE_MAX / sizeof(*impl->route_free) ||
        route_member_count >
            SIZE_MAX / sizeof(*impl->route_members))
        return TURBOWASM_INVALID_ARGUMENT;

    impl = (turbowasm_wasi02_native_io_poll_impl *)
        turbowasm_rt_calloc(1u, sizeof(*impl));
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->pollables =
        (turbowasm_wasi02_native_io_poll_slot *)
            turbowasm_rt_calloc(
                pollable_capacity,
                sizeof(*impl->pollables));
    impl->pollable_free = (uint32_t *)turbowasm_rt_malloc(
        pollable_capacity * sizeof(*impl->pollable_free));
    impl->routes =
        (turbowasm_wasi02_native_io_route *)
            turbowasm_rt_calloc(
                route_capacity,
                sizeof(*impl->routes));
    impl->route_free = (uint32_t *)turbowasm_rt_malloc(
        route_capacity * sizeof(*impl->route_free));
    impl->route_members = (uint64_t *)turbowasm_rt_calloc(
        route_member_count,
        sizeof(*impl->route_members));

    if (impl->pollables == NULL ||
        impl->pollable_free == NULL ||
        impl->routes == NULL ||
        impl->route_free == NULL ||
        impl->route_members == NULL) {
        turbowasm_rt_free(impl->route_members);
        turbowasm_rt_free(impl->route_free);
        turbowasm_rt_free(impl->routes);
        turbowasm_rt_free(impl->pollable_free);
        turbowasm_rt_free(impl->pollables);
        turbowasm_rt_free(impl);
        return TURBOWASM_OUT_OF_MEMORY;
    }

    impl->backend = backend;
    impl->cancel_fn =
        cancel_fn != NULL
            ? cancel_fn
            : native_io_backend_cancel;
    impl->pollable_capacity = (uint32_t)pollable_capacity;
    impl->pollable_free_count =
        (uint32_t)pollable_capacity;
    impl->route_capacity = (uint32_t)route_capacity;
    impl->route_free_count =
        (uint32_t)route_capacity;
    impl->max_members_per_route =
        max_members_per_route;

    for (i = 0u; i < impl->pollable_capacity; ++i)
        impl->pollable_free[i] =
            impl->pollable_capacity - 1u - i;
    for (i = 0u; i < impl->route_capacity; ++i) {
        impl->route_free[i] =
            impl->route_capacity - 1u - i;
        impl->routes[i].members =
            &impl->route_members[
                (size_t)i * max_members_per_route];
    }

    adapter->impl = impl;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_native_io_poll_destroy(
    turbowasm_wasi02_native_io_poll *adapter) {
    turbowasm_wasi02_native_io_poll_impl *impl =
        impl_mut(adapter);

    if (adapter == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl == NULL)
        return TURBOWASM_OK;
    if (impl->pollable_free_count !=
            impl->pollable_capacity ||
        impl->route_free_count !=
            impl->route_capacity)
        return TURBOWASM_INVALID_ARGUMENT;

    turbowasm_rt_free(impl->route_members);
    turbowasm_rt_free(impl->route_free);
    turbowasm_rt_free(impl->routes);
    turbowasm_rt_free(impl->pollable_free);
    turbowasm_rt_free(impl->pollables);
    turbowasm_rt_free(impl);
    adapter->impl = NULL;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_native_io_poll_register_request(
    turbowasm_wasi02_native_io_poll *adapter,
    native_io_request request,
    turbowasm_value *out_provider_rep) {
    turbowasm_wasi02_native_io_poll_impl *impl =
        impl_mut(adapter);
    turbowasm_wasi02_native_io_poll_slot *slot;
    uint64_t packed;

    if (impl == NULL || out_provider_rep == NULL ||
        !native_io_request_valid(request))
        return TURBOWASM_INVALID_ARGUMENT;

    slot = reserve_slot(impl);
    if (slot == NULL)
        return TURBOWASM_OUT_OF_MEMORY;
    slot->request = request;

    packed = slot_handle(impl, slot);
    if (packed == 0u) {
        release_slot(impl, slot);
        return TURBOWASM_TRAPPED;
    }

    memset(out_provider_rep, 0, sizeof(*out_provider_rep));
    out_provider_rep->kind = TURBOWASM_VALUE_I64;
    out_provider_rep->as.i64 = (int64_t)packed;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_native_io_poll_provider(
    turbowasm_wasi02_native_io_poll *adapter,
    turbowasm_wasi02_poll_provider *out_provider) {
    turbowasm_wasi02_native_io_poll_impl *impl =
        impl_mut(adapter);

    if (impl == NULL || out_provider == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out_provider, 0, sizeof(*out_provider));
    out_provider->context = impl;
    out_provider->ready = provider_ready;
    out_provider->arm_routed = provider_arm_routed;
    out_provider->arm_many_routed =
        provider_arm_many_routed;
    out_provider->drop = provider_drop;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_native_io_poll_complete(
    turbowasm_wasi02_native_io_poll *adapter,
    const native_io_completion *completion) {
    turbowasm_wasi02_native_io_poll_impl *impl =
        impl_mut(adapter);
    uint32_t i;
    uint32_t matched = 0u;
    turbowasm_status final_status = TURBOWASM_OK;

    if (impl == NULL || completion == NULL ||
        !native_io_request_valid(completion->request))
        return TURBOWASM_INVALID_ARGUMENT;

    /*
     * Mark every logical alias terminal first. Routes are completed only
     * after the full alias set is visible as ready.
     */
    for (i = 0u; i < impl->pollable_capacity; ++i) {
        turbowasm_wasi02_native_io_poll_slot *slot =
            &impl->pollables[i];
        if (!slot->active ||
            !request_equal(slot->request, completion->request))
            continue;
        if (slot->terminal)
            continue;
        slot->terminal = true;
        ++matched;
    }
    if (matched == 0u)
        return TURBOWASM_TRAPPED;

    /*
     * One route may contain more than one alias of the same request. Complete
     * that host wait exactly once when any member belongs to this completion.
     */
    for (i = 0u; i < impl->route_capacity; ++i) {
        turbowasm_wasi02_native_io_route *route =
            &impl->routes[i];
        bool affected = false;
        size_t member_index;
        turbowasm_status status;

        if (!route->active)
            continue;

        for (member_index = 0u;
             member_index < route->member_count;
             ++member_index) {
            uint32_t slot_index;
            uint32_t generation;
            turbowasm_value rep = {0};
            turbowasm_wasi02_native_io_poll_slot *slot;

            rep.kind = TURBOWASM_VALUE_I64;
            rep.as.i64 = (int64_t)route->members[member_index];
            if (!unpack_pollable(
                    rep, &slot_index, &generation) ||
                slot_index >= impl->pollable_capacity)
                continue;
            slot = &impl->pollables[slot_index];
            if (!slot->active ||
                slot->generation != generation)
                continue;
            if (request_equal(
                    slot->request,
                    completion->request)) {
                affected = true;
                break;
            }
        }
        if (!affected)
            continue;

        if (route->call == NULL ||
            route->wait.generation == 0u) {
            release_route(impl, route);
            final_status = TURBOWASM_TRAPPED;
            continue;
        }

        status = turbowasm_host_call_complete_wait(
            route->call,
            route->wait,
            SALTS_OK);
        release_route(impl, route);
        if (status != TURBOWASM_OK)
            final_status = status;
    }

    for (i = 0u; i < impl->pollable_capacity; ++i) {
        turbowasm_wasi02_native_io_poll_slot *slot =
            &impl->pollables[i];
        if (slot->active && slot->terminal &&
            slot->abandoned &&
            request_equal(
                slot->request,
                completion->request))
            release_slot(impl, slot);
    }

    return final_status;
}

turbowasm_status turbowasm_wasi02_native_io_poll_abandon_wait(
    turbowasm_wasi02_native_io_poll *adapter,
    uintptr_t operation_token) {
    turbowasm_wasi02_native_io_poll_impl *impl =
        impl_mut(adapter);
    turbowasm_wasi02_native_io_route *route =
        route_from_token(impl, operation_token);

    if (route == NULL || !route->active)
        return TURBOWASM_TRAPPED;

    release_route(impl, route);
    return TURBOWASM_OK;
}
