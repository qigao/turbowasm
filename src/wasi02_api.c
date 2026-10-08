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
    void *io_owner;
    void (*io_owner_release)(void *);
    void *socket_owner;
    void (*socket_owner_release)(void *);
    turbowasm_runtime_config runtime_config;
    turbowasm_wasi02_config config;

    turbowasm_wasi02_provider provider;
    turbowasm_wasi02_filesystem filesystem;
    turbowasm_wasi02_poll poll;
    turbowasm_wasi02_streams streams;
    turbowasm_wasi02_sockets sockets;

    turbowasm_wasi02_exec_capabilities capabilities;

    bool filesystem_initialized;
    bool poll_initialized;
    bool streams_initialized;
    bool sockets_initialized;

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

    if ((config->socket_network_resource_capacity == 0u) !=
        (config->tcp_socket_resource_capacity == 0u))
        return false;
    if (config->socket_network_resource_capacity != 0u &&
        (config->pollable_capacity == 0u ||
         config->stream_resource_capacity == 0u))
        return false;

    if ((config->streams.get_stdin != NULL ||
         config->streams.get_stdout != NULL ||
         config->streams.get_stderr != NULL) &&
        config->stream_resource_capacity == 0u)
        return false;
    if (config->streams.get_stdin != NULL &&
        config->streams.input_drop == NULL)
        return false;
    if ((config->streams.get_stdout != NULL ||
         config->streams.get_stderr != NULL) &&
        config->streams.output_drop == NULL)
        return false;

    if (config->filesystem_streams.read_via_stream != NULL ||
        config->filesystem_streams.write_via_stream != NULL ||
        config->filesystem_streams.append_via_stream != NULL) {
        if (config->filesystem == NULL ||
            config->stream_resource_capacity == 0u)
            return false;
        if (config->filesystem_streams.read_via_stream != NULL &&
            config->streams.input_drop == NULL)
            return false;
        if ((config->filesystem_streams.write_via_stream != NULL ||
             config->filesystem_streams.append_via_stream != NULL) &&
            config->streams.output_drop == NULL)
            return false;
    }

    return true;
}

static bool component_resources_quiescent(
    const turbowasm_wasi02_public_impl *impl) {
    if (impl == NULL)
        return false;

    if (impl->sockets_initialized &&
        (impl->sockets.networks.live_count != 0u ||
         impl->sockets.tcp_resources.live_count != 0u ||
         impl->sockets.tcp_free_count !=
             impl->sockets.tcp_capacity))
        return false;
    if (impl->sockets_initialized && impl->sockets.network)
        for (unsigned i = 0; i < TW_NETWORK_KINDS; ++i)
            if (impl->sockets.network->tables[i].live_count) return false;
    if (impl->streams_initialized &&
        (impl->streams.resources.live_count != 0u ||
         impl->streams.free_count != impl->streams.capacity))
        return false;
    if (impl->poll_initialized &&
        impl->poll.resources.live_count != 0u)
        return false;
#if TURBOWASM_WASI02_HAS_FILESYSTEM
    if (impl->filesystem_initialized &&
        impl->filesystem.resources.live_count != 0u)
        return false;
#endif
    return true;
}

static void reset_component_resource_identities(
    turbowasm_wasi02_public_impl *impl) {
    if (impl == NULL)
        return;

#if TURBOWASM_WASI02_HAS_FILESYSTEM
    if (impl->filesystem_initialized) {
        impl->filesystem.descriptor_identity = 0u;
        impl->filesystem.descriptor_identity_bound = false;
    }
#endif
    if (impl->poll_initialized) {
        impl->poll.pollable_identity = 0u;
        impl->poll.pollable_identity_bound = false;
    }
    if (impl->streams_initialized) {
        impl->streams.component_input_identity = 0u;
        impl->streams.component_output_identity = 0u;
        impl->streams.component_error_identity = 0u;
        impl->streams.component_input_identity_bound = false;
        impl->streams.component_output_identity_bound = false;
        impl->streams.component_error_identity_bound = false;
    }
    if (impl->sockets_initialized) {
        if (impl->sockets.network) {
            memset(impl->sockets.network->identities, 0, sizeof(impl->sockets.network->identities));
            memset(impl->sockets.network->identity_bound, 0, sizeof(impl->sockets.network->identity_bound));
        }
        impl->sockets.component_network_identity = 0u;
        impl->sockets.component_tcp_identity = 0u;
        impl->sockets.component_network_identity_bound = false;
        impl->sockets.component_tcp_identity_bound = false;
    }
}

static void wasi02_instance_release(void *context) {
    turbowasm_wasi02_public_impl *impl =
        (turbowasm_wasi02_public_impl *)context;

    if (impl == NULL || impl->instance_count == 0u)
        return;

    --impl->instance_count;
    if (impl->instance_count == 0u &&
        component_resources_quiescent(impl))
        reset_component_resource_identities(impl);
}

static void destroy_initialized(
    turbowasm_wasi02_public_impl *impl) {
    if (impl == NULL)
        return;

    if (impl->sockets_initialized) {
        (void)turbowasm_wasi02_sockets_destroy(
            &impl->sockets);
        impl->sockets_initialized = false;
    }
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

    if (impl->config.socket_network_resource_capacity != 0u) {
        status = turbowasm_wasi02_sockets_init(
            &impl->sockets,
            &impl->config.sockets,
            impl->config.socket_network_resource_capacity,
            impl->config.tcp_socket_resource_capacity);
        if (status != TURBOWASM_OK)
            goto fail;
        impl->sockets_initialized = true;

        status = turbowasm_wasi02_sockets_attach_io(
            &impl->sockets,
            &impl->streams,
            &impl->poll);
        if (status != TURBOWASM_OK)
            goto fail;
        impl->capabilities.sockets = &impl->sockets;
    }

#if TURBOWASM_WASI02_HAS_FILESYSTEM
    if (impl->filesystem_initialized &&
        impl->streams_initialized) {
        status = turbowasm_wasi02_filesystem_attach_streams(
            &impl->filesystem,
            &impl->streams,
            &impl->config.filesystem_streams);
        if (status != TURBOWASM_OK)
            goto fail;
    }
#endif

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
    if (impl->sockets_initialized &&
        (impl->sockets.networks.live_count != 0u ||
         impl->sockets.tcp_resources.live_count != 0u ||
         impl->sockets.tcp_free_count !=
             impl->sockets.tcp_capacity))
        return TURBOWASM_INVALID_ARGUMENT;
    if (impl->sockets_initialized && impl->sockets.network)
        for (unsigned i = 0; i < TW_NETWORK_KINDS; ++i)
            if (impl->sockets.network->tables[i].live_count) return TURBOWASM_INVALID_ARGUMENT;
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

    if (impl->sockets_initialized) {
        status = turbowasm_wasi02_sockets_destroy(
            &impl->sockets);
        if (status != TURBOWASM_OK)
            return status;
        impl->sockets_initialized = false;
    }
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
    if (impl->io_owner_release != NULL) impl->io_owner_release(impl->io_owner);
    if (impl->socket_owner_release != NULL) impl->socket_owner_release(impl->socket_owner);
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
    instance_state->ref_count = 1u;
    instance->impl = instance_state;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_set_transport_observer(turbowasm_wasi02 *wasi,
    turbowasm_status (*closed)(void *, turbowasm_value, bool *), void *context, void (*release)(void *)) {
    turbowasm_wasi02_public_impl *p = wasi02_impl(wasi);
    if (!p || !p->sockets_initialized || p->instance_count || p->socket_owner || !closed || !release)
        return TURBOWASM_INVALID_ARGUMENT;
    p->sockets.transport_closed = closed; p->sockets.transport_context = context;
    p->socket_owner = context; p->socket_owner_release = release; return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_set_wait_cleanup(turbowasm_wasi02 *wasi02,
    void (*cleanup)(void *, uintptr_t), void *context, void (*owner_release)(void *)) {
    turbowasm_wasi02_public_impl *impl = wasi02_impl(wasi02);
    if (impl == NULL || impl->instance_count || !impl->poll_initialized) return TURBOWASM_INVALID_ARGUMENT;
    impl->poll.wait_done = cleanup; impl->poll.wait_done_context = context;
    impl->io_owner = context; impl->io_owner_release = owner_release; return TURBOWASM_OK;
}

turbowasm_status turbowasm_wasi02_component_instance_create_async(
    turbowasm_component_instance *instance, const turbowasm_component *component,
    turbowasm_wasi02 *wasi02, const turbowasm_component_async_options *options) {
    turbowasm_wasi02_public_impl *wasi = wasi02_impl(wasi02);
    turbowasm_component_exec_imports imports[5]; size_t count = 0;
    turbowasm_component_async_options defaults;
    turbowasm_component_instance_public_impl *impl;
    turbowasm_status status;
    if (wasi == NULL || wasi->instance_count == UINT32_MAX) return TURBOWASM_INVALID_ARGUMENT;
    if (options == NULL) { turbowasm_component_async_options_init(&defaults); options = &defaults; }
    status = turbowasm_wasi02_exec_import_sets(&wasi->capabilities, imports, 5u, &count);
    if (status != TURBOWASM_OK) return status;
    /* Pin the facade before allocator/import callbacks can attempt destruction. */
    ++wasi->instance_count;
    status = turbowasm_component_instance_create_async_with_options_private(instance, component, options, imports, count);
    if (status != TURBOWASM_OK) { --wasi->instance_count; return status; }
    impl = turbowasm_component_instance_public_impl_get(instance);
    impl->owner_context = wasi; impl->owner_release = wasi02_instance_release;
    return TURBOWASM_OK;
}

void turbowasm_wasi02_config_v2_init(turbowasm_wasi02_config_v2 *c) {
    if (!c) return;
    memset(c, 0, sizeof(*c)); c->size = sizeof(*c); c->api_version = 2;
    c->network.size = sizeof(c->network); c->network.api_version = 1;
}
turbowasm_status turbowasm_wasi02_set_network_private(turbowasm_wasi02 *wasi,
    const turbowasm_wasi02_config_v2 *config) {
    turbowasm_wasi02_public_impl *p = wasi02_impl(wasi);
    if (!p || p->instance_count) return TURBOWASM_INVALID_ARGUMENT;
    if (!p->sockets_initialized) return !config->udp_socket_capacity && !config->datagram_stream_capacity && !config->resolve_stream_capacity ? TURBOWASM_OK : TURBOWASM_INVALID_ARGUMENT;
    turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&p->runtime_config);
    turbowasm_status status = tw_network_init(&p->sockets, config);
    turbowasm_runtime_scope_leave(scope); return status;
}
turbowasm_status turbowasm_wasi02_init_v2(turbowasm_wasi02 *wasi,
    const turbowasm_wasi02_config_v2 *config, const turbowasm_runtime_config *runtime) {
    if (!config || config->size != sizeof(*config) || config->api_version != 2)
        return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = turbowasm_wasi02_init(wasi, &config->base, runtime);
    if (status != TURBOWASM_OK) return status;
    status = turbowasm_wasi02_set_network_private(wasi, config);
    if (status != TURBOWASM_OK) (void)turbowasm_wasi02_destroy(wasi);
    return status;
}
