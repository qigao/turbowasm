#include "wasi_preview1_socket_fixture.h"
#include <string.h>

typedef struct p1_file {
    bool open, eof;
    unsigned leases, closes, reads, writes, accepts;
    uint8_t bytes[32]; size_t size, target;
} p1_file;
static p1_file files[8];
static turbowasm_wasi_fs fs;
static turbowasm_wasi_preview1 wasi;
static p1_guest guest, peer;
static turbowasm_execution execution, second;
static turbowasm_wasi_descriptor_ops ops;
static uint64_t now, last_precision;
enum { EFFECT_RETAIN=1, EFFECT_RECV, EFFECT_SEND, EFFECT_ACCEPT, EFFECT_CLOCK };
enum { GROW_MEMORY=1, MUTATE_IOV=2, MUTATE_POLL=4, MUTATE_PAYLOAD=8 };
static unsigned effect_callback, effect_flags, effects;
static uint8_t sent[32];
static size_t sent_count;
static bool send_again;
static uint32_t recv_error;
static bool recv_overreport;
/* Another instance owns the memory instructions: callbacks never reenter the
 * currently executing guest instance to mutate/grow its backing storage. */
static void effect(unsigned callback) {
    if (callback != effect_callback) return;
    effect_callback=0; ++effects;
    if (effect_flags & MUTATE_IOV) { p1_store(&peer,0,700); p1_store(&peer,4,32); }
    if (effect_flags & MUTATE_POLL) {
        p1_store(&peer,1072,99); p1_store(&peer,1080,255); p1_store(&peer,1088,99);
    }
    if (effect_flags & MUTATE_PAYLOAD) p1_store(&peer,512,0);
    if (effect_flags & GROW_MEMORY) {
        turbowasm_value pages=p1_i32(1);
        check_equal(p1_invoke(&peer,P1_GROW,&pages,1).as.i32,1);
    }
}
static uint32_t mock_close(void *ctx, turbowasm_wasi_fs_file f) {
    (void)ctx; if (!files[f.object].open) return TURBOWASM_WASI_ERRNO_BADF;
    files[f.object].open=false; ++files[f.object].closes; return 0;
}
static uint32_t mock_retain(void *ctx, turbowasm_wasi_fs_file f) { (void)ctx; effect(EFFECT_RETAIN); ++files[f.object].leases; return 0; }
static void mock_release(void *ctx, turbowasm_wasi_fs_file f) { (void)ctx; --files[f.object].leases; }
static void mock_finish(void *ctx, turbowasm_wasi_fs_file f, uint8_t direction) { (void)ctx; if (direction==1) files[f.object].target=0; }
static uint32_t mock_ready(void *ctx, turbowasm_wasi_fs_file f, uint8_t direction, turbowasm_wasi_readiness *r) {
    (void)ctx; p1_file *p=&files[f.object];
    *r=(turbowasm_wasi_readiness){0};
    r->ready=direction==TURBOWASM_WASI_EVENT_FD_WRITE || p->eof || p->accepts || p->size;
    r->bytes=p->size; if (p->eof) r->flags=TURBOWASM_WASI_EVENT_HANGUP; return 0;
}
static uint32_t mock_recv(void *ctx, turbowasm_wasi_fs_file f, const turbowasm_wasi_buffer *b, size_t count,
    uint16_t flags, bool nonblock, uint32_t *out, uint16_t *roflags) {
    (void)ctx; p1_file *p=&files[f.object]; *out=0; *roflags=0; ++p->reads;
    size_t total=0; for (size_t i=0; i<count; ++i) total+=b[i].size;
    effect(EFFECT_RECV);
    if (recv_error) return recv_error;
    if (recv_overreport) { *out=(uint32_t)total+1; return 0; }
    if (!total) return 0;
    if ((flags&3)==3 && !nonblock && p->size<total && !p->eof) { p->target=total; return TURBOWASM_WASI_ERRNO_AGAIN; }
    if (!p->size) return p->eof ? 0 : TURBOWASM_WASI_ERRNO_AGAIN;
    size_t pos=0;
    for (size_t i=0; i<count && pos<p->size; ++i) {
        size_t n=b[i].size; if (n>p->size-pos) n=p->size-pos;
        memcpy(b[i].data,p->bytes+pos,n); pos+=n;
    }
    *out=(uint32_t)pos;
    if (!(flags&1)) { p->size-=pos; memmove(p->bytes,p->bytes+pos,p->size); }
    return 0;
}
static uint32_t mock_read(void *ctx, turbowasm_wasi_fs_file f, const turbowasm_wasi_buffer *b, size_t n, uint32_t *out) {
    uint16_t flags; return mock_recv(ctx,f,b,n,0,true,out,&flags);
}
static uint32_t mock_write(void *ctx, turbowasm_wasi_fs_file f, const turbowasm_wasi_const_buffer *b, size_t n, uint32_t *out) {
    (void)ctx; ++files[f.object].writes; *out=0; effect(EFFECT_SEND);
    sent_count=0;
    for (size_t i=0; i<n; ++i) {
        check(b[i].size <= sizeof(sent)-sent_count);
        memcpy(sent+sent_count,b[i].data,b[i].size); sent_count+=b[i].size;
        *out+=(uint32_t)b[i].size;
    }
    if (send_again) { send_again=false; *out=0; return TURBOWASM_WASI_ERRNO_AGAIN; }
    return 0;
}
static uint32_t mock_stat(void *ctx, turbowasm_wasi_fs_file f, turbowasm_wasi_fs_stat *stat) {
    (void)ctx; *stat=(turbowasm_wasi_fs_stat){0};
    stat->file_type=f.object==1 ? TURBOWASM_WASI_FILETYPE_REGULAR_FILE : TURBOWASM_WASI_FILETYPE_SOCKET_STREAM;
    stat->size=files[f.object].size; return 0;
}
static uint32_t mock_shutdown(void *ctx, turbowasm_wasi_fs_file f, uint8_t how) { (void)ctx; (void)how; files[f.object].eof=true; return 0; }
static uint32_t mock_accept(void *ctx, turbowasm_wasi_fs_file f, turbowasm_wasi_fs_file *out) {
    (void)ctx; effect(EFFECT_ACCEPT); if (!files[f.object].accepts) return TURBOWASM_WASI_ERRNO_AGAIN;
    --files[f.object].accepts; files[4].open=true; *out=(turbowasm_wasi_fs_file){4,1}; return 0;
}
static uint32_t mock_clock(void *ctx, uint32_t id, uint64_t precision, uint64_t *out) {
    (void)ctx; (void)id; effect(EFFECT_CLOCK); last_precision=precision; *out=now; return 0;
}
static void bind_socket(uint32_t fd, unsigned object, uint64_t base, uint64_t inherit) {
    turbowasm_wasi_fs_file file={object,1}; turbowasm_wasi_fs_descriptor d={0}; files[object].open=true;
    check_equal(turbowasm_wasi_fs_bind_socket_move(&fs,fd,&ops,&file,TURBOWASM_WASI_FILETYPE_SOCKET_STREAM,0,base,inherit,&d),TURBOWASM_OK);
    check_equal(file.generation,0u);
}
static uint32_t recv_call(uint32_t flags) { uint32_t a[]={4,0,1,flags,128,132}; return p1_call(&guest,P1_RECV,a,6); }
static void recv_start(uint32_t flags) { uint32_t a[]={4,0,1,flags,128,132}; p1_start(&guest,&execution,P1_RECV,a,6); }
static void data(unsigned object, const char *s, size_t n) { memcpy(files[object].bytes+files[object].size,s,n); files[object].size+=n; }
static uint32_t poll_call(uint32_t count) { uint32_t a[]={1024,2048,count,128}; return p1_call(&guest,P1_POLL,a,4); }
static void poll_start(uint32_t count) { uint32_t a[]={1024,2048,count,128}; p1_start(&guest,&execution,P1_POLL,a,4); }
static uint32_t rights_call(uint32_t fd, uint64_t base, uint64_t inherit) {
    turbowasm_value a[]={p1_i32(fd),p1_i64(base),p1_i64(inherit)};
    return (uint32_t)p1_invoke(&guest,P1_WRAPPER_BASE+P1_RIGHTS,a,3).as.i32;
}
static void fixture_init(void) {
    memset(files,0,sizeof(files)); now=100; last_precision=0;
    effect_callback=effect_flags=effects=0; sent_count=0; send_again=false; recv_error=0; recv_overreport=false;
    ops=(turbowasm_wasi_descriptor_ops){0}; ops.size=sizeof(ops); ops.api_version=1;
    ops.file=(turbowasm_wasi_fs_provider){0}; ops.file.close=mock_close; ops.file.read=mock_read;
    ops.file.write=mock_write; ops.file.stat=mock_stat; ops.retain=mock_retain; ops.release=mock_release;
    ops.ready=mock_ready; ops.accept=mock_accept; ops.recv=mock_recv; ops.send=mock_write;
    ops.shutdown=mock_shutdown; ops.finish=mock_finish;
    turbowasm_wasi_fs_config c={8,ops.file}; check_equal(turbowasm_wasi_fs_init(&fs,&c),TURBOWASM_OK);
    turbowasm_wasi_fs_descriptor d; files[1].open=true;
    check_equal(turbowasm_wasi_fs_bind_descriptor(&fs,3,(turbowasm_wasi_fs_file){1,1},false,NULL,&d),TURBOWASM_OK);
    bind_socket(4,2,UINT64_MAX,UINT64_MAX);
    turbowasm_wasi_preview1_config_v2 v; turbowasm_wasi_preview1_config_v2_init(&v);
    v.base.filesystem=&fs; v.base.allow_fd_read=v.base.allow_fd_write=true;
    v.base.allow_clock=true; v.base.clock_time=mock_clock; v.allow_sockets=v.allow_poll=true;
    v.io_bytes=32; v.pending_bytes=4096; v.wait_capacity=1;
    check_equal(turbowasm_wasi_preview1_init_v2(&wasi,&v),TURBOWASM_OK); p1_guest_init(&guest,&wasi); p1_iovec(&guest,4);
    p1_guest_init_with_memory(&peer,&wasi,&guest);
}
static void fixture_destroy(void) {
    turbowasm_execution_destroy(&second); turbowasm_execution_destroy(&execution); p1_guest_destroy(&peer); p1_guest_destroy(&guest);
    check_equal(turbowasm_wasi_preview1_destroy_checked(&wasi),TURBOWASM_OK);
    for (uint32_t fd=0; fd<16; ++fd) { turbowasm_wasi_fs_descriptor_info info;
        if (turbowasm_wasi_fs_descriptor_info_get(&fs,fd,&info)) check_equal(turbowasm_wasi_fs_close_fd(&fs,fd),0u); }
    check_equal(turbowasm_wasi_fs_destroy(&fs),TURBOWASM_OK);
    for (unsigned i=0; i<8; ++i) { check_equal(files[i].leases,0u); check_false(files[i].open); }
}

suite("Preview1 descriptor and async socket ABI") {
    before_each() { fixture_init(); }
    after_each() { fixture_destroy(); }
    it("reports exact fdstat, allows only rights reduction and validates flag widths") {
        uint32_t a[]={4,256}; check_equal(p1_call(&guest,P1_STAT,a,2),0u);
        check_equal(p1_load(&guest,256)&255u,TURBOWASM_WASI_FILETYPE_SOCKET_STREAM);
        check_equal(p1_load64(&guest,264),UINT64_MAX);
        a[1]=4; check_equal(p1_call(&guest,P1_FLAGS,a,2),0u);
        a[1]=256; check_equal(p1_call(&guest,P1_STAT,a,2),0u); check_equal((p1_load(&guest,256)>>16)&65535u,4u);
        a[1]=65536; check_equal(p1_call(&guest,P1_FLAGS,a,2),TURBOWASM_WASI_ERRNO_INVAL);
        a[1]=1; check_equal(p1_call(&guest,P1_FLAGS,a,2),TURBOWASM_WASI_ERRNO_NOTSUP);
        check_equal(rights_call(4,TURBOWASM_WASI_RIGHT_FD_READ,0),0u);
        check_equal(rights_call(4,UINT64_MAX,0),TURBOWASM_WASI_ERRNO_NOTCAPABLE);
        uint32_t send[]={4,0,1,0,128}; check_equal(p1_call(&guest,P1_SEND,send,5),TURBOWASM_WASI_ERRNO_NOTCAPABLE);
        check_equal(files[2].writes,0u);
    }
    it("validates every iovec and output before consuming bytes") {
        data(2,"ping",4); uint32_t a[]={4,0,1,0,65535,132};
        check_equal(p1_call(&guest,P1_RECV,a,6),TURBOWASM_WASI_ERRNO_FAULT); check_equal(files[2].reads,0u);
        a[4]=128; a[2]=2; p1_store(&guest,8,65535); p1_store(&guest,12,4);
        check_equal(p1_call(&guest,P1_RECV,a,6),TURBOWASM_WASI_ERRNO_FAULT); check_equal(files[2].reads,0u);
        a[2]=1; a[3]=4; check_equal(p1_call(&guest,P1_RECV,a,6),TURBOWASM_WASI_ERRNO_INVAL);
        p1_iovec(&guest,33); check_equal(recv_call(0),TURBOWASM_WASI_ERRNO_MSGSIZE); check_equal(files[2].size,4u);
    }
    it("preserves PEEK bytes and completes WAITALL without replaying consumed prefixes") {
        data(2,"pi",2); recv_start(3); check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        check_equal(files[2].size,2u);
        p1_subscription(&guest,0,9,1,4); check_equal(poll_call(1),0u); check_equal(p1_load64(&guest,2064),2u);
        check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        data(2,"ng",2);
        check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK); check_equal(p1_execution_result(&execution),0u);
        check_equal(p1_load(&guest,512),UINT32_C(0x676e6970)); check_equal(files[2].size,4u);
        turbowasm_execution_destroy(&execution); check_equal(recv_call(0),0u); check_equal(files[2].size,0u);
        data(2,"po",2); recv_start(2); check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        check_equal(files[2].size,0u); data(2,"ng",2); check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK); check_equal(p1_execution_result(&execution),0u);
        check_equal(p1_load(&guest,512),UINT32_C(0x676e6f70)); check_equal(p1_load(&guest,128),4u);
    }
    it("unwinds cancellation, rejects busy destroy, and bounds concurrent waits") {
        recv_start(0); check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        check_equal(turbowasm_wasi_preview1_destroy_checked(&wasi),TURBOWASM_INVALID_ARGUMENT);
        p1_subscription(&guest,0,9,TURBOWASM_WASI_EVENT_CLOCK,1); p1_store(&guest,1048,200);
        uint32_t a[]={1024,2048,1,128}; p1_start(&guest,&second,P1_POLL,a,4);
        check_equal(turbowasm_execution_resume(&second,NULL),TURBOWASM_OK); check_equal(p1_execution_result(&second),TURBOWASM_WASI_ERRNO_AGAIN);
        turbowasm_execution_destroy(&execution); check_equal(files[2].leases,0u);
        data(2,"done",4); check_equal(recv_call(0),0u);
    }
    it("never redirects a parked operation to a reused guest fd") {
        recv_start(0); check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        check_equal(turbowasm_wasi_fs_close_fd(&fs,4),0u); bind_socket(4,5,UINT64_MAX,0); data(5,"new!",4);
        check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK); check_equal(p1_execution_result(&execution),TURBOWASM_WASI_ERRNO_BADF);
        check_equal(files[5].reads,0u); check_equal(files[2].closes,1u);
    }
    it("wakes on shutdown and waits for callback cleanup") {
        recv_start(0); check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        check_equal(turbowasm_wasi_preview1_shutdown_request(&wasi),TURBOWASM_OK);
        bool done=true; check_equal(turbowasm_wasi_preview1_shutdown_poll(&wasi,&done),TURBOWASM_OK); check_false(done);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK); check_equal(p1_execution_result(&execution),TURBOWASM_WASI_ERRNO_INTR);
        check_equal(turbowasm_wasi_preview1_shutdown_poll(&wasi,&done),TURBOWASM_OK); check_true(done);
    }
    it("returns duplicate fd events and per-event bad-fd errors with original userdata") {
        data(2,"ping",4); p1_subscription(&guest,0,UINT64_C(0x1234567800000001),1,4);
        p1_subscription(&guest,1,2,1,4); p1_subscription(&guest,2,3,1,99); p1_subscription(&guest,3,4,1,3);
        check_equal(poll_call(4),0u); check_equal(p1_load(&guest,128),4u);
        check_equal(p1_load64(&guest,2048),UINT64_C(0x1234567800000001)); check_equal(p1_load64(&guest,2064),4u);
        check_equal(p1_load64(&guest,2080),2u); check_equal(p1_load(&guest,2120)&65535u,TURBOWASM_WASI_ERRNO_BADF);
        check_equal(files[2].reads,0u); check_equal(files[2].size,4u);
    }
    it("holds clock deadlines across suspension and reports checked overflow") {
        p1_subscription(&guest,0,7,0,1); p1_store(&guest,1048,25); p1_store(&guest,1056,3);
        poll_start(1); check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        uint64_t timeout=0; check_equal(turbowasm_wasi_preview1_next_timeout(&wasi,&timeout),TURBOWASM_OK); check_equal(timeout,25u);
        now=124; check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        now=125; check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK); check_equal(p1_execution_result(&execution),0u);
        check_equal(p1_load(&guest,128),1u); check_equal(last_precision,3u); turbowasm_execution_destroy(&execution);
        p1_store(&guest,1048,UINT32_MAX); p1_store(&guest,1052,UINT32_MAX);
        check_equal(poll_call(1),0u); check_equal(p1_load(&guest,2056)&65535u,TURBOWASM_WASI_ERRNO_INVAL);
        p1_store(&guest,1064,1); check_equal(poll_call(0),TURBOWASM_WASI_ERRNO_INVAL);
        check_equal(poll_call(65),TURBOWASM_WASI_ERRNO_INVAL);
        p1_store(&guest,1048,100); p1_store(&guest,1052,0);
        check_equal(poll_call(1),0u); check_equal(p1_load(&guest,2056)&65535u,0u);
    }
    it("bounds poll storage and joins timers with socket readiness") {
        check_equal(poll_call(64),TURBOWASM_WASI_ERRNO_NOMEM); check_equal(files[2].leases,0u);
        p1_subscription(&guest,0,1,0,1); p1_store(&guest,1048,20);
        p1_subscription(&guest,1,2,1,4); poll_start(2);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        now=120; data(2,"ping",4); check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK); check_equal(p1_execution_result(&execution),0u);
        check_equal(p1_load(&guest,128),2u); check_equal(p1_load64(&guest,2048),1u); check_equal(p1_load64(&guest,2080),2u);
    }
    it("returns a capability error if rights are removed during a parked read") {
        recv_start(0); check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        check_equal(rights_call(4,0,0),0u); data(2,"ping",4);
        check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK);
        check_equal(p1_execution_result(&execution),TURBOWASM_WASI_ERRNO_NOTCAPABLE); check_equal(files[2].size,4u);
    }
    it("publishes accept without allocation and keeps children independent") {
        bind_socket(5,3,TURBOWASM_WASI_RIGHT_SOCK_ACCEPT,UINT64_MAX); files[3].accepts=1;
        uint32_t a[]={5,4,128}; check_equal(p1_call(&guest,P1_ACCEPT,a,3),0u); uint32_t fd=p1_load(&guest,128);
        check_equal(fd,6u); check_equal(turbowasm_wasi_fs_close_fd(&fs,5),0u); check_true(files[4].open);
        a[0]=fd; a[1]=256; check_equal(p1_call(&guest,P1_STAT,a,2),0u);
        check_equal(p1_load64(&guest,264)&TURBOWASM_WASI_RIGHT_SOCK_ACCEPT,0u); check_equal(p1_load64(&guest,272),0u);
        check_equal((p1_load(&guest,256)>>16)&65535u,4u);
    }
    it("applies inheriting-rights reduction made while accept is parked") {
        bind_socket(5,3,TURBOWASM_WASI_RIGHT_SOCK_ACCEPT,UINT64_MAX);
        uint32_t a[]={5,0,128}; p1_start(&guest,&execution,P1_ACCEPT,a,3);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        check_equal(rights_call(5,TURBOWASM_WASI_RIGHT_SOCK_ACCEPT,TURBOWASM_WASI_RIGHT_FD_READ),0u);
        files[3].accepts=1; check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK); check_equal(p1_execution_result(&execution),0u);
        turbowasm_wasi_fs_descriptor_info info; check_true(turbowasm_wasi_fs_descriptor_info_get(&fs,p1_load(&guest,128),&info));
        check_equal(info.rights_base,TURBOWASM_WASI_RIGHT_FD_READ);
    }
    it("releases staged buffers after cancellation and reacquires memory after growth") {
        data(2,"pi",2); recv_start(2); check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        turbowasm_value pages=p1_i32(1); check_equal(p1_invoke(&guest,P1_GROW,&pages,1).as.i32,1);
        data(2,"ng",2); check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK); check_equal(p1_execution_result(&execution),0u);
        check_equal(p1_load(&guest,512),UINT32_C(0x676e6970));
    }
    it("uses the common fd table for ordinary read/write and obeys NONBLOCK") {
        uint32_t a[]={4,4}; check_equal(p1_call(&guest,P1_FLAGS,a,2),0u); check_equal(recv_call(0),TURBOWASM_WASI_ERRNO_AGAIN);
        data(2,"sock",4); uint32_t read[]={4,0,1,128}; check_equal(p1_call(&guest,P1_READ,read,4),0u);
        check_equal(p1_load(&guest,512),UINT32_C(0x6b636f73)); check_equal(files[2].size,0u);
        read[0]=3; data(1,"file",4); check_equal(p1_call(&guest,P1_READ,read,4),0u); check_equal(files[1].size,0u);
        check_equal(p1_call(&guest,P1_WRITE,read,4),0u); check_equal(files[1].writes,1u);
    }
    it("keeps send addresses valid when retain grows memory and changes the iovec") {
        p1_store(&guest,512,UINT32_C(0x676e6970));
        effect_callback=EFFECT_RETAIN; effect_flags=GROW_MEMORY|MUTATE_IOV;
        uint32_t a[]={4,0,1,0,65532};
        check_equal(p1_call(&guest,P1_SEND,a,5),0u);
        check_equal(effects,1u); check_equal(sent_count,(size_t)4);
        check(memcmp(sent,"ping",4)==0); check_equal(p1_load(&guest,65532),4u);
    }
    it("retains staged send bytes across provider mutation and suspension") {
        p1_store(&guest,512,UINT32_C(0x676e6970));
        effect_callback=EFFECT_SEND; effect_flags=GROW_MEMORY|MUTATE_PAYLOAD|MUTATE_IOV;
        send_again=true; uint32_t a[]={4,0,1,0,128};
        p1_start(&guest,&execution,P1_SEND,a,5);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        check_equal(effects,1u); check_equal(p1_load(&guest,512),0u);
        check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK);
        check_equal(p1_execution_result(&execution),0u);
        check_equal(sent_count,(size_t)4); check(memcmp(sent,"ping",4)==0);
        check_equal(files[2].writes,2u); check_equal(p1_load(&guest,128),4u);
    }
    it("scatters a short receive to saved ranges after callback memory growth") {
        p1_store(&guest,0,65532); p1_store(&guest,4,4);
        p1_store(&guest,8,512); p1_store(&guest,12,4);
        p1_store(&guest,512,UINT32_C(0xa5a5a5a5)); p1_store(&guest,700,UINT32_C(0xa5a5a5a5));
        data(2,"pingxy",6); effect_callback=EFFECT_RECV; effect_flags=GROW_MEMORY|MUTATE_IOV;
        uint32_t a[]={4,0,2,0,128,132};
        check_equal(p1_call(&guest,P1_RECV,a,6),0u); check_equal(effects,1u);
        check_equal(p1_load(&guest,65532),UINT32_C(0x676e6970));
        check_equal(p1_load(&guest,512),UINT32_C(0xa5a57978));
        check_equal(p1_load(&guest,700),UINT32_C(0xa5a5a5a5)); check_equal(p1_load(&guest,128),6u);
    }
    it("keeps receive destinations and publication fixed while parked") {
        recv_start(0); check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_YIELDED);
        p1_iovec(&peer,1); p1_store(&peer,0,700); p1_store(&peer,700,UINT32_C(0xa5a5a5a5));
        data(2,"ping",4); check_equal(turbowasm_wasi_preview1_advance(&wasi),TURBOWASM_OK);
        check_equal(turbowasm_execution_resume(&execution,NULL),TURBOWASM_OK);
        check_equal(p1_execution_result(&execution),0u);
        check_equal(p1_load(&guest,512),UINT32_C(0x676e6970));
        check_equal(p1_load(&guest,700),UINT32_C(0xa5a5a5a5)); check_equal(p1_load(&guest,128),4u);
    }
    it("preserves receive outputs on provider failure and over-reporting") {
        p1_store(&guest,512,UINT32_C(0xa5a5a5a5));
        p1_store(&guest,128,UINT32_C(0xa5a5a5a5)); p1_store(&guest,132,UINT32_C(0xa5a5a5a5));
        recv_error=TURBOWASM_WASI_ERRNO_IO; check_equal(recv_call(0),recv_error);
        recv_error=0; recv_overreport=true; check_equal(recv_call(0),TURBOWASM_WASI_ERRNO_IO);
        check_equal(p1_load(&guest,512),UINT32_C(0xa5a5a5a5));
        check_equal(p1_load(&guest,128),UINT32_C(0xa5a5a5a5)); check_equal(p1_load(&guest,132),UINT32_C(0xa5a5a5a5));
    }
    it("publishes accept after provider growth and rejects invalid output before effects") {
        bind_socket(5,3,TURBOWASM_WASI_RIGHT_SOCK_ACCEPT,UINT64_MAX); files[3].accepts=1;
        effect_callback=EFFECT_ACCEPT; effect_flags=GROW_MEMORY;
        uint32_t a[]={5,0,65533}; check_equal(p1_call(&guest,P1_ACCEPT,a,3),TURBOWASM_WASI_ERRNO_FAULT);
        check_equal(effects,0u); check_equal(files[3].accepts,1u);
        a[2]=65532; check_equal(p1_call(&guest,P1_ACCEPT,a,3),0u); check_equal(effects,1u);
        check_equal(p1_load(&guest,65532),6u); check_true(files[4].open);
    }
    it("decodes all poll arguments before clock callbacks mutate or grow memory") {
        p1_subscription(&guest,0,1,TURBOWASM_WASI_EVENT_CLOCK,1);
        p1_subscription(&guest,1,2,TURBOWASM_WASI_EVENT_FD_READ,4); data(2,"ping",4);
        effect_callback=EFFECT_CLOCK; effect_flags=GROW_MEMORY|MUTATE_POLL;
        uint32_t a[]={1024,65536-64,2,128}; check_equal(p1_call(&guest,P1_POLL,a,4),0u);
        check_equal(effects,1u); check_equal(p1_load(&guest,128),2u);
        check_equal(p1_load64(&guest,65536-64),UINT64_C(1));
        check_equal(p1_load64(&guest,65536-32),UINT64_C(2));
        check_equal(p1_load(&guest,65536-24)&65535u,0u);
        check_equal(p1_load64(&guest,65536-16),UINT64_C(4));
    }
    it("validates the whole poll vector and outputs before retaining or querying") {
        p1_subscription(&guest,0,1,TURBOWASM_WASI_EVENT_CLOCK,1);
        p1_subscription(&guest,1,2,255,4);
        effect_callback=EFFECT_CLOCK; effect_flags=GROW_MEMORY;
        uint32_t a[]={1024,2048,2,128};
        check_equal(p1_call(&guest,P1_POLL,a,4),TURBOWASM_WASI_ERRNO_INVAL);
        check_equal(effects,0u); check_equal(files[2].leases,0u);
        p1_subscription(&guest,1,2,TURBOWASM_WASI_EVENT_FD_READ,4);
        a[1]=65536-63; check_equal(p1_call(&guest,P1_POLL,a,4),TURBOWASM_WASI_ERRNO_FAULT);
        check_equal(effects,0u); a[1]=2048; a[3]=65533;
        check_equal(p1_call(&guest,P1_POLL,a,4),TURBOWASM_WASI_ERRNO_FAULT); check_equal(effects,0u);
    }
}
