#ifndef TURBOWASM_DATABASE_GUEST_TEST_H
#define TURBOWASM_DATABASE_GUEST_TEST_H

#include "database_orm.h"
#include "fixtures/database_bridge.h"
#include <core/clock.h>
#include <threads/threads.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { SUBMIT=8,POLL,AWAIT,RECEIPT,READ,TAKE,CANCEL,DROP,PUT,GET };

typedef struct wake_signal {sync_mutex_t mutex;sync_cond_t cond;bool ready;} wake_signal;
static inline void notify(void *context) {
    wake_signal *s=context;
    sync_mutex_lock(&s->mutex);
    s->ready=true;
    sync_cond_signal(&s->cond);
    sync_mutex_unlock(&s->mutex);
}
static inline void deliver(tw_db_worker *worker,wake_signal *s) {
    cflow_waitable wait=tw_db_worker_waitable(worker);
    assert(cflow_waitable_arm(&wait,(cflow_waker){notify,s}));
    const uint64_t deadline=hrtime()+UINT64_C(10000000000);
    sync_mutex_lock(&s->mutex);
    while(!s->ready) {
        uint64_t now=hrtime();
        assert(now<deadline);
        assert(sync_cond_timedwait(&s->cond,&s->mutex,deadline-now)==SALTS_OK);
    }
    s->ready=false;
    sync_mutex_unlock(&s->mutex);
    assert(tw_db_worker_progress(worker)==TW_DB_OK);
}

static inline turbowasm_value i32(uint32_t n) {
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I32,.as.i32=(int32_t)n};
}
static inline turbowasm_value i64(uint64_t n) {
    int64_t bits;memcpy(&bits,&n,sizeof bits);
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I64,.as.i64=bits};
}
static inline void call(turbowasm_instance *i,uint32_t fn,const turbowasm_value *args,
    size_t n,turbowasm_value *result,size_t count) {
    size_t actual=0;turbowasm_trap trap=TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(i,fn,args,n,result,count,&actual,&trap)==TURBOWASM_OK);
    assert(actual==count&&trap==TURBOWASM_TRAP_NONE);
}
static inline uint64_t submit(turbowasm_instance *i,uint32_t op,uint64_t owner,
    const uint8_t *bytes,size_t size,tw_db_status expected) {
    for(size_t j=0;j<size;++j) {
        turbowasm_value a[]={i32(8192+(uint32_t)j),i32(bytes[j])};
        call(i,PUT,a,2,NULL,0);
    }
    turbowasm_value a[]={i32(op),i64(owner),i64(8192),i64(size)},r[2];
    call(i,SUBMIT,a,4,r,2);
    assert(r[0].as.i32==(int32_t)expected);
    assert((r[1].as.i64!=0)==(expected==TW_DB_OK));
    return (uint64_t)r[1].as.i64;
}
static inline void single(turbowasm_instance *i,uint32_t fn,uint64_t op,tw_db_status expected) {
    turbowasm_value a=i64(op),r;
    call(i,fn,&a,1,&r,1);
    assert(r.as.i32==(int32_t)expected);
}
static inline void receipt(turbowasm_instance *i,uint64_t op,tw_db_outcome outcome,size_t size) {
    turbowasm_value a=i64(op),r[3];
    call(i,RECEIPT,&a,1,r,3);
    if(r[0].as.i32!=TW_DB_OK||r[1].as.i32!=(int32_t)outcome||
        (uint64_t)r[2].as.i64!=size)
        fprintf(stderr,"DB receipt op=%" PRIu64 " status=%d outcome=%d bytes=%" PRIu64
            " expected status=%d outcome=%d bytes=%zu\n",op,
            (int)r[0].as.i32,(int)r[1].as.i32,(uint64_t)r[2].as.i64,
            (int)TW_DB_OK,(int)outcome,size);
    assert(r[0].as.i32==TW_DB_OK&&r[1].as.i32==(int32_t)outcome&&
        (uint64_t)r[2].as.i64==size);
}
static inline void check_bytes(turbowasm_instance *i,uint64_t op,
    const uint8_t *expected,size_t size) {
    receipt(i,op,TW_DB_SUCCESS,size);
    /* Read the same immutable operation result in two passes, including
       a nonzero offset. This never advances the provider cursor. */
    size_t split=size/2;
    for(unsigned pass=0;pass<2;++pass) {
        size_t offset=pass?split:0,n=pass?size-split:split;
        turbowasm_value args[]={i64(op),i64(offset),i64(1000+offset),i64(n)},r[2];
        call(i,READ,args,4,r,2);
        assert(r[0].as.i32==TW_DB_OK&&(uint64_t)r[1].as.i64==n);
    }
    for(size_t j=0;j<size;++j) {
        turbowasm_value a=i32(1000+(uint32_t)j),v;
        call(i,GET,&a,1,&v,1);assert(v.as.i32==expected[j]);
    }
}
static inline void trap_read(turbowasm_instance *i,uint64_t op,size_t size) {
    turbowasm_value a[]={i64(op),i64(0),i64(65535),i64(size)},r[2];
    size_t count=0;turbowasm_trap trap=TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(i,READ,a,4,r,2,&count,&trap)==TURBOWASM_TRAPPED);
    assert(trap==TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
}
static inline uint64_t take(turbowasm_instance *i,uint64_t op) {
    turbowasm_value a=i64(op),r[2];call(i,TAKE,&a,1,r,2);
    assert(r[0].as.i32==TW_DB_OK&&r[1].as.i64);
    single(i,DROP,op,TW_DB_OK);
    return (uint64_t)r[1].as.i64;
}
static inline void settle(tw_db_bridge *bridge,tw_db_worker *worker,wake_signal *wake,
    turbowasm_instance *instance,uint64_t op) {
    turbowasm_execution execution={0};
    turbowasm_value a=i64(op);
    assert(turbowasm_execution_create(&execution,instance,AWAIT,&a,1)==TURBOWASM_OK);
    /* A native result is never published before owner-side mailbox progress. */
    assert(tw_db_bridge_resume(bridge,&execution,NULL)==TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(&execution)==TURBOWASM_YIELD_HOST_WAIT);
    deliver(worker,wake);
    assert(tw_db_bridge_wake(bridge,&execution)==TW_DB_OK);
    assert(tw_db_bridge_resume(bridge,&execution,NULL)==TURBOWASM_OK);
    assert(turbowasm_execution_result_count(&execution)==2);
    assert(turbowasm_execution_result_at(&execution,0)->as.i32==TW_DB_OK);
    assert(turbowasm_execution_result_at(&execution,1)->as.i32==TW_DB_TERMINAL);
    assert(tw_db_bridge_destroy_execution(bridge,&execution)==TW_DB_OK);
}

static inline void finish_ok(tw_db_bridge *bridge,tw_db_worker *worker,wake_signal *wake,
    turbowasm_instance *instance,uint64_t owner,tw_db_opcode code) {
    uint64_t op=submit(instance,code,owner,NULL,0,TW_DB_OK);
    settle(bridge,worker,wake,instance,op);
    receipt(instance,op,TW_DB_SUCCESS,0);single(instance,DROP,op,TW_DB_OK);
}


#endif
