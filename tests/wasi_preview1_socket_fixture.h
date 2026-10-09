#ifndef TW_P1_SOCKET_TEST_FIXTURE_H
#define TW_P1_SOCKET_TEST_FIXTURE_H
#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi_sockets.h>
#include <tinytest.h>
#include "fixtures/wasi_preview1_sockets.h"
#if defined(TW_P1_MIR)
#include "../src/instance_internal.h"
#include "../src/jit/mir_backend.h"
#endif

enum { P1_ACCEPT, P1_RECV, P1_SEND, P1_SHUTDOWN, P1_STAT, P1_FLAGS, P1_RIGHTS,
    P1_POLL, P1_READ, P1_WRITE, P1_CLOSE, P1_WRAPPER_BASE=11, P1_STORE32=22,
    P1_LOAD32, P1_LOAD8, P1_LOAD16, P1_LOAD64, P1_GROW };
typedef struct p1_guest { turbowasm_module module; turbowasm_instance instance; } p1_guest;
static turbowasm_value p1_i32(uint32_t x) {
    turbowasm_value v={0}; v.kind=TURBOWASM_VALUE_I32; v.as.i32=(int32_t)x; return v;
}
static turbowasm_value p1_i64(uint64_t x) {
    turbowasm_value v={0}; v.kind=TURBOWASM_VALUE_I64; v.as.i64=(int64_t)x; return v;
}
static void p1_guest_init_with_memory(p1_guest *g, turbowasm_wasi_preview1 *wasi, p1_guest *owner) {
    turbowasm_linker linker={0};
#if defined(TW_P1_SHARED)
    const uint8_t *bytes=owner ? tw_p1_socket_module_shared_peer : tw_p1_socket_module_shared;
    size_t length=owner ? sizeof(tw_p1_socket_module_shared_peer) : sizeof(tw_p1_socket_module_shared);
#else
    const uint8_t *bytes=owner ? tw_p1_socket_module_peer : tw_p1_socket_module;
    size_t length=owner ? sizeof(tw_p1_socket_module_peer) : sizeof(tw_p1_socket_module);
#endif
    check_equal(turbowasm_module_load_borrowed(&g->module,bytes,length),TURBOWASM_OK);
    check_equal(turbowasm_linker_init(&linker),TURBOWASM_OK);
    if (owner) {
        turbowasm_name name={(const uint8_t *)"p1",2};
        check_equal(turbowasm_linker_define_instance(&linker,name,&owner->instance),TURBOWASM_OK);
    }
    check_equal(turbowasm_wasi_preview1_define(wasi,&linker),TURBOWASM_OK);
    check_equal(turbowasm_instance_create_linked(&g->instance,&g->module,&linker),TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);
#if defined(TW_P1_MIR)
    turbowasm_jit_backend backend={0};
    check_equal(turbowasm_mir_backend_create(&backend),TURBOWASM_OK);
    check_equal(turbowasm_jit_instance_attach_backend(g->instance.impl,&backend,1),TURBOWASM_OK);
#endif
}
static void p1_guest_init(p1_guest *g, turbowasm_wasi_preview1 *wasi) {
    p1_guest_init_with_memory(g,wasi,NULL);
}
static void p1_guest_destroy(p1_guest *g) {
#if defined(TW_P1_MIR)
    if (g->instance.impl) {
        turbowasm_instance_impl *p=g->instance.impl;
        for (unsigned i=P1_WRAPPER_BASE; i<P1_STORE32; ++i) {
            if (p->jit_functions[i].call_count)
                check_equal(p->jit_functions[i].state,TURBOWASM_JIT_COMPILED);
        }
    }
#endif
    turbowasm_instance_destroy(&g->instance); turbowasm_module_destroy(&g->module);
}
static turbowasm_value p1_invoke(p1_guest *g, uint32_t index, const turbowasm_value *args, size_t count) {
    turbowasm_value result={0}; size_t results=0; turbowasm_trap trap=TURBOWASM_TRAP_NONE;
    check_equal(turbowasm_instance_invoke(&g->instance,index,args,count,&result,1,&results,&trap),TURBOWASM_OK);
    check_equal(trap,TURBOWASM_TRAP_NONE); return result;
}
static uint32_t p1_call(p1_guest *g, unsigned import, const uint32_t *values, size_t n) {
    turbowasm_value args[6]={{0}};
    for (size_t i=0; i<n; ++i) args[i]=p1_i32(values[i]);
    return (uint32_t)p1_invoke(g,P1_WRAPPER_BASE+import,args,n).as.i32;
}
static void p1_store(p1_guest *g, uint32_t address, uint32_t x) {
    turbowasm_value args[]={p1_i32(address),p1_i32(x)}; (void)p1_invoke(g,P1_STORE32,args,2);
}
static uint32_t p1_load(p1_guest *g, uint32_t address) {
    turbowasm_value arg=p1_i32(address); return (uint32_t)p1_invoke(g,P1_LOAD32,&arg,1).as.i32;
}
static uint64_t p1_load64(p1_guest *g, uint32_t address) {
    turbowasm_value arg=p1_i32(address); return (uint64_t)p1_invoke(g,P1_LOAD64,&arg,1).as.i64;
}
static void p1_iovec(p1_guest *g, uint32_t length) { p1_store(g,0,512); p1_store(g,4,length); }
static void p1_subscription(p1_guest *g, unsigned index, uint64_t userdata, uint8_t type, uint32_t fd) {
    uint32_t address=1024+index*48;
    for (unsigned i=0; i<48; i+=4) p1_store(g,address+i,0);
    p1_store(g,address,(uint32_t)userdata); p1_store(g,address+4,(uint32_t)(userdata>>32));
    p1_store(g,address+8,type); p1_store(g,address+16,fd);
}
static void p1_start(p1_guest *g, turbowasm_execution *execution, unsigned import, const uint32_t *values, size_t n) {
    turbowasm_value args[6]={{0}}; for (size_t i=0; i<n; ++i) args[i]=p1_i32(values[i]);
    check_equal(turbowasm_execution_create(execution,&g->instance,P1_WRAPPER_BASE+import,args,n),TURBOWASM_OK);
}
static uint32_t p1_execution_result(turbowasm_execution *execution) {
    check_equal(turbowasm_execution_result_count(execution),1u);
    const turbowasm_value *v=turbowasm_execution_result_at(execution,0); check_true(v != NULL);
    return v ? (uint32_t)v->as.i32 : UINT32_MAX;
}
#endif
