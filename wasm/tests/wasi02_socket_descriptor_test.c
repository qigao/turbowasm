#include "../src/wasi02_descriptor.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static void assert_v028(turbowasm_wasi02_version version) {
    assert(version.major == 0u);
    assert(version.minor == 2u);
    assert(version.patch == 8u);
}

static void assert_sockets_source(
    const turbowasm_wasi02_interface_desc *iface) {
    assert(iface != NULL);
    assert_v028(iface->version);
    assert(strcmp(
        iface->source_repository,
        "https://github.com/WebAssembly/wasi-sockets") == 0);
    assert(strcmp(
        iface->source_commit,
        "85f0c064f5b9ea2faa3c65b1a80b870119c0fc7f") == 0);
}

int main(void) {
    static const char *const tcp_names[] = {
        "[method]tcp-socket.start-bind",
        "[method]tcp-socket.finish-bind",
        "[method]tcp-socket.start-connect",
        "[method]tcp-socket.finish-connect",
        "[method]tcp-socket.start-listen",
        "[method]tcp-socket.finish-listen",
        "[method]tcp-socket.accept",
        "[method]tcp-socket.local-address",
        "[method]tcp-socket.remote-address",
        "[method]tcp-socket.is-listening",
        "[method]tcp-socket.address-family",
        "[method]tcp-socket.set-listen-backlog-size",
        "[method]tcp-socket.keep-alive-enabled",
        "[method]tcp-socket.set-keep-alive-enabled",
        "[method]tcp-socket.keep-alive-idle-time",
        "[method]tcp-socket.set-keep-alive-idle-time",
        "[method]tcp-socket.keep-alive-interval",
        "[method]tcp-socket.set-keep-alive-interval",
        "[method]tcp-socket.keep-alive-count",
        "[method]tcp-socket.set-keep-alive-count",
        "[method]tcp-socket.hop-limit",
        "[method]tcp-socket.set-hop-limit",
        "[method]tcp-socket.receive-buffer-size",
        "[method]tcp-socket.set-receive-buffer-size",
        "[method]tcp-socket.send-buffer-size",
        "[method]tcp-socket.set-send-buffer-size",
        "[method]tcp-socket.subscribe",
        "[method]tcp-socket.shutdown"
    };
    const turbowasm_wasi02_interface_desc *network =
        turbowasm_wasi02_find_interface("wasi:sockets", "network");
    const turbowasm_wasi02_interface_desc *tcp =
        turbowasm_wasi02_find_interface("wasi:sockets", "tcp");
    const turbowasm_wasi02_interface_desc *create =
        turbowasm_wasi02_find_interface(
            "wasi:sockets", "tcp-create-socket");
    const turbowasm_wasi02_interface_desc *instance_network =
        turbowasm_wasi02_find_interface(
            "wasi:sockets", "instance-network");
    const turbowasm_wasi02_function_desc *start_bind;
    const turbowasm_wasi02_function_desc *finish_connect;
    const turbowasm_wasi02_function_desc *accept_fn;
    const turbowasm_wasi02_function_desc *keepalive;
    const turbowasm_wasi02_function_desc *shutdown_fn;
    const turbowasm_wasi02_function_desc *create_fn;
    const turbowasm_wasi02_function_desc *instance_fn;
    const turbowasm_wasi02_type_desc *address;
    const turbowasm_wasi02_type_desc *ipv4;
    const turbowasm_wasi02_type_desc *ipv6;
    const turbowasm_wasi02_type_desc *error_code;
    const turbowasm_wasi02_type_desc *network_resource;
    const turbowasm_wasi02_type_desc *tcp_socket;
    size_t i;

    assert_sockets_source(network);
    assert_sockets_source(tcp);
    assert_sockets_source(create);
    assert_sockets_source(instance_network);

    /* network-error-code is upstream unstable and excluded from baseline. */
    assert(network->function_count == 0u);
    assert(network->functions == NULL);

    assert(tcp->function_count ==
           sizeof(tcp_names) / sizeof(tcp_names[0]));
    for (i = 0u; i < tcp->function_count; ++i) {
        assert(turbowasm_wasi02_find_function(
                   tcp, tcp_names[i]) != NULL);
    }

    start_bind = turbowasm_wasi02_find_function(
        tcp, "[method]tcp-socket.start-bind");
    assert(start_bind->param_count == 3u);
    tcp_socket = start_bind->params[0].type;
    assert(tcp_socket->kind == TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(strcmp(
        tcp_socket->as.resource.resource_name,
        "tcp-socket") == 0);
    network_resource = start_bind->params[1].type;
    assert(network_resource->kind == TURBOWASM_WASI02_TYPE_RESOURCE);
    assert(strcmp(
        network_resource->as.resource.interface_name,
        "network") == 0);

    address = start_bind->params[2].type;
    assert(address->kind == TURBOWASM_WASI02_TYPE_VARIANT);
    assert(strcmp(address->name, "ip-socket-address") == 0);
    assert(address->as.variant.count == 2u);
    ipv4 = address->as.variant.cases[0].payload;
    ipv6 = address->as.variant.cases[1].payload;
    assert(ipv4->kind == TURBOWASM_WASI02_TYPE_RECORD);
    assert(ipv4->as.record.fields[0].type->kind ==
           TURBOWASM_WASI02_TYPE_U16);
    assert(ipv4->as.record.fields[1].type->kind ==
           TURBOWASM_WASI02_TYPE_TUPLE);
    assert(ipv4->as.record.fields[1].type->as.tuple.count == 4u);
    assert(ipv4->as.record.fields[1].type
               ->as.tuple.elements[0]->kind ==
           TURBOWASM_WASI02_TYPE_U8);
    assert(ipv6->kind == TURBOWASM_WASI02_TYPE_RECORD);
    assert(ipv6->as.record.fields[0].type->kind ==
           TURBOWASM_WASI02_TYPE_U16);
    assert(ipv6->as.record.fields[2].type->kind ==
           TURBOWASM_WASI02_TYPE_TUPLE);
    assert(ipv6->as.record.fields[2].type->as.tuple.count == 8u);
    assert(ipv6->as.record.fields[2].type
               ->as.tuple.elements[7]->kind ==
           TURBOWASM_WASI02_TYPE_U16);

    assert(start_bind->result->kind == TURBOWASM_WASI02_TYPE_RESULT);
    assert(start_bind->result->as.result.ok == NULL);
    error_code = start_bind->result->as.result.error;
    assert(error_code != NULL);
    assert(error_code->kind == TURBOWASM_WASI02_TYPE_ENUM);
    assert(strcmp(error_code->name, "error-code") == 0);
    assert(error_code->as.enumeration.count == 21u);
    assert(strcmp(
        error_code->as.enumeration.labels[8],
        "would-block") == 0);
    assert(strcmp(
        error_code->as.enumeration.labels[20],
        "permanent-resolver-failure") == 0);

    finish_connect = turbowasm_wasi02_find_function(
        tcp, "[method]tcp-socket.finish-connect");
    assert(finish_connect->result->kind ==
           TURBOWASM_WASI02_TYPE_RESULT);
    assert(finish_connect->result->as.result.ok->kind ==
           TURBOWASM_WASI02_TYPE_TUPLE);
    assert(finish_connect->result->as.result.ok->as.tuple.count == 2u);
    assert(strcmp(
        finish_connect->result->as.result.ok
            ->as.tuple.elements[0]->as.resource.resource_name,
        "input-stream") == 0);
    assert(strcmp(
        finish_connect->result->as.result.ok
            ->as.tuple.elements[1]->as.resource.resource_name,
        "output-stream") == 0);
    assert(finish_connect->result->as.result.error == error_code);

    accept_fn = turbowasm_wasi02_find_function(
        tcp, "[method]tcp-socket.accept");
    assert(accept_fn->result->as.result.ok->kind ==
           TURBOWASM_WASI02_TYPE_TUPLE);
    assert(accept_fn->result->as.result.ok->as.tuple.count == 3u);
    assert(accept_fn->result->as.result.ok
               ->as.tuple.elements[0] == tcp_socket);

    keepalive = turbowasm_wasi02_find_function(
        tcp, "[method]tcp-socket.keep-alive-idle-time");
    assert(keepalive->result->as.result.ok->kind ==
           TURBOWASM_WASI02_TYPE_ALIAS);
    assert(strcmp(
        keepalive->result->as.result.ok->name,
        "duration") == 0);
    assert(keepalive->result->as.result.ok->as.alias.target->kind ==
           TURBOWASM_WASI02_TYPE_U64);

    shutdown_fn = turbowasm_wasi02_find_function(
        tcp, "[method]tcp-socket.shutdown");
    assert(shutdown_fn->param_count == 2u);
    assert(shutdown_fn->params[1].type->kind ==
           TURBOWASM_WASI02_TYPE_ENUM);
    assert(shutdown_fn->params[1].type->as.enumeration.count == 3u);
    assert(strcmp(
        shutdown_fn->params[1].type->as.enumeration.labels[2],
        "both") == 0);

    assert(create->function_count == 1u);
    create_fn = turbowasm_wasi02_find_function(
        create, "create-tcp-socket");
    assert(create_fn != NULL);
    assert(create_fn->param_count == 1u);
    assert(create_fn->params[0].type->kind ==
           TURBOWASM_WASI02_TYPE_ENUM);
    assert(strcmp(
        create_fn->params[0].type->name,
        "ip-address-family") == 0);
    assert(create_fn->result->as.result.ok == tcp_socket);
    assert(create_fn->result->as.result.error == error_code);

    assert(instance_network->function_count == 1u);
    instance_fn = turbowasm_wasi02_find_function(
        instance_network, "instance-network");
    assert(instance_fn != NULL);
    assert(instance_fn->param_count == 0u);
    assert(instance_fn->result == network_resource);

    return 0;
}
