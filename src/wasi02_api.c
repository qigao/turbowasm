#include <turbowasm/wasi02.h>

#include "component_api_internal.h"
#include "runtime_alloc.h"
#include "wasi02_exec.h"

#include <stdint.h>
#include <string.h>

#ifndef TURBOWASM_WASI02_HAS_FILESYSTEM
#define TURBOWASM_WASI02_HAS_FILESYSTEM 1
#endif

typedef struct turbowasm_wasi02_public_impl {
    turbowasm_runtime_config runtime_config;
    turbowasm_wasi02_config config;

    turbowasm_wasi02_provider provider;
    turbowasm_wasi02_filesystem filesystem;
    turbowasm_wasi02_poll poll;
    turbowasm_wasi02_streams streams;

    turbowasm_wasi02_exec_capabilities capabilities;

    bool filesystem_initialized;
    bool poll_initialized;
    bool streams_initialized;

    uint32_t instance_count;
} turbowasm_wasi02_public_impl;

static turbowasm_wasi02_public_impl *wasi02_impl(
    const turbowasm_wasi02 *wasi02) {
    return wasi02 != NULL
        ? (turbowasm_wasi02_public_impl *)wasi02->impl
        : NULL;
}

static bool config_valid(
    const turbowasm_wasi02_config *config) {
    if (config == NULL)
        return false;

    if (config->filesystem == NULL) {
        if (config->filesystem_resource_capacity != 0u)
            return false;
    } else if (config->filesystem_resource_capacity == 0u) {
        return false;
    }

    if (config->pollable_capacity == 0u) {
        if (config->stream_resource_capacity != 0u)
            return false;
    } else if (config->poll.ready == NULL) {
        return false;
    }

    if (config->stream_resource_capacity != 0u &&
        config->pollable_capacity == 0u)
        return false;

    return true;
}

static void wasi02_instance_release(void *context) {
    turbowasm_wasi02_public_impl *impl =
        (turbowasm_wasi02_public_impl *)context;

    if (impl != NULL && impl->instance_count != 0u)
        --impl->instance_count;
}

static void destroy_initialized(
    turbowasm_wasi02_public_impl *impl) {
    if (impl == NULL)
        return;

    if (impl->streams_initialized) {
        (void)turbowasm_wasi02_streams_destroy(
            &impl->streams);
        impl->streams_initialized = false;
    }
    if (impl->poll_initialized) {
        (void)turbowasm_wasi02_poll_destroy(
            &impl->poll);
        impl->poll_initialized = false;
    }
#if TURBOWASM_WASI02_HAS_FILESYSTEM
    if (impl->filesystem_initialized) {
        (void)turbowasm_wasi02_filesystem_destroy(
            &impl->filesystem);
        impl->filesystem_initialized = false;
    }
#endif
    turbowasm_wasi02_provider_destroy(&impl->provider);
}

turbowasm_status turbowasm_wasi02_init(
    turbowasm_wasi02 *wasi02,
    const turbowasm_wasi02_config *config,
    const turbowasm_runtime_config *runtime_config) {
    turbowasm_runtime_config normalized;
    turbowasm_runtime_scope scope;
    turbowasm_wasi02_public_impl *impl;
    turbowasm_status status;

    if (wasi02 == NULL || wasi02->impl != NULL ||
        !config_valid(config) ||
        !turbowasm_runtime_config_normalize(
            runtime_config, &normalized))
        return TURBOWASM_INVALID_ARGUMENT;

#if !TURBOWASM_WASI02_HAS_FILESYSTEM
    if (config->filesystem != NULL)
        return TURBOWASM_UNSUPPORTED;
#endif

    scope = turbowasm_runtime_scope_enter(&normalized);
    impl = (turbowasm_wasi02_public_impl *)
        turbowasm_rt_calloc(1u, sizeof(*impl));
    turbowasm_runtime_scope_leave(scope);
    if (impl == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    impl->runtime_config = normalized;
    impl->config = *config;

    status = turbowasm_wasi02_provider_init(
        &impl->provider,
        &impl->config.provider,
        &impl->runtime_config);
    if (status != TURBOWASM_OK)
        goto fail;
    impl->capabilities.provider = &impl->provider;

#if TURBOWASM_WASI02_HAS_FILESYSTEM
    if (impl->config.filesystem != NULL) {
        status = turbowasm_wasi02_filesystem_init(
            &impl->filesystem,
            impl->config.filesystem,
            impl->config.filesystem_resource_capacity);
        if (status != TURBOWASM_OK)
            goto fail;
        impl->filesystem_initialized = true;
        impl->capabilities.filesystem =
            &impl->filesystem;
    }
#endif

    if (impl->config.pollable_capacity != 0u) {
        status = turbowasm_wasi02_poll_init(
            &impl->poll,
            &impl->config.poll,
            impl->config.pollable_capacity);
        if (status != TURBOWASM_OK)
            goto fail;
        impl->poll_initialized = true;
        impl->capabilities.poll = &impl->poll;
    }

    if (impl->config.stream_resource_capacity != 0u) {
        status = turbowasm_wasi02_streams_init(
            &impl->streams,
            &impl->config.streams,
            impl->config.stream_resource_capacity);
        if (status != TURBOWASM_OK)
            goto fail;
        impl->streams_initialized = true;

        status = turbowasm_wasi02_streams_attach_poll(
            &impl->streams, &impl->poll);
        if (status != TURBOWASM_OK)
            goto fail;
        impl->capabilities.streams = &impl->streams;
    }

    wasi02->impl = impl;
    return TURBOWASM_OK;

fail:
    destroy_initialized(impl);
    scope = turbowasm_runtime_scope_enter(&normalized);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);
    return status;
}

turbowasm_status turbowasm_wasi02_destroy(
    turbowasm_wasi02 *wasi02) {
    turbowasm_wasi02_public_impl *impl;
    turbowasm_runtime_config runtime_config;
    turbowasm_runtime_scope scope;
    turbowasm_status status;

    if (wasi02 == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    impl = wasi02_impl(wasi02);
    if (impl == NULL)
        return TURBOWASM_OK;

    if (impl->instance_count != 0u)
        return TURBOWASM_INVALID_ARGUMENT;

    /*
     * Preflight every strict child owner before mutating any of them so a
     * failed destroy never leaves the public context half-destroyed.
     */
    if (impl->streams_initialized &&
        (impl->streams.resources.live_count != 0u ||
         impl->streams.free_count != impl->streams.capacity))
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl->poll_initialized &&
        impl->poll.resources.live_count != 0u)
        return TURBOWASM_INVALID_ARGUMENT;
#if TURBOWASM_WASI02_HAS_FILESYSTEM
    if (impl->filesystem_initialized &&
        impl->filesystem.resources.live_count != 0u)
        return TURBOWASM_INVALID_ARGUMENT;
#endif

    if (impl->streams_initialized) {
        status = turbowasm_wasi02_streams_destroy(
            &impl->streams);
        if (status != TURBOWASM_OK)
            return status;
        impl->streams_initialized = false;
    }
    if (impl->poll_initialized) {
        status = turbowasm_wasi02_poll_destroy(
            &impl->poll);
        if (status != TURBOWASM_OK)
            return status;
        impl->poll_initialized = false;
    }
#if TURBOWASM_WASI02_HAS_FILESYSTEM
    if (impl->filesystem_initialized) {
        status = turbowasm_wasi02_filesystem_destroy(
            &impl->filesystem);
        if (status != TURBOWASM_OK)
            return status;
        impl->filesystem_initialized = false;
    }
#endif

    turbowasm_wasi02_provider_destroy(&impl->provider);

    runtime_config = impl->runtime_config;
    wasi02->impl = NULL;
    scope = turbowasm_runtime_scope_enter(&runtime_config);
    turbowasm_rt_free(impl);
    turbowasm_runtime_scope_leave(scope);
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_component_instance_create(
    turbowasm_component_instance *instance,
    const turbowasm_component *component,
    turbowasm_wasi02 *wasi02) {
    turbowasm_component_public_impl *component_state;
    turbowasm_component_instance_public_impl *instance_state;
    turbowasm_wasi02_public_impl *wasi_state;
    turbowasm_runtime_scope scope;
    turbowasm_status status;

    if (instance == NULL || component == NULL ||
        wasi02 == NULL || instance->impl != NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    component_state =
        turbowasm_component_public_impl_get(component);
    wasi_state = wasi02_impl(wasi02);
    if (component_state == NULL || wasi_state == NULL ||
        wasi_state->instance_count == UINT32_MAX)
        return TURBOWASM_INVALID_ARGUMENT;

    scope = turbowasm_runtime_scope_enter(
        &component_state->binary.config);
    instance_state =
        (turbowasm_component_instance_public_impl *)
            turbowasm_rt_calloc(
                1u, sizeof(*instance_state));
    turbowasm_runtime_scope_leave(scope);
    if (instance_state == NULL)
        return TURBOWASM_OUT_OF_MEMORY;

    if (!turbowasm_component_public_impl_retain(
            component_state)) {
        scope = turbowasm_runtime_scope_enter(
            &component_state->binary.config);
        turbowasm_rt_free(instance_state);
        turbowasm_runtime_scope_leave(scope);
        return TURBOWASM_INVALID_ARGUMENT;
    }
    instance_state->component = component_state;

    status = turbowasm_wasi02_exec_init(
        &instance_state->exec,
        &component_state->binary,
        &wasi_state->capabilities);
    if (status != TURBOWASM_OK) {
        turbowasm_component_public_impl_release(
            component_state);
        scope = turbowasm_runtime_scope_enter(
            &component_state->binary.config);
        turbowasm_rt_free(instance_state);
        turbowasm_runtime_scope_leave(scope);
        return status;
    }

    ++wasi_state->instance_count;
    instance_state->owner_context = wasi_state;
    instance_state->owner_release =
        wasi02_instance_release;
    instance->impl = instance_state;
    return TURBOWASM_OK;
}
