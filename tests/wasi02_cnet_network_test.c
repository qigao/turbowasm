#include <turbowasm/wasi02_cnet.h>
#include <salts/error_codes.h>
#include <tinytest.h>
#if !defined(TW_NETWORK_PUBLIC_ONLY)
#include "wasi02_sockets.h"
#endif
#include "wasi02/fixtures/generated/wasi02_toolchain_fixtures.h"
#include <string.h>
#include <stdlib.h>
#if !defined(TW_NETWORK_PUBLIC_ONLY)
#include "runtime_alloc.h"
#endif

#if !defined(TW_NETWORK_PUBLIC_ONLY)
static size_t fault_calls, fault_at, fault_live;
static void *fault_allocate(void *ctx, size_t bytes) {
    (void)ctx; if (++fault_calls == fault_at) return NULL;
    void *p = malloc(bytes); if (p) ++fault_live; return p;
}
static void fault_free(void *ctx, void *p) { (void)ctx; if (p) { --fault_live; free(p); } }
#endif
static turbowasm_wasi02_cnet adapter;
static turbowasm_wasi02_io io;
static native_io_backend backend;
static turbowasm_wasi02_socket_provider tcp;
static turbowasm_wasi02_network_provider net;
static turbowasm_wasi02_stream_provider streams;
static turbowasm_wasi02_poll_provider poll;
static turbowasm_value network, udp[2], in[2], out[2], aliases[4], dns;
static turbowasm_wasi02_cnet_config_v2 config;
static void pump(void) {
    size_t events, count = 0; uint32_t timeout; native_io_completion batch[16];
    check_equal(turbowasm_wasi02_cnet_advance(&adapter,&events), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_next_timeout(&adapter,5,&timeout), TURBOWASM_OK);
    int rc = native_io_backend_observe(&backend,batch,16,timeout,&count);
    check_true(rc == SALTS_OK || rc == SALTS_ETIMEDOUT);
    for (size_t i = 0; i < count; ++i) {
        bool consumed = false;
        check_equal(turbowasm_wasi02_cnet_route_completion(&adapter,&batch[i],&consumed), TURBOWASM_OK);
        check_true(consumed);
    }
    check_equal(turbowasm_wasi02_cnet_advance(&adapter,&events), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_io_advance(&io), TURBOWASM_OK);
}
static bool ready(turbowasm_value alias) {
    bool r = false; check_equal(poll.ready(poll.context,alias,&r), TURBOWASM_OK); return r;
}
static turbowasm_wasi02_ip_socket_address bind_udp(unsigned index, turbowasm_wasi02_ip_address_family family) {
    turbowasm_wasi02_socket_error e; turbowasm_wasi02_ip_socket_address a = {0}; a.family = family;
    if (family == TURBOWASM_WASI02_IP_ADDRESS_IPV4) { a.as.ipv4.address[0] = 127; a.as.ipv4.address[3] = 1; }
    else a.as.ipv6.address[7] = 1;
    check_equal(net.udp_create(net.context,family,&udp[index],&e), TURBOWASM_OK); check_equal(e,0);
    check_equal(net.udp_start_bind(net.context,udp[index],network,&a,&e), TURBOWASM_OK); check_equal(e,0);
    check_equal(net.udp_finish_bind(net.context,udp[index],&e), TURBOWASM_OK); check_equal(e,0);
    check_equal(net.udp_local_address(net.context,udp[index],&a,&e), TURBOWASM_OK); check_equal(e,0);
    return a;
}
static void pair(unsigned i, const turbowasm_wasi02_ip_socket_address *remote) {
    turbowasm_wasi02_socket_error e;
    for (unsigned tries = 0; tries < 100; ++tries) {
        check_equal(net.udp_stream(net.context,udp[i],remote,&in[i],&out[i],&e), TURBOWASM_OK);
        if (e != TURBOWASM_WASI02_SOCKET_ERROR_CONCURRENCY_CONFLICT) break;
        pump();
    }
    check_equal(e,0);
}
static void datagram_roundtrip(turbowasm_wasi02_ip_address_family family) {
    turbowasm_wasi02_ip_socket_address dest = bind_udp(1,family); (void)bind_udp(0,family);
    pair(1,NULL); pair(0,&dest);
    check_equal(net.incoming_subscribe(net.context,in[1],&aliases[0]), TURBOWASM_OK);
    for (unsigned round = 0; round < 3; ++round) {
        turbowasm_wasi02_socket_error e; uint64_t permit = 0; size_t sent = 0, count = 0;
        turbowasm_wasi02_outgoing_datagram records[2] = {{0}};
        records[0].data = (const uint8_t *)"hello"; records[0].size = 5;
        /* The second record is a real zero-byte datagram. */
        check_equal(net.outgoing_check_send(net.context,out[0],&permit,&e), TURBOWASM_OK); check_equal(e,0); check_true(permit >= 2);
        check_equal(net.outgoing_send(net.context,out[0],records,2,&sent,&e), TURBOWASM_OK); check_equal(e,0); check_equal(sent,2u);
        for (unsigned tries = 0; tries < 100 && !ready(aliases[0]); ++tries) pump();
        uint8_t bytes[64]; turbowasm_wasi02_incoming_datagram received = {bytes,sizeof(bytes),0,{0}};
        for (unsigned item = 0; item < 2; ++item) {
            for (unsigned tries = 0; tries < 100; ++tries) {
                check_equal(net.incoming_receive(net.context,in[1],&received,1,&count,&e), TURBOWASM_OK); check_equal(e,0);
                if (count) break; pump();
            }
            check_equal(count,1u); check_equal(received.size,item ? 0u : 5u);
            if (!item) check_equal(bytes,"hello",5);
            check_equal(received.remote_address.family,family);
        }
        pump(); check_false(ready(aliases[0]));
    }
}
static void run_component(const uint8_t *bytes, size_t size, uint32_t expected) {
    turbowasm_wasi02 facade = {0}; turbowasm_component component = {0};
    turbowasm_component_instance instance = {0}; turbowasm_component_call call = {0};
    turbowasm_component_host_value result = {0}; turbowasm_wasi02_config_v2 c;
    turbowasm_wasi02_config_v2_init(&c);
    c.base.socket_network_resource_capacity = 2; c.base.tcp_socket_resource_capacity = 2;
    c.base.pollable_capacity = 8; c.base.stream_resource_capacity = 4;
    c.udp_socket_capacity = 2; c.datagram_stream_capacity = 4; c.resolve_stream_capacity = 2;
    check_equal(turbowasm_wasi02_cnet_wasi02_init_v2(&adapter,&facade,&c,NULL), TURBOWASM_OK);
    check_equal(turbowasm_component_load_borrowed(&component,bytes,size), TURBOWASM_OK);
    check_equal(turbowasm_wasi02_component_instance_create_async(&instance,&component,&facade,NULL), TURBOWASM_OK);
    check_equal(turbowasm_component_call_create(&call,&instance,(turbowasm_name){(const uint8_t *)"run",3},NULL,0), TURBOWASM_OK);
    check_equal(turbowasm_component_call_resume(&call,NULL), TURBOWASM_OK);
    check_equal(turbowasm_component_call_take_result(&call,&result), TURBOWASM_OK);
    check_equal(result.kind,TURBOWASM_COMPONENT_HOST_U32); check_equal(result.as.u32,expected);
    check_equal(turbowasm_component_host_value_destroy(&result), TURBOWASM_OK);
    turbowasm_component_call_destroy(&call); turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
    check_equal(turbowasm_wasi02_destroy(&facade), TURBOWASM_OK);
}
static void fixture_init(bool allowed) {
        memset(&adapter,0,sizeof(adapter)); memset(&io,0,sizeof(io)); memset(&backend,0,sizeof(backend));
        memset(&network,0,sizeof(network)); memset(udp,0,sizeof(udp)); memset(in,0,sizeof(in)); memset(out,0,sizeof(out));
        memset(aliases,0,sizeof(aliases)); memset(&dns,0,sizeof(dns));
        turbowasm_wasi02_cnet_config_v2_init(&config);
        config.base.socket_capacity = 2; config.base.receive_bytes = config.base.send_bytes = 1024; config.base.payload_bytes = 4096;
        config.udp_capacity = 2; config.pair_capacity = 4; config.lookup_capacity = 2;
        config.datagram_bytes = 64; config.receive_datagrams = config.send_datagrams = 2;
        config.allow_udp_bind = config.allow_udp_send = config.allow_udp_receive = config.allow_name_lookup = allowed;
        native_io_backend_config bc = {0}; bc.kind = config.base.backend;
        bc.endpoint_capacity = 6; bc.request_capacity = 12; bc.completion_batch_capacity = 12;
        check_equal(native_io_backend_init(&backend,&bc), SALTS_OK);
        check_equal(turbowasm_wasi02_io_init(&io,NULL,NULL), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_providers(&io,&streams,&poll), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_cnet_init_external_v2(&adapter,&io,&backend,&config,NULL), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_cnet_socket_provider(&adapter,&tcp), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_cnet_network_provider(&adapter,&net), TURBOWASM_OK);
        check_equal(tcp.instance_network(tcp.context,&network), TURBOWASM_OK);
}
static void fixture_destroy(void) {
        if (!net.context) {
            if (io.impl) (void)turbowasm_wasi02_io_destroy(&io);
            if (backend.impl) { (void)native_io_backend_close(&backend); (void)native_io_backend_destroy(&backend); }
            return;
        }
        for (unsigned i = 0; i < 2; ++i) {
            if (in[i].kind) check_equal(net.incoming_drop(net.context,in[i]), TURBOWASM_OK);
            if (out[i].kind) check_equal(net.outgoing_drop(net.context,out[i]), TURBOWASM_OK);
            if (udp[i].kind) check_equal(net.udp_drop(net.context,udp[i]), TURBOWASM_OK);
        }
        if (dns.kind) check_equal(net.resolve_drop(net.context,dns), TURBOWASM_OK);
        for (unsigned i = 0; i < 4; ++i) if (aliases[i].kind) check_equal(poll.drop(poll.context,aliases[i]), TURBOWASM_OK);
        check_equal(tcp.network_drop(tcp.context,network), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_cnet_shutdown_request(&adapter), TURBOWASM_OK);
        bool complete = false;
        for (unsigned tries = 0; tries < 100 && !complete; ++tries) {
            pump(); check_equal(turbowasm_wasi02_cnet_shutdown_poll(&adapter,&complete), TURBOWASM_OK);
        }
        check_true(complete); check_equal(turbowasm_wasi02_cnet_destroy(&adapter), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_io_destroy(&io), TURBOWASM_OK);
        check_equal(native_io_backend_close(&backend), SALTS_OK); check_equal(native_io_backend_destroy(&backend), SALTS_OK);
}
suite("native WASI datagrams and name lookup") {
    before_each() { fixture_init(true); }
    after_each() { fixture_destroy(); }
    it("executes UDP creation and canonical resource drop from a real Component") {
        run_component(turbowasm_wasi02_fixture_socket_create_udp,turbowasm_wasi02_fixture_socket_create_udp_size,1);
    }
    it("executes name lookup, options, polling and ownership through memory32") {
        run_component(turbowasm_wasi02_fixture_socket_name_lookup,turbowasm_wasi02_fixture_socket_name_lookup_size,127);
    }
    it("executes name lookup, options, polling and ownership through memory64") {
        run_component(turbowasm_wasi02_fixture_socket_name_lookup64,turbowasm_wasi02_fixture_socket_name_lookup64_size,127);
    }
#if !defined(TW_NETWORK_PUBLIC_ONLY)
    it("publishes actual empty datagrams through the typed WIT record bridge") {
        turbowasm_wasi02_sockets front = {0}; turbowasm_wasi02_config_v2 c;
        turbowasm_wasi02_value family = {0}, result = {0}, self = {0}, args[3] = {{0}};
        turbowasm_wasi02_config_v2_init(&c); c.network = net; c.udp_socket_capacity = 2; c.datagram_stream_capacity = 4;
        check_equal(turbowasm_wasi02_sockets_init(&front,&tcp,2,2), TURBOWASM_OK);
        check_equal(tw_network_init(&front,&c), TURBOWASM_OK);
        family.kind = TURBOWASM_WASI02_VALUE_ENUM;
        check_equal(turbowasm_wasi02_sockets_call(&front,"udp-create-socket","create-udp-socket",&family,1,&result), TURBOWASM_OK);
        check_false(result.as.result.is_error); self = *result.as.result.value; turbowasm_wasi02_value_destroy(&result);
        check_equal(turbowasm_wasi02_sockets_call(&front,"instance-network","instance-network",NULL,0,&result), TURBOWASM_OK);
        uint32_t net_handle = result.as.resource;
        turbowasm_wasi02_ip_socket_address local = {0}; local.family = TURBOWASM_WASI02_IP_ADDRESS_IPV4;
        local.as.ipv4.address[0] = 127; local.as.ipv4.address[3] = 1;
        args[0] = self; args[1].kind = TURBOWASM_WASI02_VALUE_RESOURCE; args[1].as.resource = net_handle;
        check_equal(turbowasm_wasi02_socket_address_to_value(&local,&args[2]), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_sockets_call(&front,"udp","[method]udp-socket.start-bind",args,3,&result), TURBOWASM_OK);
        check_false(result.as.result.is_error); turbowasm_wasi02_value_destroy(&result); turbowasm_wasi02_value_destroy(&args[2]);
        check_equal(turbowasm_wasi02_sockets_call(&front,"udp","[method]udp-socket.finish-bind",&self,1,&result), TURBOWASM_OK);
        check_false(result.as.result.is_error); turbowasm_wasi02_value_destroy(&result);
        check_equal(turbowasm_wasi02_sockets_call(&front,"udp","[method]udp-socket.local-address",&self,1,&result), TURBOWASM_OK);
        check_false(result.as.result.is_error);
        check_equal(turbowasm_wasi02_socket_address_from_value(result.as.result.value,&local), TURBOWASM_OK);
        turbowasm_wasi02_value_destroy(&result);
        args[1].kind = TURBOWASM_WASI02_VALUE_OPTION; args[1].as.option.has_value = false; args[1].as.option.value = NULL;
        check_equal(turbowasm_wasi02_sockets_call(&front,"udp","[method]udp-socket.stream",args,2,&result), TURBOWASM_OK);
        check_false(result.as.result.is_error);
        turbowasm_wasi02_value incoming = result.as.result.value->as.tuple.items[0];
        turbowasm_wasi02_value outgoing = result.as.result.value->as.tuple.items[1]; turbowasm_wasi02_value_destroy(&result);
        (void)bind_udp(0,TURBOWASM_WASI02_IP_ADDRESS_IPV4); pair(0,&local);
        uint64_t permit; size_t sent; turbowasm_wasi02_socket_error e;
        turbowasm_wasi02_outgoing_datagram packet = {0};
        check_equal(net.outgoing_check_send(net.context,out[0],&permit,&e), TURBOWASM_OK); check_equal(e,0);
        check_equal(net.outgoing_send(net.context,out[0],&packet,1,&sent,&e), TURBOWASM_OK); check_equal(e,0); check_equal(sent,1u);
        args[0] = incoming; args[1].kind = TURBOWASM_WASI02_VALUE_U64; args[1].as.u64 = UINT64_MAX;
        turbowasm_value provider_in;
        check_equal(tw_network_rep(front.network,TW_NETWORK_INCOMING,incoming.as.resource,&provider_in), TURBOWASM_OK);
        check_equal(net.incoming_subscribe(net.context,provider_in,&aliases[2]), TURBOWASM_OK);
        for (unsigned tries = 0; tries < 100 && !ready(aliases[2]); ++tries) pump();
        check_true(ready(aliases[2]));
        turbowasm_runtime_config runtime; turbowasm_runtime_config_init(&runtime);
        runtime.allocator.allocate = fault_allocate; runtime.allocator.deallocate = fault_free;
        bool published = false;
        for (fault_at = 1; fault_at <= 32; ++fault_at) {
            fault_calls = fault_live = 0;
            turbowasm_runtime_scope scope = turbowasm_runtime_scope_enter(&runtime);
            turbowasm_status status = turbowasm_wasi02_sockets_call(&front,"udp","[method]incoming-datagram-stream.receive",args,2,&result);
            turbowasm_runtime_scope_leave(scope);
            if (status == TURBOWASM_OK) { published = true; break; }
            check_equal(status,TURBOWASM_OUT_OF_MEMORY); check_equal(fault_live,0u);
            check_true(ready(aliases[2]));
        }
        check_true(published); check_false(result.as.result.is_error);
        check_equal(result.as.result.value->as.list.count,1u);
        check_equal(result.as.result.value->as.list.items[0].as.record.items[0].as.list.count,0u);
        check_equal(result.as.result.value->as.list.items[0].as.record.items[1].as.variant.case_index,0u);
        turbowasm_wasi02_value_destroy(&result); check_equal(fault_live,0u);
        check_equal(tw_network_drop(front.network,TW_NETWORK_INCOMING,incoming.as.resource), TURBOWASM_OK);
        check_equal(tw_network_drop(front.network,TW_NETWORK_OUTGOING,outgoing.as.resource), TURBOWASM_OK);
        check_equal(tw_network_drop(front.network,TW_NETWORK_UDP,self.as.resource), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_network_drop(&front,net_handle), TURBOWASM_OK);
        check_equal(turbowasm_wasi02_sockets_destroy(&front), TURBOWASM_OK);
    }
#endif
    it("preserves IPv4 data, empty packets and reusable readiness") { datagram_roundtrip(TURBOWASM_WASI02_IP_ADDRESS_IPV4); }
    it("preserves IPv6 data, empty packets and reusable readiness") { datagram_roundtrip(TURBOWASM_WASI02_IP_ADDRESS_IPV6); }
    it("keeps closed aliases alive across stream replacement and disconnect") {
        turbowasm_wasi02_ip_socket_address a = bind_udp(1,TURBOWASM_WASI02_IP_ADDRESS_IPV4);
        (void)bind_udp(0,TURBOWASM_WASI02_IP_ADDRESS_IPV4); pair(0,&a);
        check_equal(net.incoming_subscribe(net.context,in[0],&aliases[0]), TURBOWASM_OK);
        check_equal(net.incoming_drop(net.context,in[0]), TURBOWASM_OK); memset(&in[0],0,sizeof(in[0]));
        check_equal(net.outgoing_drop(net.context,out[0]), TURBOWASM_OK); memset(&out[0],0,sizeof(out[0]));
        pair(0,NULL); check_true(ready(aliases[0]));
        turbowasm_wasi02_socket_error e;
        check_equal(net.udp_remote_address(net.context,udp[0],&a,&e), TURBOWASM_OK);
        check_equal(e,TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE);
        check_equal(net.udp_local_address(net.context,udp[0],&a,&e), TURBOWASM_OK); check_equal(e,0); check_not_equal(a.as.ipv4.port,0);
    }
    it("supports hop-limit and buffer options before bind") {
        turbowasm_wasi02_socket_error e; uint64_t value;
        for (unsigned i = 0; i < 2; ++i) {
            check_equal(net.udp_create(net.context,i ? TURBOWASM_WASI02_IP_ADDRESS_IPV6 : TURBOWASM_WASI02_IP_ADDRESS_IPV4,&udp[i],&e), TURBOWASM_OK); check_equal(e,0);
            check_equal(net.udp_option_set(net.context,udp[i],TURBOWASM_WASI02_UDP_HOP_LIMIT,42,&e), TURBOWASM_OK); check_equal(e,0);
            check_equal(net.udp_option_get(net.context,udp[i],TURBOWASM_WASI02_UDP_HOP_LIMIT,&value,&e), TURBOWASM_OK); check_equal(e,0); check_equal(value,42u);
            check_equal(net.udp_option_set(net.context,udp[i],TURBOWASM_WASI02_UDP_SEND_BUFFER,UINT64_MAX,&e), TURBOWASM_OK); check_equal(e,0);
            check_equal(net.udp_option_get(net.context,udp[i],TURBOWASM_WASI02_UDP_SEND_BUFFER,&value,&e), TURBOWASM_OK); check_equal(e,0); check_true(value > 0);
        }
    }
    it("enforces check-send and reports a deferred error after an admitted prefix") {
        turbowasm_wasi02_ip_socket_address a = bind_udp(1,TURBOWASM_WASI02_IP_ADDRESS_IPV4);
        (void)bind_udp(0,TURBOWASM_WASI02_IP_ADDRESS_IPV4); pair(0,&a);
        turbowasm_wasi02_socket_error e; uint64_t permit; size_t sent;
        turbowasm_wasi02_outgoing_datagram records[2] = {{0}};
        records[1].size = 65; records[1].data = (const uint8_t *)"x";
        check_equal(net.outgoing_send(net.context,out[0],NULL,0,&sent,&e), TURBOWASM_TRAPPED);
        check_equal(net.outgoing_check_send(net.context,out[0],&permit,&e), TURBOWASM_OK); check_equal(e,0);
        check_equal(net.outgoing_send(net.context,out[0],records,2,&sent,&e), TURBOWASM_OK); check_equal(e,0); check_equal(sent,1u);
        check_equal(net.outgoing_check_send(net.context,out[0],&permit,&e), TURBOWASM_OK);
        check_equal(e,TURBOWASM_WASI02_SOCKET_ERROR_DATAGRAM_TOO_LARGE);
    }
    it("resolves numeric names without DNS and pins closed poll aliases") {
        turbowasm_wasi02_socket_error e; bool has; turbowasm_wasi02_ip_address a;
        check_equal(net.resolve_addresses(net.context,network,(turbowasm_wasi02_string_view){(const uint8_t *)"::ffff:127.0.0.1",16},&dns,&e), TURBOWASM_OK); check_equal(e,0);
        check_equal(net.resolve_subscribe(net.context,dns,&aliases[0]), TURBOWASM_OK); check_true(ready(aliases[0]));
        check_equal(net.resolve_next_address(net.context,dns,&has,&a,&e), TURBOWASM_OK); check_equal(e,0); check_true(has);
        check_equal(a.family,TURBOWASM_WASI02_IP_ADDRESS_IPV4); check_equal(a.as.ipv4[0],127);
        check_equal(net.resolve_next_address(net.context,dns,&has,&a,&e), TURBOWASM_OK); check_equal(e,0); check_false(has);
        check_equal(net.resolve_drop(net.context,dns), TURBOWASM_OK); memset(&dns,0,sizeof(dns)); check_true(ready(aliases[0]));
    }
}

suite("WASI network default-deny admission") {
    before_each() { fixture_init(false); }
    after_each() { fixture_destroy(); }
    it("rejects bind and name lookup before admitting network requests") {
        turbowasm_wasi02_socket_error e;
        check_equal(net.udp_create(net.context,TURBOWASM_WASI02_IP_ADDRESS_IPV4,&udp[0],&e), TURBOWASM_OK); check_equal(e,0);
        turbowasm_wasi02_ip_socket_address address = {0}; address.family = TURBOWASM_WASI02_IP_ADDRESS_IPV4;
        address.as.ipv4.address[0] = 127; address.as.ipv4.address[3] = 1;
        check_equal(net.udp_start_bind(net.context,udp[0],network,&address,&e), TURBOWASM_OK);
        check_equal(e,TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED);
        check_equal(net.resolve_addresses(net.context,network,(turbowasm_wasi02_string_view){(const uint8_t *)"127.0.0.1",9},&dns,&e), TURBOWASM_OK);
        check_equal(e,TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED); check_equal(dns.kind,0);
        native_io_backend_stats stats; check_true(native_io_backend_get_stats(&backend,&stats));
        check_equal(stats.active_requests,0u);
    }
}
