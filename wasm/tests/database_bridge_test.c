#include "database_bridge.h"
#include "fixtures/database_bridge.h"
#ifdef TW_DB_TEST_MIR
#include "instance_internal.h"
#include "jit/mir_backend.h"
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static const uint32_t masks[]={TW_DB_MASK(TW_DB_I64)};
static const tw_db_template templates[]={
    {"item.put",TW_DB_MASK(TW_DB_EXECUTE),masks,1},
    {"item.get",TW_DB_MASK(TW_DB_QUERY),masks,1}
};
static const tw_db_alias aliases[]={{"local",TW_DB_SQL,TW_DB_MASK(TW_DB_ISOLATION_DEFAULT),0,templates,2}};
static const tw_db_policy policy={{128,24,64,4,3,16},aliases,1};
static const tw_db_capacity capacity={2,1,1,4,64,32768};
static const uint8_t open_local[]={5,0,0,0,'l','o','c','a','l'};
static const uint8_t open_denied[]={6,0,0,0,'s','e','c','r','e','t'};
static const uint8_t put[]={8,0,0,0,'i','t','e','m','.','p','u','t',1,0,0,0,
    TW_DB_I64,8,0,0,0,7,0,0,0,0,0,0,0};
static const uint8_t get[]={8,0,0,0,'i','t','e','m','.','g','e','t',1,0,0,0,
    TW_DB_I64,8,0,0,0,7,0,0,0,0,0,0,0};
static const uint8_t zero[]={0,0,0,0},one[]={1,0,0,0};
static const uint8_t binary[]={0,0xff,0x61,0};
enum { SUBMIT=8,POLL,AWAIT,RECEIPT,READ,TAKE,CANCEL,DROP,PUT,GET };

typedef struct probe {
    unsigned starts,cancels;
    bool defer;
    tw_db_work work;
    uintptr_t next_native,closed[16];
    tw_db_kind closed_kind[16];
    unsigned closes;
} probe;

static void dispatch_start(void *context,tw_db_bridge *bridge,const tw_db_work *work) {
    probe *p=context;
    ++p->starts;
    assert(!p->work.operation);
    if(work->request.opcode==TW_DB_CLOSE) {
        assert(p->closes<16&&work->native_owner&&work->native_kind!=TW_DB_NONE);
        p->closed[p->closes]=work->native_owner;
        p->closed_kind[p->closes++]=work->native_kind;
    }
    if(p->defer) { p->work=*work;return; }
    bool creates=work->request.opcode==TW_DB_OPEN_SQL||work->request.opcode==TW_DB_OPEN_REDIS||
        work->request.opcode==TW_DB_BEGIN||work->request.opcode==TW_DB_QUERY;
    uintptr_t native=creates?++p->next_native:0;
    assert(tw_db_bridge_complete(bridge,work->operation,TW_DB_SUCCESS,native,NULL,0)==TW_DB_OK);
}

static void dispatch_cancel(void *context,tw_db_bridge *bridge,uint64_t operation) {
    probe *p=context;
    (void)bridge;
    assert(p->work.operation==operation);
    ++p->cancels;
}

static void finish(probe *p,tw_db_bridge *bridge,tw_db_outcome outcome,
    uintptr_t native,const uint8_t *bytes,size_t size) {
    uint64_t operation=p->work.operation;
    assert(operation);
    assert(tw_db_bridge_complete(bridge,operation,outcome,native,bytes,size)==TW_DB_OK);
    p->work=(tw_db_work){0};
    assert(tw_db_bridge_complete(bridge,operation,outcome,native,bytes,size)==TW_DB_WRONG_STATE);
}

static turbowasm_value i32(uint32_t n) {
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I32,.as.i32=(int32_t)n};
}
static turbowasm_value i64(uint64_t n) {
    int64_t bits;memcpy(&bits,&n,sizeof bits);
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I64,.as.i64=bits};
}
static uint64_t u64(turbowasm_value value) {
    assert(value.kind==TURBOWASM_VALUE_I64);
    return (uint64_t)value.as.i64;
}
static void attach(turbowasm_instance *i) {
#ifdef TW_DB_TEST_MIR
    turbowasm_jit_backend backend={0};
    assert(turbowasm_mir_backend_create(&backend)==TURBOWASM_OK);
    assert(turbowasm_jit_instance_attach_backend(i->impl,&backend,1)==TURBOWASM_OK);
#else
    (void)i;
#endif
}
static void call(turbowasm_instance *i,unsigned function,const turbowasm_value *args,
    size_t argc,turbowasm_value *out,size_t count) {
    size_t actual=0;turbowasm_trap trap=TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(i,function,args,argc,out,count,&actual,&trap)==TURBOWASM_OK);
    assert(trap==TURBOWASM_TRAP_NONE && actual==count);
}
static void store(turbowasm_instance *i,const uint8_t *bytes,size_t size) {
    for(size_t j=0;j<size;++j) {
        turbowasm_value a[]={i32(8192+(uint32_t)j),i32(bytes[j])};
        call(i,PUT,a,2,NULL,0);
    }
}
static uint64_t submit(turbowasm_instance *i,uint32_t opcode,uint64_t owner,
    const uint8_t *bytes,size_t size,tw_db_status expected) {
    store(i,bytes,size);
    turbowasm_value a[]={i32(opcode),i64(owner),i64(8192),i64(size)},r[2];
    call(i,SUBMIT,a,4,r,2);
    assert(r[0].kind==TURBOWASM_VALUE_I32&&r[0].as.i32==(int32_t)expected);
    uint64_t token=u64(r[1]);
    assert((expected==TW_DB_OK)==(token!=0));
    return token;
}
static void state(turbowasm_instance *i,unsigned function,uint64_t token,
    tw_db_status expected,uint32_t value) {
    turbowasm_value a=i64(token),r[2];
    call(i,function,&a,1,r,2);
    assert(r[0].kind==TURBOWASM_VALUE_I32&&r[0].as.i32==(int32_t)expected);
    assert(r[1].kind==TURBOWASM_VALUE_I32&&(uint32_t)r[1].as.i32==value);
}
static void single(turbowasm_instance *i,unsigned function,uint64_t token,tw_db_status expected) {
    turbowasm_value a=i64(token),r;
    call(i,function,&a,1,&r,1);
    assert(r.kind==TURBOWASM_VALUE_I32&&r.as.i32==(int32_t)expected);
}
static uint64_t take(turbowasm_instance *i,uint64_t op,tw_db_status expected) {
    turbowasm_value a=i64(op),r[2];call(i,TAKE,&a,1,r,2);
    assert(r[0].as.i32==(int32_t)expected);
    assert((expected==TW_DB_OK)==(u64(r[1])!=0));return u64(r[1]);
}
static void receipt(turbowasm_instance *i,uint64_t op,tw_db_status expected,
    uint32_t outcome,uint64_t size) {
    turbowasm_value a=i64(op),r[3];call(i,RECEIPT,&a,1,r,3);
    assert(r[0].as.i32==(int32_t)expected&&r[1].as.i32==(int32_t)outcome&&u64(r[2])==size);
}
static void read_result(turbowasm_instance *i,uint64_t op,uint64_t offset,
    uint64_t address,uint64_t size,tw_db_status expected,uint64_t copied) {
    turbowasm_value a[]={i64(op),i64(offset),i64(address),i64(size)},r[2];
    call(i,READ,a,4,r,2);
    assert(r[0].as.i32==(int32_t)expected&&u64(r[1])==copied);
}
static void trap_call(turbowasm_instance *i,unsigned function,const turbowasm_value *a,size_t argc) {
    turbowasm_value r[2];size_t count=0;turbowasm_trap trap=TURBOWASM_TRAP_NONE;
    assert(turbowasm_instance_invoke(i,function,a,argc,r,2,&count,&trap)==TURBOWASM_TRAPPED);
    assert(trap==TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
}
static void start_wait(tw_db_bridge *bridge,turbowasm_instance *i,
    turbowasm_execution *execution,uint64_t op) {
    turbowasm_value a=i64(op);
    assert(turbowasm_execution_create(execution,i,AWAIT,&a,1)==TURBOWASM_OK);
    assert(tw_db_bridge_resume(bridge,execution,NULL)==TURBOWASM_YIELDED);
    assert(turbowasm_execution_yield_reason_get(execution)==TURBOWASM_YIELD_HOST_WAIT);
    assert(tw_db_bridge_wake(bridge,execution)==TW_DB_PENDING);
}
static void end_wait(tw_db_bridge *bridge,turbowasm_execution *execution) {
    assert(tw_db_bridge_wake(bridge,execution)==TW_DB_OK);
    assert(tw_db_bridge_wake(bridge,execution)==TW_DB_WRONG_STATE);
    assert(tw_db_bridge_resume(bridge,execution,NULL)==TURBOWASM_OK);
    assert(turbowasm_execution_result_count(execution)==2);
    assert(turbowasm_execution_result_at(execution,0)->as.i32==TW_DB_OK);
    assert(turbowasm_execution_result_at(execution,1)->as.i32==TW_DB_TERMINAL);
    assert(tw_db_bridge_destroy_execution(bridge,execution)==TW_DB_OK);
}
static void test_bridge(void) {
    turbowasm_module module={0};
    turbowasm_instance first={0},peer={0},missing={0};
    turbowasm_linker linker={0};
    tw_db_bridge bridge={0};
    probe p={.next_native=100};
    const tw_db_dispatch dispatch={&p,dispatch_start,dispatch_cancel};
    assert(tw_db_bridge_init(&bridge,&policy,&capacity,&dispatch)==TW_DB_OK);
    assert(turbowasm_module_load_borrowed(&module,database_bridge_bytes,
        sizeof database_bridge_bytes)==TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker)==TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(&missing,&module,&linker)==TURBOWASM_LINK_ERROR);
    assert(!missing.impl);
    assert(tw_db_bridge_define(&bridge,&linker)==TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(&first,&module,&linker)==TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(&peer,&module,&linker)==TURBOWASM_OK);
    attach(&first);attach(&peer);
    submit(&first,TW_DB_OPEN_SQL,0,open_local,sizeof open_local,TW_DB_DENIED);
    assert(p.starts==0);
    assert(tw_db_bridge_bind(&bridge,&first)==TW_DB_OK);
    assert(tw_db_bridge_bind(&bridge,&peer)==TW_DB_WRONG_STATE);
    assert(tw_db_bridge_destroy(&bridge)==TW_DB_BUSY);
    submit(&first,TW_DB_OPEN_SQL,0,open_denied,sizeof open_denied,TW_DB_DENIED);
    turbowasm_value bad[]={i32(TW_DB_OPEN_SQL),i64(0),i64(65535),i64(9)};
    trap_call(&first,SUBMIT,bad,4);
    assert(p.starts==0);
    uint64_t op=submit(&first,TW_DB_OPEN_SQL,0,open_local,sizeof open_local,TW_DB_OK);
    assert(p.starts==1);
    state(&first,AWAIT,op,TW_DB_OK,TW_DB_TERMINAL); /* completion before await */
    receipt(&first,op,TW_DB_OK,TW_DB_SUCCESS,0);
    submit(&peer,TW_DB_OPEN_SQL,0,open_local,sizeof open_local,TW_DB_DENIED);
    state(&peer,POLL,op,TW_DB_DENIED,0);
    state(&peer,AWAIT,op,TW_DB_DENIED,0);
    receipt(&peer,op,TW_DB_DENIED,0,0);
    read_result(&peer,op,0,UINT64_MAX,32,TW_DB_DENIED,0);
    take(&peer,op,TW_DB_DENIED);
    single(&peer,CANCEL,op,TW_DB_DENIED);single(&peer,DROP,op,TW_DB_DENIED);
    uint64_t sql=take(&first,op,TW_DB_OK);
    take(&first,op,TW_DB_WRONG_STATE);
    single(&first,DROP,op,TW_DB_OK);
    state(&first,POLL,op,TW_DB_INVALID_HANDLE,0);
    assert(p.starts==1&&p.cancels==0);

    p.defer=true;
    op=submit(&first,TW_DB_EXECUTE,sql,put,sizeof put,TW_DB_OK);
    assert(p.starts==2&&p.work.native_owner==101);
    state(&first,POLL,op,TW_DB_OK,TW_DB_RUNNING);
    state(&first,AWAIT,op,TW_DB_UNSUPPORTED,0); /* one-shot pending await */
    receipt(&first,op,TW_DB_PENDING,0,0);
    read_result(&first,op,0,1000,4,TW_DB_PENDING,0);
    single(&first,DROP,op,TW_DB_BUSY);
    uint8_t zeros[sizeof put]={0};store(&first,zeros,sizeof zeros);
    tw_db_reader reader={p.work.request.arguments.data,p.work.request.arguments.size};
    tw_db_value value;
    assert(tw_db_read_value(&reader,64,&value)&&value.payload.data[0]==7&&reader.remaining==0);
    turbowasm_execution exec={0},other={0};
    start_wait(&bridge,&first,&exec,op);
    turbowasm_value argument=i64(op);
    assert(turbowasm_execution_create(&other,&peer,AWAIT,&argument,1)==TURBOWASM_OK);
    assert(tw_db_bridge_resume(&bridge,&other,NULL)==TURBOWASM_INVALID_ARGUMENT);
    assert(tw_db_bridge_wake(&bridge,&other)==TW_DB_WRONG_STATE);
    assert(tw_db_bridge_destroy_execution(&bridge,&other)==TW_DB_WRONG_STATE);
    turbowasm_execution_destroy(&other);
    assert(tw_db_bridge_resume(&bridge,&exec,NULL)==TURBOWASM_YIELDED);
    finish(&p,&bridge,TW_DB_SUCCESS,0,binary,sizeof binary);
    end_wait(&bridge,&exec);
    assert(p.starts==2&&p.cancels==0);
    receipt(&first,op,TW_DB_OK,TW_DB_SUCCESS,sizeof binary);
    turbowasm_value read_bad[]={i64(op),i64(0),i64(65535),i64(4)};
    trap_call(&first,READ,read_bad,4);
    read_result(&first,op,0,1200,UINT64_MAX,TW_DB_OK,sizeof binary);
    read_result(&first,op,1,1300,2,TW_DB_OK,2);
    for(size_t j=0;j<sizeof binary;++j) {
        turbowasm_value a=i32(1200+(uint32_t)j),v;
        call(&first,GET,&a,1,&v,1);assert(v.as.i32==binary[j]);
    }
    single(&first,DROP,op,TW_DB_OK);
    assert(tw_db_bridge_complete(&bridge,op,TW_DB_SUCCESS,0,NULL,0)==TW_DB_INVALID_HANDLE);

    op=submit(&first,TW_DB_EXECUTE,sql,put,sizeof put,TW_DB_OK);
    single(&first,CANCEL,op,TW_DB_OK);single(&first,CANCEL,op,TW_DB_OK);
    assert(p.cancels==1);
    state(&first,POLL,op,TW_DB_OK,TW_DB_CANCEL_REQUESTED);
    finish(&p,&bridge,TW_DB_OUTCOME_UNKNOWN,0,NULL,0);
    receipt(&first,op,TW_DB_OK,TW_DB_OUTCOME_UNKNOWN,0);
    single(&first,DROP,op,TW_DB_OK);
    submit(&first,TW_DB_EXECUTE,sql,put,sizeof put,TW_DB_WRONG_STATE);
    p.defer=false;
    op=submit(&first,TW_DB_CLOSE,sql,NULL,0,TW_DB_OK);
    single(&first,DROP,op,TW_DB_OK);
    submit(&first,TW_DB_CLOSE,sql,NULL,0,TW_DB_INVALID_HANDLE);

    p.defer=true;
    op=submit(&first,TW_DB_OPEN_SQL,0,open_local,sizeof open_local,TW_DB_OK);
    start_wait(&bridge,&first,&exec,op);
    assert(tw_db_bridge_destroy_execution(&bridge,&exec)==TW_DB_OK);
    assert(p.cancels==2&&p.work.operation==op);
    state(&first,POLL,op,TW_DB_OK,TW_DB_CANCEL_REQUESTED);
    tw_db_bridge_stop(&bridge);
    state(&first,POLL,op,TW_DB_DENIED,0);
#ifdef TW_DB_TEST_MIR
    turbowasm_instance_impl *f=first.impl;
    for(unsigned j=SUBMIT;j<=DROP;++j)
        assert(f->jit_functions[j].state==TURBOWASM_JIT_COMPILED);
#endif
    turbowasm_instance_destroy(&peer);turbowasm_instance_destroy(&first);
    turbowasm_linker_destroy(&linker);turbowasm_module_destroy(&module);
    assert(tw_db_bridge_detach(&bridge)==TW_DB_OK);
    assert(tw_db_bridge_destroy(&bridge)==TW_DB_BUSY);
    tw_db_shutdown_action action;
    assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&action.kind==TW_DB_SHUTDOWN_WAIT);
    finish(&p,&bridge,TW_DB_CANCELLED_NO_EFFECT,0,NULL,0);
    assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&
        action.kind==TW_DB_SHUTDOWN_RETIRED&&action.operation==op&&
        action.receipt.outcome==TW_DB_CANCELLED_NO_EFFECT);
    assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&action.kind==TW_DB_SHUTDOWN_DONE);
    assert(tw_db_bridge_destroy(&bridge)==TW_DB_OK&&!bridge.impl);
    assert(p.starts==5&&p.cancels==2);
}

typedef struct cleanup_fixture {
    turbowasm_module module;
    turbowasm_instance instance;
    turbowasm_linker linker;
    tw_db_bridge bridge;
    probe provider;
} cleanup_fixture;

static void cleanup_init(cleanup_fixture *f,uint32_t operations) {
    tw_db_capacity cap=capacity;cap.operations=operations;
    f->provider.next_native=100;
    const tw_db_dispatch dispatch={&f->provider,dispatch_start,dispatch_cancel};
    assert(tw_db_bridge_init(&f->bridge,&policy,&cap,&dispatch)==TW_DB_OK);
    assert(turbowasm_module_load_borrowed(&f->module,database_bridge_bytes,
        sizeof database_bridge_bytes)==TURBOWASM_OK);
    assert(turbowasm_linker_init(&f->linker)==TURBOWASM_OK);
    assert(tw_db_bridge_define(&f->bridge,&f->linker)==TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(&f->instance,&f->module,&f->linker)==TURBOWASM_OK);
    attach(&f->instance);
    assert(tw_db_bridge_bind(&f->bridge,&f->instance)==TW_DB_OK);
    assert(tw_db_bridge_shutdown_step(&f->bridge,&(tw_db_shutdown_action){0})==TW_DB_WRONG_STATE);
}

static void cleanup_detach(cleanup_fixture *f) {
    tw_db_bridge_stop(&f->bridge);
    turbowasm_instance_destroy(&f->instance);
    turbowasm_linker_destroy(&f->linker);
    turbowasm_module_destroy(&f->module);
    assert(tw_db_bridge_detach(&f->bridge)==TW_DB_OK);
}

static tw_db_shutdown_action step(cleanup_fixture *f,tw_db_shutdown_kind expected) {
    tw_db_shutdown_action a={0};
    unsigned starts=f->provider.starts,cancels=f->provider.cancels;
    assert(tw_db_bridge_shutdown_step(&f->bridge,&a)==TW_DB_OK);
    assert(a.kind==expected);
    assert(f->provider.starts==starts+(expected==TW_DB_SHUTDOWN_DISPATCH));
    assert(f->provider.cancels==cancels+(expected==TW_DB_SHUTDOWN_CANCEL));
    return a;
}
static uint64_t take_open(cleanup_fixture *f,uint32_t code,uint64_t owner,
    const uint8_t *bytes,size_t size) {
    uint64_t op=submit(&f->instance,code,owner,bytes,size,TW_DB_OK);
    uint64_t resource=take(&f->instance,op,TW_DB_OK);
    single(&f->instance,DROP,op,TW_DB_OK);
    return resource;
}

/* All operation slots are occupied by the original open result. Its cleanup
 * still progresses using the same slot and a fresh provider completion ID. */
static void test_guest_discard(void) {
    cleanup_fixture f={0};cleanup_init(&f,1);
    probe *p=&f.provider;
    uint64_t op=submit(&f.instance,TW_DB_OPEN_SQL,0,open_local,sizeof open_local,TW_DB_OK);
    p->defer=true;
    single(&f.instance,DROP,op,TW_DB_PENDING);
    assert(p->starts==2&&p->closes==1&&p->closed[0]==101&&
        p->closed_kind[0]==TW_DB_SQL&&p->work.operation!=op);
    state(&f.instance,POLL,op,TW_DB_OK,TW_DB_RUNNING);
    receipt(&f.instance,op,TW_DB_PENDING,0,0);
    take(&f.instance,op,TW_DB_PENDING);
    single(&f.instance,CANCEL,op,TW_DB_WRONG_STATE);
    single(&f.instance,DROP,op,TW_DB_PENDING);
    submit(&f.instance,TW_DB_OPEN_SQL,0,open_local,sizeof open_local,TW_DB_LIMIT);
    assert(p->starts==2&&p->cancels==0);
    assert(tw_db_bridge_complete(&f.bridge,op,TW_DB_SUCCESS,101,NULL,0)==TW_DB_INVALID_HANDLE);
    finish(p,&f.bridge,TW_DB_SUCCESS,0,NULL,0);
    state(&f.instance,POLL,op,TW_DB_OK,TW_DB_TERMINAL);
    receipt(&f.instance,op,TW_DB_OK,TW_DB_SUCCESS,0);
    take(&f.instance,op,TW_DB_WRONG_STATE);
    single(&f.instance,DROP,op,TW_DB_OK);
    single(&f.instance,DROP,op,TW_DB_INVALID_HANDLE);
    p->defer=false;
    op=submit(&f.instance,TW_DB_OPEN_SQL,0,open_local,sizeof open_local,TW_DB_OK);
    single(&f.instance,DROP,op,TW_DB_OK); /* inline close */
    assert(p->starts==4&&p->closes==2&&p->closed[1]==102);

    /* Uncertain disposal of a held child retains the lease, never publishes
       the cursor handle, and quarantines its connection. No retry on drop. */
    uint64_t sql=take_open(&f,TW_DB_OPEN_SQL,0,open_local,sizeof open_local);
    op=submit(&f.instance,TW_DB_QUERY,sql,get,sizeof get,TW_DB_OK);
    p->defer=true;
    single(&f.instance,DROP,op,TW_DB_PENDING);
    assert(p->closed_kind[2]==TW_DB_CURSOR&&p->closed[2]==104);
    finish(p,&f.bridge,TW_DB_OUTCOME_UNKNOWN,0,NULL,0);
    receipt(&f.instance,op,TW_DB_OK,TW_DB_OUTCOME_UNKNOWN,0);
    take(&f.instance,op,TW_DB_WRONG_STATE);
    single(&f.instance,DROP,op,TW_DB_WRONG_STATE);
    assert(p->closes==3);
    cleanup_detach(&f);
    tw_db_shutdown_action blocked=step(&f,TW_DB_SHUTDOWN_BLOCKED);
    assert(blocked.operation==op&&blocked.receipt.outcome==TW_DB_OUTCOME_UNKNOWN);
    step(&f,TW_DB_SHUTDOWN_BLOCKED);
    assert(tw_db_bridge_destroy(&f.bridge)==TW_DB_BUSY);
    /* This deterministic host now proves the unpublished native cursor
       is drained; no guest can supply this proof. */
    tw_db_manager *m=tw_db_bridge_cleanup_manager(&f.bridge);
    uintptr_t native=0;
    assert(m&&tw_db_manager_untaken(m,op,&native)==TW_DB_OK&&native==104);
    assert(tw_db_manager_disposed(m,op)==TW_DB_OK);
    assert(step(&f,TW_DB_SHUTDOWN_RETIRED).receipt.outcome==TW_DB_OUTCOME_UNKNOWN);
    p->defer=false;
    step(&f,TW_DB_SHUTDOWN_DISPATCH);
    assert(p->closed_kind[3]==TW_DB_SQL&&p->closed[3]==103);
    step(&f,TW_DB_SHUTDOWN_RETIRED);
    step(&f,TW_DB_SHUTDOWN_DONE);
    assert(tw_db_bridge_destroy(&f.bridge)==TW_DB_OK);
}

/* Kill a guest with a live transaction/cursor hierarchy, a terminal but
 * untaken second connection, and an in-flight business request. */
static void test_shutdown_order(void) {
    cleanup_fixture f={0};cleanup_init(&f,4);
    probe *p=&f.provider;
    uint64_t sql=take_open(&f,TW_DB_OPEN_SQL,0,open_local,sizeof open_local);
    uint64_t tx=take_open(&f,TW_DB_BEGIN,sql,zero,4);
    uint64_t cursor=take_open(&f,TW_DB_QUERY,tx,get,sizeof get);
    uint64_t held=submit(&f.instance,TW_DB_OPEN_SQL,0,open_local,sizeof open_local,TW_DB_OK);
    p->defer=true;
    uint64_t fetch=submit(&f.instance,TW_DB_FETCH,cursor,one,4,TW_DB_OK);
    cleanup_detach(&f);
    assert(step(&f,TW_DB_SHUTDOWN_CANCEL).operation==fetch);
    step(&f,TW_DB_SHUTDOWN_WAIT);
    step(&f,TW_DB_SHUTDOWN_WAIT);
    assert(p->cancels==1&&p->closes==0);
    finish(p,&f.bridge,TW_DB_OUTCOME_UNKNOWN,0,NULL,0);
    /* All pending business has drained. Close held resources first, then
       retire business receipts and close child->parent in dependency order. */
    tw_db_shutdown_action a=step(&f,TW_DB_SHUTDOWN_DISPATCH);
    assert(a.operation==held&&a.work.operation!=held&&a.work.native_owner==104);
    step(&f,TW_DB_SHUTDOWN_WAIT);
    finish(p,&f.bridge,TW_DB_SUCCESS,0,NULL,0);
    assert(step(&f,TW_DB_SHUTDOWN_RETIRED).operation==held);
    a=step(&f,TW_DB_SHUTDOWN_RETIRED);
    assert(a.operation==fetch&&a.receipt.outcome==TW_DB_OUTCOME_UNKNOWN);
    const uintptr_t expected[]={103,102,101};
    const tw_db_kind kinds[]={TW_DB_CURSOR,TW_DB_TRANSACTION,TW_DB_SQL};
    for(size_t i=0;i<3;++i) {
        a=step(&f,TW_DB_SHUTDOWN_DISPATCH);
        assert(a.work.native_owner==expected[i]&&a.work.native_kind==kinds[i]&&
            a.work.request.opcode==TW_DB_CLOSE);
        step(&f,TW_DB_SHUTDOWN_WAIT);
        finish(p,&f.bridge,TW_DB_SUCCESS,0,NULL,0);
        assert(step(&f,TW_DB_SHUTDOWN_RETIRED).receipt.outcome==TW_DB_SUCCESS);
    }
    step(&f,TW_DB_SHUTDOWN_DONE);
    assert(p->closes==4&&p->cancels==1);
    assert(tw_db_bridge_destroy(&f.bridge)==TW_DB_OK);
}

static void test_shutdown_close_failure(void) {
    cleanup_fixture f={0};cleanup_init(&f,1);
    (void)take_open(&f,TW_DB_OPEN_SQL,0,open_local,sizeof open_local);
    cleanup_detach(&f);f.provider.defer=true;
    tw_db_shutdown_action a=step(&f,TW_DB_SHUTDOWN_DISPATCH);
    tw_db_manager *m=tw_db_bridge_cleanup_manager(&f.bridge);
    assert(m&&tw_db_manager_confirm_shutdown_close(m,a.operation)==TW_DB_PENDING);
    finish(&f.provider,&f.bridge,TW_DB_FAILED_NO_EFFECT,0,NULL,0);
    assert(step(&f,TW_DB_SHUTDOWN_BLOCKED).operation==a.operation);
    assert(step(&f,TW_DB_SHUTDOWN_BLOCKED).receipt.outcome==TW_DB_FAILED_NO_EFFECT);
    assert(f.provider.closes==1&&tw_db_bridge_destroy(&f.bridge)==TW_DB_BUSY);
    assert(tw_db_manager_confirm_shutdown_close(m,a.operation+1)==TW_DB_WRONG_STATE);
    /* A trusted host explicitly establishes native drain, not a replay. */
    assert(tw_db_manager_confirm_shutdown_close(m,a.operation)==TW_DB_OK);
    assert(step(&f,TW_DB_SHUTDOWN_RETIRED).receipt.outcome==TW_DB_FAILED_NO_EFFECT);
    step(&f,TW_DB_SHUTDOWN_DONE);
    assert(f.provider.closes==1&&tw_db_bridge_destroy(&f.bridge)==TW_DB_OK);
}

int main(void) {
    test_bridge();
    test_guest_discard();
    test_shutdown_order();
    test_shutdown_close_failure();
    return 0;
}
