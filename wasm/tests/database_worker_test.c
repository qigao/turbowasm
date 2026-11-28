#include "database_worker.h"
#include "fixtures/database_bridge.h"
#include <core/clock.h>
#include <threads/threads.h>
#ifdef TW_DB_TEST_MIR
#include "instance_internal.h"
#include "jit/mir_backend.h"
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum { SUBMIT=8,POLL,AWAIT,RECEIPT,READ,TAKE,CANCEL,DROP,PUT,GET };
static const uint32_t masks[]={TW_DB_MASK(TW_DB_I64)};
static const tw_db_template templates[]={{"item.put",TW_DB_MASK(TW_DB_EXECUTE),masks,1}};
static const tw_db_alias aliases[]={{"local",TW_DB_SQL,0,0,templates,1}};
static const tw_db_policy policy={{128,24,64,4,3,16},aliases,1};
static const tw_db_capacity cap={2,1,1,4,64,32768};
static const uint8_t local[]={5,0,0,0,'l','o','c','a','l'};
static const uint8_t put[]={8,0,0,0,'i','t','e','m','.','p','u','t',1,0,0,0,
    TW_DB_I64,8,0,0,0,7,0,0,0,0,0,0,0};
static const uint8_t payload[]={0,0xff,0x61,0};

typedef struct worker_signal {
    sync_mutex_t mutex;
    sync_cond_t cond;
    bool ready;
} worker_signal;

static void signal_init(worker_signal *s) {
    sync_mutex_init(&s->mutex);
    sync_cond_init(&s->cond);
    s->ready=false;
}
static void notify(void *context) {
    worker_signal *s=context;
    sync_mutex_lock(&s->mutex);
    s->ready=true;
    sync_cond_signal(&s->cond);
    sync_mutex_unlock(&s->mutex);
}
static void wait_signal(worker_signal *s) {
    const uint64_t deadline=hrtime()+UINT64_C(10000000000);
    sync_mutex_lock(&s->mutex);
    while(!s->ready) {
        uint64_t now=hrtime();
        assert(now<deadline);
        assert(sync_cond_timedwait(&s->cond,&s->mutex,deadline-now)==SALTS_OK);
    }
    s->ready=false;
    sync_mutex_unlock(&s->mutex);
}
static void signal_destroy(worker_signal *s) {
    sync_cond_destroy(&s->cond);
    sync_mutex_destroy(&s->mutex);
}

typedef struct backend {
    const void *owner,*lane;
    worker_signal entered,release;
    bool block;
    unsigned opens,writes,closes;
    uintptr_t native;
} backend;

static tw_db_worker_result execute(void *context,const tw_db_worker_call *call,
    uint8_t *bytes,size_t capacity) {
    backend *b=context;
    const void *thread=sync_thread_current_token();
    assert(thread!=b->owner);
    if(!b->lane)b->lane=thread;
    assert(thread==b->lane);
    assert(call->deadline_ns>=hrtime());
    switch(call->work.request.opcode) {
    case TW_DB_OPEN_SQL:
        assert(!call->work.native_owner);
        ++b->opens;
        return (tw_db_worker_result){TW_DB_SUCCESS,++b->native,0};
    case TW_DB_EXECUTE: {
        assert(call->work.native_kind==TW_DB_SQL&&call->work.native_owner);
        tw_db_reader r={call->work.request.arguments.data,call->work.request.arguments.size};
        tw_db_value v;
        assert(tw_db_read_value(&r,64,&v)&&r.remaining==0&&v.payload.data[0]==7);
        ++b->writes;
        if(b->block) {
            notify(&b->entered);
            wait_signal(&b->release);
            assert(call->interrupted(call->interrupt_context));
            /* The worker entered native work: only the provider decides
               whether cancellation is no-effect. Simulate an uncertain write. */
            return (tw_db_worker_result){TW_DB_OUTCOME_UNKNOWN,0,0};
        }
        assert(capacity>=sizeof payload);
        memcpy(bytes,payload,sizeof payload);
        return (tw_db_worker_result){TW_DB_SUCCESS,0,sizeof payload};
    }
    case TW_DB_CLOSE:
        assert(call->work.native_kind==TW_DB_SQL&&call->work.native_owner);
        ++b->closes;
        return (tw_db_worker_result){TW_DB_SUCCESS,0,0};
    default:
        assert(!"unexpected operation");
        abort();
    }
}

static turbowasm_value i32(uint32_t n) {
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I32,.as.i32=(int32_t)n};
}
static turbowasm_value i64(uint64_t n) {
    int64_t bits;memcpy(&bits,&n,sizeof bits);
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I64,.as.i64=bits};
}
static void call(turbowasm_instance *i,uint32_t fn,const turbowasm_value *args,
    size_t n,turbowasm_value *result,size_t count) {
    size_t actual=0;turbowasm_trap trap=TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(i,fn,args,n,result,count,&actual,&trap)==TURBOWASM_OK);
    assert(actual==count&&trap==TURBOWASM_TRAP_NONE);
}
static uint64_t submit(turbowasm_instance *i,uint32_t op,uint64_t owner,
    const uint8_t *bytes,size_t size,tw_db_status expected) {
    for(size_t j=0;j<size;++j) {
        turbowasm_value a[]={i32(8192+(uint32_t)j),i32(bytes[j])};
        call(i,PUT,a,2,NULL,0);
    }
    turbowasm_value a[]={i32(op),i64(owner),i64(8192),i64(size)},r[2];
    call(i,SUBMIT,a,4,r,2);
    assert(r[0].as.i32==(int32_t)expected);
    return (uint64_t)r[1].as.i64;
}
static void single(turbowasm_instance *i,uint32_t fn,uint64_t op,tw_db_status expected) {
    turbowasm_value a=i64(op),r;
    call(i,fn,&a,1,&r,1);
    assert(r.as.i32==(int32_t)expected);
}
static void receipt(turbowasm_instance *i,uint64_t op,tw_db_outcome outcome,size_t size) {
    turbowasm_value a=i64(op),r[3];
    call(i,RECEIPT,&a,1,r,3);
    assert(r[0].as.i32==TW_DB_OK&&r[1].as.i32==(int32_t)outcome&&
        (uint64_t)r[2].as.i64==size);
}
static uint64_t take(turbowasm_instance *i,uint64_t op) {
    turbowasm_value a=i64(op),r[2];
    call(i,TAKE,&a,1,r,2);
    assert(r[0].as.i32==TW_DB_OK&&r[1].as.i64);
    single(i,DROP,op,TW_DB_OK);
    return (uint64_t)r[1].as.i64;
}
static void deliver(tw_db_worker *worker,worker_signal *wake) {
    cflow_waitable wait=tw_db_worker_waitable(worker);
    assert(cflow_waitable_arm(&wait,(cflow_waker){notify,wake}));
    wait_signal(wake);
    assert(tw_db_worker_progress(worker)==TW_DB_OK);
}
static void park(tw_db_bridge *bridge,turbowasm_instance *i,turbowasm_execution *e,uint64_t op) {
    turbowasm_value a=i64(op);
    assert(turbowasm_execution_create(e,i,AWAIT,&a,1)==TURBOWASM_OK);
    assert(tw_db_bridge_resume(bridge,e,NULL)==TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(e)==TURBOWASM_YIELD_HOST_WAIT);
    assert(tw_db_bridge_wake(bridge,e)==TW_DB_PENDING);
}
static void resume(tw_db_bridge *bridge,turbowasm_execution *e) {
    assert(tw_db_bridge_wake(bridge,e)==TW_DB_OK);
    assert(tw_db_bridge_resume(bridge,e,NULL)==TURBOWASM_OK);
    assert(turbowasm_execution_result_count(e)==2);
    assert(turbowasm_execution_result_at(e,0)->as.i32==TW_DB_OK);
    assert(turbowasm_execution_result_at(e,1)->as.i32==TW_DB_TERMINAL);
    assert(tw_db_bridge_destroy_execution(bridge,e)==TW_DB_OK);
}

int main(void) {
    tw_db_worker worker={0};
    tw_db_bridge bridge={0};
    turbowasm_module module={0};
    turbowasm_linker linker={0};
    turbowasm_instance instance={0};
    turbowasm_execution execution={0};
    backend b={.owner=sync_thread_current_token(),.native=100};
    worker_signal wake={0};
    signal_init(&b.entered);signal_init(&b.release);signal_init(&wake);
    tw_db_dispatch dispatch={0};
    const tw_db_worker_config config={4,64,16384,UINT64_C(30000000000),execute,&b};
    tw_db_worker_config too_small=config;too_small.host_bytes=1;
    assert(tw_db_worker_init(&worker,&bridge,&too_small,&dispatch)==TW_DB_LIMIT&&!worker.impl);
    assert(tw_db_worker_init(&worker,&bridge,&config,&dispatch)==TW_DB_OK);
    assert(tw_db_bridge_init(&bridge,&policy,&cap,&dispatch)==TW_DB_OK);
    assert(turbowasm_module_load_borrowed(&module,database_bridge_bytes,
        sizeof database_bridge_bytes)==TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker)==TURBOWASM_OK);
    assert(tw_db_bridge_define(&bridge,&linker)==TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(&instance,&module,&linker)==TURBOWASM_OK);
#ifdef TW_DB_TEST_MIR
    turbowasm_jit_backend jit={0};
    assert(turbowasm_mir_backend_create(&jit)==TURBOWASM_OK);
    assert(turbowasm_jit_instance_attach_backend(instance.impl,&jit,1)==TURBOWASM_OK);
#endif
    assert(tw_db_bridge_bind(&bridge,&instance)==TW_DB_OK);
    assert(tw_db_worker_progress(&worker)==TW_DB_PENDING);

    uint64_t op=submit(&instance,TW_DB_OPEN_SQL,0,local,sizeof local,TW_DB_OK);
    assert(op);
    park(&bridge,&instance,&execution,op);
    assert(tw_db_worker_destroy(&worker)==TW_DB_BUSY);
    deliver(&worker,&wake);
    resume(&bridge,&execution);
    receipt(&instance,op,TW_DB_SUCCESS,0);
    uint64_t first=take(&instance,op);
    op=submit(&instance,TW_DB_OPEN_SQL,0,local,sizeof local,TW_DB_OK);
    deliver(&worker,&wake);
    uint64_t second=take(&instance,op);
    assert(b.opens==2&&first!=second);

    op=submit(&instance,TW_DB_EXECUTE,first,put,sizeof put,TW_DB_OK);
    park(&bridge,&instance,&execution,op);
    deliver(&worker,&wake);resume(&bridge,&execution);
    receipt(&instance,op,TW_DB_SUCCESS,sizeof payload);
    turbowasm_value read[]={i64(op),i64(0),i64(1000),i64(sizeof payload)},r[2];
    call(&instance,READ,read,4,r,2);
    assert(r[0].as.i32==TW_DB_OK&&r[1].as.i64==sizeof payload);
    for(size_t j=0;j<sizeof payload;++j) {
        turbowasm_value a=i32(1000+(uint32_t)j),v;
        call(&instance,GET,&a,1,&v,1);
        assert(v.as.i32==payload[j]);
    }
    single(&instance,DROP,op,TW_DB_OK);
    assert(b.writes==1);

    /* Hold one native call, enqueue another, then cancel both before release.
       The queued operation must never enter the native callback. */
    b.block=true;
    uint64_t running=submit(&instance,TW_DB_EXECUTE,first,put,sizeof put,TW_DB_OK);
    wait_signal(&b.entered);
    uint64_t queued=submit(&instance,TW_DB_EXECUTE,second,put,sizeof put,TW_DB_OK);
    assert(running&&queued&&running!=queued);
    single(&instance,CANCEL,queued,TW_DB_OK);
    single(&instance,CANCEL,running,TW_DB_OK);
    single(&instance,CANCEL,running,TW_DB_OK);
    park(&bridge,&instance,&execution,running);
    assert(tw_db_worker_progress(&worker)==TW_DB_PENDING);
    notify(&b.release);
    deliver(&worker,&wake);resume(&bridge,&execution);
    receipt(&instance,running,TW_DB_OUTCOME_UNKNOWN,0);
    deliver(&worker,&wake);
    receipt(&instance,queued,TW_DB_CANCELLED_NO_EFFECT,0);
    assert(b.writes==2);
    single(&instance,DROP,running,TW_DB_OK);
    single(&instance,DROP,queued,TW_DB_OK);
    assert(submit(&instance,TW_DB_EXECUTE,first,put,sizeof put,TW_DB_WRONG_STATE)==0);
#ifdef TW_DB_TEST_MIR
    turbowasm_instance_impl *impl=instance.impl;
    assert(impl->jit_functions[SUBMIT].state==TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[AWAIT].state==TURBOWASM_JIT_COMPILED);
    assert(impl->jit_functions[READ].state==TURBOWASM_JIT_COMPILED);
#endif
    tw_db_bridge_stop(&bridge);
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    assert(tw_db_bridge_detach(&bridge)==TW_DB_OK);
    tw_db_shutdown_action action;
    for(unsigned j=0;j<2;++j) {
        assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&
            action.kind==TW_DB_SHUTDOWN_DISPATCH);
        assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&
            action.kind==TW_DB_SHUTDOWN_WAIT);
        deliver(&worker,&wake);
        assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&
            action.kind==TW_DB_SHUTDOWN_RETIRED&&action.receipt.outcome==TW_DB_SUCCESS);
    }
    assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&action.kind==TW_DB_SHUTDOWN_DONE);
    assert(b.closes==2&&tw_db_worker_progress(&worker)==TW_DB_PENDING);
    cflow_waitable wait=tw_db_worker_waitable(&worker);cflow_waitable_cancel(&wait);
    assert(tw_db_worker_destroy(&worker)==TW_DB_OK&&!worker.impl);
    assert(tw_db_bridge_destroy(&bridge)==TW_DB_OK&&!bridge.impl);
    signal_destroy(&wake);signal_destroy(&b.release);signal_destroy(&b.entered);
    return 0;
}
