#include "../src/wasi02_sockets.h"
#include "../src/component_type_graph.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct socket_probe {
    uint32_t instance_network_calls;
    uint32_t network_drop_calls;
} socket_probe;

static turbowasm_component_name cname(const char *text) {
    turbowasm_component_name name;
    name.bytes = (const uint8_t *)text;
    name.size = (uint32_t)strlen(text);
    return name;
}

static turbowasm_status instance_network(
    void *context,
    turbowasm_value *out_rep) {
    socket_probe *probe = (socket_probe *)context;

    assert(probe != NULL);
    assert(out_rep != NULL);
    ++probe->instance_network_calls;
    out_rep->kind = TURBOWASM_VALUE_I32;
    out_rep->as.i32 = 77;
    return TURBOWASM_OK;
}

static turbowasm_status socket_drop(
    void *context,
    turbowasm_value rep) {
    socket_probe *probe = (socket_probe *)context;

    assert(probe != NULL);
    assert(rep.kind == TURBOWASM_VALUE_I32);
    assert(rep.as.i32 == 77);
    ++probe->network_drop_calls;
    return TURBOWASM_OK;
}

static turbowasm_status tcp_create(
    void *context,
    turbowasm_wasi02_ip_address_family family,
    turbowasm_value *out_rep,
    turbowasm_wasi02_socket_error *out_error) {
    (void)context;
    (void)family;
    if (out_rep == NULL || out_error == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    out_rep->kind = TURBOWASM_VALUE_I32;
    out_rep->as.i32 = 88;
    *out_error = TURBOWASM_WASI02_SOCKET_ERROR_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status tcp_drop(
    void *context,
    turbowasm_value rep) {
    (void)context;
    (void)rep;
    return TURBOWASM_OK;
}

int main(void) {
    socket_probe probe = {0};
    turbowasm_wasi02_socket_provider provider = {0};
    turbowasm_wasi02_sockets sockets = {0};
    turbowasm_component_exec_imports imports = {0};
    turbowasm_component_type_graph graph = {0};
    turbowasm_component_value result = {0};
    turbowasm_component_value lifted = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_value rep = {0};
    uint32_t handle;

    provider.context = &probe;
    provider.instance_network = instance_network;
    provider.network_drop = socket_drop;
    provider.tcp_create = tcp_create;
    provider.tcp_drop = tcp_drop;

    assert(turbowasm_wasi02_sockets_init(
               &sockets, &provider, 4u, 4u) == TURBOWASM_OK);

    assert(turbowasm_component_type_graph_allocate(
               &graph, 3u));
    assert(turbowasm_component_type_graph_define_resource(
               &graph, 0u, UINT64_C(0x51525354)));
    assert(turbowasm_component_type_graph_define_handle(
               &graph, 1u,
               TURBOWASM_COMPONENT_TYPE_OWN,
               0u));
    assert(turbowasm_component_type_graph_define_function(
               &graph, 2u,
               NULL, 0u,
               true,
               turbowasm_component_type_ref_indexed(1u)));
    assert(turbowasm_component_type_graph_validate(&graph));

    assert(turbowasm_wasi02_sockets_imports(
               &sockets, &imports) == TURBOWASM_OK);
    assert(imports.can_bind != NULL);
    assert(imports.invoke != NULL);
    assert(imports.resource_lift != NULL);
    assert(imports.resource_drop != NULL);

    assert(imports.can_bind(
        imports.context,
        cname("wasi:sockets/instance-network@0.2.8"),
        cname("instance-network"),
        &graph,
        2u));
    assert(sockets.component_network_identity_bound);
    assert(sockets.component_network_identity ==
           UINT64_C(0x51525354));

    assert(imports.invoke(
               imports.context,
               NULL,
               cname("wasi:sockets/instance-network@0.2.8"),
               cname("instance-network"),
               &graph,
               2u,
               NULL, 0u,
               &result,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(probe.instance_network_calls == 1u);
    assert(result.kind == TURBOWASM_COMPONENT_TYPE_OWN);
    assert(result.as.resource_rep.kind == TURBOWASM_VALUE_I32);
    assert(result.as.resource_rep.as.i32 > 0);
    handle = (uint32_t)result.as.resource_rep.as.i32;

    assert(imports.resource_lift(
               imports.context,
               &graph,
               turbowasm_component_type_ref_indexed(1u),
               handle,
               &lifted) == TURBOWASM_OK);
    assert(lifted.kind == TURBOWASM_COMPONENT_TYPE_OWN);
    assert((uint32_t)lifted.as.resource_rep.as.i32 == handle);

    assert(turbowasm_component_resource_rep(
               &sockets.networks,
               handle,
               UINT64_C(0x77617369326e6574),
               &rep) == TURBOWASM_OK);
    assert(rep.kind == TURBOWASM_VALUE_I32);
    assert(rep.as.i32 == 77);

    assert(imports.resource_drop(
               imports.context,
               UINT64_C(0x51525354),
               handle) == TURBOWASM_OK);
    assert(probe.network_drop_calls == 1u);

    assert(turbowasm_wasi02_sockets_destroy(
               &sockets) == TURBOWASM_OK);
    turbowasm_component_type_graph_destroy(&graph);
    return 0;
}
