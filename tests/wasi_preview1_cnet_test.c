#include "wasi_preview1_socket_fixture.h"
#include <turbowasm/wasi_cnet.h>
#include <salts/error_codes.h>
#include <string.h>

static turbowasm_wasi02_io io;
static turbowasm_wasi02_cnet adapter;
static native_io_backend backend;
static turbowasm_wasi02_socket_provider tcp;
static turbowasm_wasi02_network_provider net;
static turbowasm_wasi02_stream_provider streams;
static turbowasm_wasi02_poll_provider poll;
static turbowasm_value network, listener, socket_reps[2], input[2], output[2];
static turbowasm_wasi_descriptor_ops ops;
static turbowasm_wasi_fs fs;
static turbowasm_wasi_preview1 wasi;
static p1_guest guest;
static turbowasm_execution execution;
static turbowasm_wasi_fs_file identities[2];
static bool udp_mode;
static uint32_t clock_time(void *ctx, uint32_t id, uint64_t precision, uint64_t *out) {
    (void)ctx; (void)id; (void)precision; *out=0; return 0;
}
static void pump(void) {
    size_t events=0, count=0; uint32_t timeout; native_io_completion batch[16];
    check_equal(turbowasm_wasi02_cnet_advance(&adapter,&events),TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_next_timeout(&adapter,5,&timeout),TURBOWASM_OK);
    int rc=native_io_backend_observe(&backend,batch,16,timeout,&count);
    check_true(rc==SALTS_OK || rc==SALTS_ETIMEDOUT);
    for (size_t i=0; i<count; ++i) {
        bool consumed=false;
        check_equal(turbowasm_wasi02_cnet_route_completion(&adapter,&batch[i],&consumed),TURBOWASM_OK); check_true(consumed);
    }
    check_equal(turbowasm_wasi02_cnet_advance(&adapter,&events),TURBOWASM_OK);
    check_equal(turbowasm_wasi02_io_advance(&io),TURBOWASM_OK);
    check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
}
static turbowasm_wasi02_ip_socket_address loopback(turbowasm_wasi02_ip_address_family family) {
    turbowasm_wasi02_ip_socket_address a={0}; a.family=family;
    if (family==TURBOWASM_WASI02_IP_ADDRESS_IPV4) { a.as.ipv4.address[0]=127; a.as.ipv4.address[3]=1; }
    else a.as.ipv6.address[7]=1;
    return a;
}
static void bind_file(uint32_t fd, turbowasm_wasi_fs_file *f, uint8_t type) {
    turbowasm_wasi_fs_descriptor d={0};
    check_equal(turbowasm_wasi_fs_bind_socket_move(&fs,fd,&ops,f,type,0,UINT64_MAX,UINT64_MAX,&d),TURBOWASM_OK);
    check_equal(f->generation,0u);
}
static uint32_t finish_execution(void) {
    turbowasm_status status=TURBOWASM_YIELDED;
    for (unsigned i=0; i<200 && status==TURBOWASM_YIELDED; ++i) { pump(); status=turbowasm_execution_resume(&execution,NULL); }
    check_equal(status,TURBOWASM_OK); return p1_execution_result(&execution);
}
static uint32_t recv_start(uint32_t fd, uint32_t length, uint32_t flags) {
    p1_iovec(&guest,length); uint32_t a[]={fd,0,1,flags,128,132};
    p1_start(&guest,&execution,P1_RECV,a,6); return (uint32_t)turbowasm_execution_resume(&execution,NULL);
}
static uint32_t send_call(uint32_t fd, uint32_t length, uint32_t word) {
    p1_iovec(&guest,length); p1_store(&guest,512,word); uint32_t a[]={fd,0,1,0,128};
    return p1_call(&guest,P1_SEND,a,5);
}
static void tcp_pair(turbowasm_wasi02_ip_address_family family) {
    turbowasm_wasi02_socket_error e; turbowasm_wasi02_ip_socket_address a=loopback(family);
    check_equal(tcp.tcp_create(tcp.context,family,&listener,&e),TURBOWASM_OK); check_equal(e,0);
    check_equal(tcp.tcp_start_bind(tcp.context,listener,network,&a,&e),TURBOWASM_OK); check_equal(e,0);
    check_equal(tcp.tcp_finish_bind(tcp.context,listener,&e),TURBOWASM_OK); check_equal(e,0);
    check_equal(tcp.tcp_start_listen(tcp.context,listener,&e),TURBOWASM_OK); check_equal(e,0);
    check_equal(tcp.tcp_finish_listen(tcp.context,listener,&e),TURBOWASM_OK); check_equal(e,0);
    check_equal(tcp.tcp_local_address(tcp.context,listener,&a,&e),TURBOWASM_OK); check_equal(e,0);
    check_equal(tcp.tcp_create(tcp.context,family,&socket_reps[0],&e),TURBOWASM_OK); check_equal(e,0);
    check_equal(tcp.tcp_start_connect(tcp.context,socket_reps[0],network,&a,&e),TURBOWASM_OK); check_equal(e,0);
    for (unsigned i=0; i<200; ++i) {
        pump(); check_equal(tcp.tcp_finish_connect(tcp.context,socket_reps[0],&input[0],&output[0],&e),TURBOWASM_OK);
        if (e != TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK) break;
    }
    check_equal(e,0);
    /* Cross-kind and aliases fail without consuming any owned carrier. */
    turbowasm_wasi_fs_file f={0}; turbowasm_value saved=socket_reps[0], alias={0};
    check_equal(turbowasm_wasi_cnet_listener_move(&adapter,&socket_reps[0],&f),TURBOWASM_INVALID_ARGUMENT);
    check_equal(socket_reps[0].as.i64,saved.as.i64);
    check_equal(streams.input_subscribe(streams.context,input[0],&alias),TURBOWASM_OK);
    check_equal(turbowasm_wasi_cnet_tcp_move(&adapter,&socket_reps[0],&input[0],&output[0],&f),TURBOWASM_INVALID_ARGUMENT);
    check_equal(socket_reps[0].as.i64,saved.as.i64); check_equal(poll.drop(poll.context,alias),TURBOWASM_OK);
    check_equal(turbowasm_wasi_cnet_tcp_move(&adapter,&socket_reps[0],&input[0],&output[0],&f),TURBOWASM_OK);
    identities[0]=f; bind_file(4,&f,TURBOWASM_WASI_FILETYPE_SOCKET_STREAM);
    check_equal(turbowasm_wasi_cnet_listener_move(&adapter,&listener,&f),TURBOWASM_OK);
    bind_file(5,&f,TURBOWASM_WASI_FILETYPE_SOCKET_STREAM);
    uint32_t args[]={5,0,128}; p1_start(&guest,&execution,P1_ACCEPT,args,3);
    turbowasm_status status=turbowasm_execution_resume(&execution,NULL);
    if (status==TURBOWASM_YIELDED) check_equal(finish_execution(),0u);
    else { check_equal(status,TURBOWASM_OK); check_equal(p1_execution_result(&execution),0u); }
    check_equal(p1_load(&guest,128),3u); turbowasm_execution_destroy(&execution);
    turbowasm_wasi_fs_descriptor_info info; check_true(turbowasm_wasi_fs_descriptor_info_get(&fs,3,&info)); identities[1]=info.file;
    /* Listener ownership is independent of accepted child ownership. */
    check_equal(turbowasm_wasi_fs_close_fd(&fs,5),0u);
}
static void tcp_roundtrip(turbowasm_wasi02_ip_address_family family) {
    tcp_pair(family);
    check_equal(recv_start(3,4,2),TURBOWASM_YIELDED);
    check_equal(send_call(4,2,UINT32_C(0x6970)),0u);
    for (unsigned i=0; i<10; ++i) pump();
    check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
    check_equal(send_call(4,2,UINT32_C(0x676e)),0u); check_equal(finish_execution(),0u);
    check_equal(p1_load(&guest,512),UINT32_C(0x676e6970)); check_equal(p1_load(&guest,128),4u);
    turbowasm_execution_destroy(&execution);
    /* PEEK|WAITALL must admit more data while preserving the first chunk. */
    check_equal(send_call(4,2,UINT32_C(0x6f70)),0u);
    for (unsigned i=0; i<10; ++i) pump();
    check_equal(recv_start(3,4,3),TURBOWASM_YIELDED);
    check_equal(send_call(4,2,UINT32_C(0x676e)),0u); check_equal(finish_execution(),0u);
    check_equal(p1_load(&guest,512),UINT32_C(0x676e6f70)); turbowasm_execution_destroy(&execution);
    p1_iovec(&guest,4); uint32_t a[]={3,0,1,0,128,132}; check_equal(p1_call(&guest,P1_RECV,a,6),0u);
    check_equal(p1_load(&guest,512),UINT32_C(0x676e6f70));
    check_equal(recv_start(3,1,0),TURBOWASM_YIELDED);
    uint32_t shutdown[]={4,TURBOWASM_WASI_SHUTDOWN_WR}; check_equal(p1_call(&guest,P1_SHUTDOWN,shutdown,2),0u);
    check_equal(finish_execution(),0u); check_equal(p1_load(&guest,128),0u); turbowasm_execution_destroy(&execution);
    /* Preserve the reverse direction after orderly FIN. */
    check_equal(send_call(3,4,UINT32_C(0x6b636162)),0u);
    uint32_t status=recv_start(4,4,2);
    if (status==TURBOWASM_YIELDED) check_equal(finish_execution(),0u);
    else { check_equal(status,TURBOWASM_OK); check_equal(p1_execution_result(&execution),0u); }
    check_equal(p1_load(&guest,512),UINT32_C(0x6b636162));
}
static void udp_pair(turbowasm_wasi02_ip_address_family family) {
    udp_mode=true;
    turbowasm_wasi02_socket_error e; turbowasm_wasi02_ip_socket_address address[2];
    for (unsigned j=0; j<2; ++j) {
        address[j]=loopback(family);
        check_equal(net.udp_create(net.context,family,&socket_reps[j],&e),TURBOWASM_OK); check_equal(e,0);
        check_equal(net.udp_start_bind(net.context,socket_reps[j],network,&address[j],&e),TURBOWASM_OK); check_equal(e,0);
        check_equal(net.udp_finish_bind(net.context,socket_reps[j],&e),TURBOWASM_OK); check_equal(e,0);
        check_equal(net.udp_local_address(net.context,socket_reps[j],&address[j],&e),TURBOWASM_OK); check_equal(e,0);
    }
    for (unsigned j=0; j<2; ++j) {
        check_equal(net.udp_stream(net.context,socket_reps[j],&address[1-j],&input[j],&output[j],&e),TURBOWASM_OK); check_equal(e,0);
        turbowasm_wasi_fs_file f={0};
        check_equal(turbowasm_wasi_cnet_udp_move(&adapter,&socket_reps[j],&input[j],&output[j],&f),TURBOWASM_OK);
        identities[j]=f; bind_file(3+j,&f,TURBOWASM_WASI_FILETYPE_SOCKET_DGRAM);
    }
}
static void udp_roundtrip(turbowasm_wasi02_ip_address_family family) {
    udp_pair(family); check_equal(recv_start(4,2,3),TURBOWASM_YIELDED);
    check_equal(send_call(3,4,UINT32_C(0x676e6970)),0u); check_equal(finish_execution(),0u);
    check_equal(p1_load(&guest,128),2u); check_equal(p1_load(&guest,132)&65535u,TURBOWASM_WASI_RECV_DATA_TRUNCATED);
    turbowasm_execution_destroy(&execution);
    p1_subscription(&guest,0,UINT64_C(0xf00000001),1,4); p1_subscription(&guest,1,2,1,4);
    uint32_t poll_args[]={1024,2048,2,128}; check_equal(p1_call(&guest,P1_POLL,poll_args,4),0u);
    check_equal(p1_load(&guest,128),2u); check_equal(p1_load64(&guest,2064),4u); check_equal(p1_load64(&guest,2048),UINT64_C(0xf00000001));
    p1_iovec(&guest,4); uint32_t a[]={4,0,1,2,128,132}; check_equal(p1_call(&guest,P1_RECV,a,6),0u);
    check_equal(p1_load(&guest,512),UINT32_C(0x676e6970)); check_equal(p1_load(&guest,132)&65535u,0u);
    /* Zero-length messages are datagrams, including zero-length receive iovecs. */
    check_equal(send_call(3,0,0),0u); check_equal(recv_start(4,0,2),TURBOWASM_YIELDED);
    check_equal(finish_execution(),0u); check_equal(p1_load(&guest,128),0u); turbowasm_execution_destroy(&execution);
    check_equal(send_call(3,4,UINT32_C(0x61626364)),0u); check_equal(recv_start(4,1,0),TURBOWASM_YIELDED);
    check_equal(finish_execution(),0u); check_equal(p1_load(&guest,128),1u); check_equal(p1_load(&guest,132)&65535u,1u);
    turbowasm_execution_destroy(&execution);
    uint32_t flags[]={4,4}; check_equal(p1_call(&guest,P1_FLAGS,flags,2),0u);
    check_equal(p1_call(&guest,P1_RECV,a,6),TURBOWASM_WASI_ERRNO_AGAIN);
    uint32_t shutdown[]={4,3}; check_equal(p1_call(&guest,P1_SHUTDOWN,shutdown,2),0u);
    check_equal(p1_call(&guest,P1_RECV,a,6),0u); check_equal(send_call(4,1,1),TURBOWASM_WASI_ERRNO_PIPE);
}
static void fixture_init(void) {
    udp_mode=false;
    turbowasm_wasi02_cnet_config_v2 c; turbowasm_wasi02_cnet_config_v2_init(&c);
    c.base.socket_capacity=4; c.base.receive_bytes=c.base.send_bytes=16; c.base.payload_bytes=1024;
    c.base.allow_bind=c.base.allow_connect=c.base.allow_accept=true;
    c.udp_capacity=2; c.pair_capacity=2; c.lookup_capacity=1; c.datagram_bytes=16;
    c.receive_datagrams=c.send_datagrams=2; c.allow_udp_bind=c.allow_udp_send=c.allow_udp_receive=true;
    native_io_backend_config b={0}; b.kind=c.base.backend; b.endpoint_capacity=12; b.request_capacity=24; b.completion_batch_capacity=16;
    check_equal(native_io_backend_init(&backend,&b),SALTS_OK); check_equal(turbowasm_wasi02_io_init(&io,NULL,NULL),TURBOWASM_OK);
    check_equal(turbowasm_wasi02_io_providers(&io,&streams,&poll),TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_init_external_v2(&adapter,&io,&backend,&c,NULL),TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_socket_provider(&adapter,&tcp),TURBOWASM_OK);
    check_equal(turbowasm_wasi02_cnet_network_provider(&adapter,&net),TURBOWASM_OK);
    check_equal(tcp.instance_network(tcp.context,&network),TURBOWASM_OK);
    check_equal(turbowasm_wasi_cnet_descriptor_ops(&adapter,&ops),TURBOWASM_OK);
    turbowasm_wasi_fs_config fc={8,ops.file}; check_equal(turbowasm_wasi_fs_init(&fs,&fc),TURBOWASM_OK);
    turbowasm_wasi_preview1_config_v2 v; turbowasm_wasi_preview1_config_v2_init(&v);
    v.base.filesystem=&fs; v.base.allow_fd_read=v.base.allow_fd_write=v.base.allow_clock=true; v.base.clock_time=clock_time;
    v.allow_sockets=v.allow_poll=true; v.io_bytes=16;
    check_equal(turbowasm_wasi_preview1_init_v2(&wasi,&v),TURBOWASM_OK); p1_guest_init(&guest,&wasi);
}
static void fixture_destroy(void) {
    if (!adapter.impl) {
        if (io.impl) (void)turbowasm_wasi02_io_destroy(&io);
        if (backend.impl) { (void)native_io_backend_close(&backend); (void)native_io_backend_destroy(&backend); }
        return;
    }
    turbowasm_execution_destroy(&execution); p1_guest_destroy(&guest);
    for (uint32_t fd=0; fd<8; ++fd) {
        turbowasm_wasi_fs_descriptor_info info;
        if (turbowasm_wasi_fs_descriptor_info_get(&fs,fd,&info)) check_equal(turbowasm_wasi_fs_close_fd(&fs,fd),0u);
    }
    for (unsigned i=0; i<2; ++i) {
        if (input[i].kind) { if (udp_mode) (void)net.incoming_drop(net.context,input[i]); else streams.input_drop(streams.context,input[i]); }
        if (output[i].kind) { if (udp_mode) (void)net.outgoing_drop(net.context,output[i]); else streams.output_drop(streams.context,output[i]); }
        if (socket_reps[i].kind) { if (udp_mode) (void)net.udp_drop(net.context,socket_reps[i]); else (void)tcp.tcp_drop(tcp.context,socket_reps[i]); }
        socket_reps[i]=(turbowasm_value){0}; input[i]=(turbowasm_value){0}; output[i]=(turbowasm_value){0};
        identities[i]=(turbowasm_wasi_fs_file){0};
    }
    if (listener.kind) { (void)tcp.tcp_drop(tcp.context,listener); listener=(turbowasm_value){0}; }
    check_equal(tcp.network_drop(tcp.context,network),TURBOWASM_OK); network=(turbowasm_value){0};
    check_equal(turbowasm_wasi02_cnet_shutdown_request(&adapter),TURBOWASM_OK);
    bool complete=false;
    for (unsigned i=0; i<200 && !complete; ++i) { pump(); check_equal(turbowasm_wasi02_cnet_shutdown_poll(&adapter,&complete),TURBOWASM_OK); }
    check_true(complete); check_equal(turbowasm_wasi02_cnet_destroy(&adapter),TURBOWASM_OK);
    check_equal(turbowasm_wasi02_io_destroy(&io),TURBOWASM_OK);
    check_equal(turbowasm_wasi_preview1_destroy_checked(&wasi),TURBOWASM_OK); check_equal(turbowasm_wasi_fs_destroy(&fs),TURBOWASM_OK);
    check_equal(native_io_backend_close(&backend),SALTS_OK); check_equal(native_io_backend_destroy(&backend),SALTS_OK);
}
suite("Preview1 CNet transport") {
    before_each() { fixture_init(); }
    after_each() { fixture_destroy(); }
    it("accepts and round trips IPv4 TCP with PEEK WAITALL and directional FIN") { tcp_roundtrip(TURBOWASM_WASI02_IP_ADDRESS_IPV4); }
    it("accepts and round trips IPv6 TCP with PEEK WAITALL and directional FIN") { tcp_roundtrip(TURBOWASM_WASI02_IP_ADDRESS_IPV6); }
    it("preserves IPv4 UDP packet boundaries, truncation, polling and empty messages") { udp_roundtrip(TURBOWASM_WASI02_IP_ADDRESS_IPV4); }
    it("preserves IPv6 UDP packet boundaries, truncation, polling and empty messages") { udp_roundtrip(TURBOWASM_WASI02_IP_ADDRESS_IPV6); }
    it("cancels a real parked receive while CNet retains actual native IO") {
        tcp_pair(TURBOWASM_WASI02_IP_ADDRESS_IPV4); check_equal(recv_start(3,4,0),TURBOWASM_YIELDED);
        check_equal(turbowasm_wasi_preview1_destroy_checked(&wasi),TURBOWASM_INVALID_ARGUMENT);
        turbowasm_execution_destroy(&execution); check_equal(send_call(4,4,UINT32_C(0x646e6573)),0u);
        check_equal(recv_start(3,4,2),TURBOWASM_YIELDED); check_equal(finish_execution(),0u);
        check_equal(p1_load(&guest,512),UINT32_C(0x646e6573));
    }
}
