#include "database_lifecycle.h"
#include "runtime_alloc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t mask[]={TW_DB_MASK(TW_DB_I64)};
static const tw_db_template templates[]={
    {"item.get",TW_DB_MASK(TW_DB_QUERY),mask,1},
    {"item.put",TW_DB_MASK(TW_DB_EXECUTE),mask,1}
};
static const tw_db_alias aliases[]={
    {"local",TW_DB_SQL,TW_DB_MASK(TW_DB_ISOLATION_DEFAULT),0,templates,2},
    {"cache",TW_DB_REDIS,0,TW_DB_MASK(TW_DB_GET),NULL,0}
};
static const tw_db_policy policy={{128,24,64,4,3,16},aliases,2};
static const tw_db_capacity cap={2,1,1,4,64,16384};
static const uint8_t local[]={5,0,0,0,'l','o','c','a','l'};
static const uint8_t cache[]={5,0,0,0,'c','a','c','h','e'};
static const uint8_t put[]={8,0,0,0,'i','t','e','m','.','p','u','t',
    1,0,0,0,TW_DB_I64,8,0,0,0,1,0,0,0,0,0,0,0};
static const uint8_t get[]={8,0,0,0,'i','t','e','m','.','g','e','t',
    1,0,0,0,TW_DB_I64,8,0,0,0,1,0,0,0,0,0,0,0};
static const uint8_t zero[]={0,0,0,0},one[]={1,0,0,0};
static const uint8_t binary[]={0,0xff,0x61,0};
static const uint8_t redis_get[]={TW_DB_GET,0,0,0,1,0,0,0,1,0,0,0,'k'};

static uint64_t submit(tw_db_manager *m,uint64_t owner,uint32_t code,
    const uint8_t *b,size_t n) {
    uint64_t token=0;
    assert(tw_db_manager_submit(m,owner,code,b,n,&token)==TW_DB_OK);
    assert(token!=0);
    return token;
}

static void reject(tw_db_manager *m,uint64_t owner,uint32_t code,
    const uint8_t *b,size_t n,tw_db_status expected) {
    uint64_t token=UINT64_C(987654321);
    assert(tw_db_manager_submit(m,owner,code,b,n,&token)==expected);
    assert(token==UINT64_C(987654321));
}

static tw_db_work start(tw_db_manager *m,uint64_t op,uintptr_t owner) {
    tw_db_work work={0};
    assert(tw_db_manager_start(m,op,&work)==TW_DB_OK);
    assert(work.operation==op && work.native_owner==owner);
    assert(tw_db_manager_start(m,op,&work)==TW_DB_WRONG_STATE);
    return work;
}

static void complete(tw_db_manager *m,uint64_t op,tw_db_outcome outcome,
    uintptr_t native,const uint8_t *b,size_t n) {
    assert(tw_db_manager_complete(m,op,outcome,native,b,n)==TW_DB_OK);
    assert(tw_db_manager_complete(m,op,outcome,native,b,n)==TW_DB_WRONG_STATE);
    tw_db_receipt receipt={0};
    assert(tw_db_manager_receipt(m,op,&receipt)==TW_DB_OK);
    assert(receipt.outcome==outcome && receipt.size==n);
}

static uint64_t take(tw_db_manager *m,uint64_t op) {
    uint64_t resource=0;
    assert(tw_db_manager_drop(m,op)==TW_DB_BUSY);
    assert(tw_db_manager_take(m,op,&resource)==TW_DB_OK);
    assert(resource!=0 && resource!=op);
    assert(tw_db_manager_take(m,op,&resource)==TW_DB_WRONG_STATE);
    assert(tw_db_manager_drop(m,op)==TW_DB_OK);
    return resource;
}

static uint64_t open_sql(tw_db_manager *m,uintptr_t native) {
    uint64_t op=submit(m,0,TW_DB_OPEN_SQL,local,sizeof local);
    tw_db_work w=start(m,op,0);
    assert(w.request.alias_index==0);
    complete(m,op,TW_DB_SUCCESS,native,NULL,0);
    return take(m,op);
}

static void close_resource(tw_db_manager *m,uint64_t resource,uintptr_t native) {
    uint64_t op=submit(m,resource,TW_DB_CLOSE,NULL,0);
    start(m,op,native);
    complete(m,op,TW_DB_SUCCESS,0,NULL,0);
    assert(tw_db_manager_drop(m,op)==TW_DB_OK);
    reject(m,resource,TW_DB_CLOSE,NULL,0,TW_DB_INVALID_HANDLE);
}

static void reservation_and_receipts(void) {
    tw_db_manager m={0};
    assert(tw_db_manager_init(&m,&policy,&cap)==TW_DB_OK);
    uint64_t op=submit(&m,0,TW_DB_OPEN_SQL,local,sizeof local);
    uint64_t second=submit(&m,0,TW_DB_OPEN_REDIS,cache,sizeof cache);
    reject(&m,0,TW_DB_OPEN_SQL,local,sizeof local,TW_DB_LIMIT);
    assert(tw_db_manager_destroy(&m)==TW_DB_BUSY);
    tw_db_receipt receipt={0};
    assert(tw_db_manager_receipt(&m,op,&receipt)==TW_DB_PENDING);
    assert(tw_db_manager_complete(&m,op,TW_DB_SUCCESS,10,NULL,0)==TW_DB_WRONG_STATE);
    bool notify=true;
    assert(tw_db_manager_cancel(&m,second,&notify)==TW_DB_OK && !notify);
    assert(tw_db_manager_start(&m,second,&(tw_db_work){0})==TW_DB_WRONG_STATE);
    assert(tw_db_manager_receipt(&m,second,&receipt)==TW_DB_OK &&
        receipt.outcome==TW_DB_CANCELLED_NO_EFFECT);
    assert(tw_db_manager_drop(&m,second)==TW_DB_OK);
    reject(&m,second,TW_DB_CLOSE,NULL,0,TW_DB_INVALID_HANDLE);
    start(&m,op,0);
    assert(tw_db_manager_complete(&m,op,TW_DB_SUCCESS,0,NULL,0)==TW_DB_INVALID_REQUEST);
    complete(&m,op,TW_DB_SUCCESS,10,NULL,0);
    uint64_t sql=take(&m,op);
    assert(tw_db_manager_complete(&m,op,TW_DB_SUCCESS,10,NULL,0)==TW_DB_INVALID_HANDLE);

    uint8_t request[sizeof put];
    memcpy(request,put,sizeof request);
    op=submit(&m,sql,TW_DB_EXECUTE,request,sizeof request);
    memset(request,0,sizeof request);
    reject(&m,sql,TW_DB_EXECUTE,put,sizeof put,TW_DB_BUSY);
    tw_db_work work=start(&m,op,10);
    assert(work.request.template_index==1 && work.request.argument_count==1);
    tw_db_reader reader={work.request.arguments.data,work.request.arguments.size};
    tw_db_value v;
    assert(tw_db_read_value(&reader,64,&v) && v.kind==TW_DB_I64 &&
           v.payload.data[0]==1 && reader.remaining==0);
    assert(tw_db_manager_drop(&m,op)==TW_DB_BUSY);
    assert(tw_db_manager_complete(&m,op,TW_DB_SUCCESS,7,NULL,0)==TW_DB_INVALID_REQUEST);
    uint8_t large[65]={0};
    assert(tw_db_manager_complete(&m,op,TW_DB_SUCCESS,0,large,sizeof large)==TW_DB_LIMIT);
    uint8_t result[sizeof binary]; memcpy(result,binary,sizeof result);
    complete(&m,op,TW_DB_SUCCESS,0,result,sizeof result);
    memset(result,0,sizeof result);
    size_t copied=123;
    assert(tw_db_manager_read(&m,op,5,result,sizeof result,&copied)==TW_DB_INVALID_REQUEST);
    assert(copied==123);
    assert(tw_db_manager_read(&m,op,0,NULL,1,&copied)==TW_DB_INVALID_REQUEST);
    assert(tw_db_manager_read(&m,op,1,result,2,&copied)==TW_DB_OK &&
        copied==2 && memcmp(result,binary+1,2)==0);
    assert(tw_db_manager_read(&m,op,0,result,sizeof result,&copied)==TW_DB_OK &&
        copied==sizeof result && memcmp(result,binary,sizeof result)==0);
    assert(tw_db_manager_read(&m,op,sizeof result,NULL,0,&copied)==TW_DB_OK&&copied==0);
    assert(tw_db_manager_drop(&m,op)==TW_DB_OK);
    close_resource(&m,sql,10);
    assert(tw_db_manager_destroy(&m)==TW_DB_OK && !m.impl);
}

static void dependencies_and_quarantine(void) {
    tw_db_manager m={0};
    assert(tw_db_manager_init(&m,&policy,&cap)==TW_DB_OK);
    uint64_t sql=open_sql(&m,100);
    uint64_t op=submit(&m,sql,TW_DB_BEGIN,zero,4);
    reject(&m,sql,TW_DB_CLOSE,NULL,0,TW_DB_BUSY);
    start(&m,op,100); complete(&m,op,TW_DB_SUCCESS,200,NULL,0);
    uint64_t tx=take(&m,op);
    reject(&m,sql,TW_DB_EXECUTE,put,sizeof put,TW_DB_BUSY);
    op=submit(&m,tx,TW_DB_QUERY,get,sizeof get);
    start(&m,op,200); complete(&m,op,TW_DB_SUCCESS,300,NULL,0);
    uint64_t cursor=take(&m,op);
    reject(&m,tx,TW_DB_COMMIT,NULL,0,TW_DB_BUSY);
    reject(&m,cursor,TW_DB_EXECUTE,put,sizeof put,TW_DB_INVALID_HANDLE);
    op=submit(&m,cursor,TW_DB_FETCH,one,4);
    start(&m,op,300);
    bool notify=false;
    assert(tw_db_manager_cancel(&m,op,&notify)==TW_DB_OK&&notify);
    assert(tw_db_manager_cancel(&m,op,&notify)==TW_DB_OK&&!notify);
    tw_db_operation_state state=0;
    assert(tw_db_manager_poll(&m,op,&state)==TW_DB_OK&&state==TW_DB_CANCEL_REQUESTED);
    assert(tw_db_manager_destroy(&m)==TW_DB_BUSY);
    complete(&m,op,TW_DB_OUTCOME_UNKNOWN,0,NULL,0);
    assert(tw_db_manager_drop(&m,op)==TW_DB_OK);
    reject(&m,cursor,TW_DB_FETCH,one,4,TW_DB_WRONG_STATE);
    close_resource(&m,cursor,300);
    reject(&m,tx,TW_DB_COMMIT,NULL,0,TW_DB_WRONG_STATE);
    close_resource(&m,tx,200);
    reject(&m,sql,TW_DB_EXECUTE,put,sizeof put,TW_DB_WRONG_STATE);
    close_resource(&m,sql,100);

    sql=open_sql(&m,101);
    op=submit(&m,sql,TW_DB_BEGIN,zero,4);
    start(&m,op,101); complete(&m,op,TW_DB_SUCCESS,201,NULL,0);
    tx=take(&m,op);
    op=submit(&m,tx,TW_DB_COMMIT,NULL,0);
    start(&m,op,201); complete(&m,op,TW_DB_FAILED_NO_EFFECT,0,NULL,0);
    assert(tw_db_manager_drop(&m,op)==TW_DB_OK);
    op=submit(&m,tx,TW_DB_ROLLBACK,NULL,0);
    start(&m,op,201); complete(&m,op,TW_DB_SUCCESS,0,NULL,0);
    assert(tw_db_manager_drop(&m,op)==TW_DB_OK);
    reject(&m,tx,TW_DB_CLOSE,NULL,0,TW_DB_INVALID_HANDLE);
    close_resource(&m,sql,101);
    assert(tw_db_manager_destroy(&m)==TW_DB_OK);
}

static void drain_and_quota(void) {
    tw_db_manager m={0};
    tw_db_capacity limit=cap; limit.operations=1;limit.connections=1;
    assert(tw_db_manager_init(&m,&policy,&limit)==TW_DB_OK);
    uint64_t op=submit(&m,0,TW_DB_OPEN_REDIS,cache,sizeof cache);
    reject(&m,0,TW_DB_OPEN_SQL,local,sizeof local,TW_DB_LIMIT);
    start(&m,op,0); complete(&m,op,TW_DB_SUCCESS,900,NULL,0);
    uintptr_t native=0;
    assert(tw_db_manager_untaken(&m,op,&native)==TW_DB_OK&&native==900);
    assert(tw_db_manager_drop(&m,op)==TW_DB_BUSY);
    assert(tw_db_manager_destroy(&m)==TW_DB_BUSY);
    /* Deterministic provider confirms the untaken lease is now drained. */
    assert(tw_db_manager_disposed(&m,op)==TW_DB_OK);
    assert(tw_db_manager_disposed(&m,op)==TW_DB_WRONG_STATE);
    assert(tw_db_manager_drop(&m,op)==TW_DB_OK);
    uint64_t previous=op;
    op=submit(&m,0,TW_DB_OPEN_REDIS,cache,sizeof cache);
    assert(op!=previous);
    assert(tw_db_manager_complete(&m,previous,TW_DB_SUCCESS,900,NULL,0)==TW_DB_INVALID_HANDLE);
    start(&m,op,0);complete(&m,op,TW_DB_SUCCESS,901,NULL,0);
    uint64_t redis=take(&m,op);
    op=submit(&m,redis,TW_DB_REDIS_COMMAND,redis_get,sizeof redis_get);
    start(&m,op,901);complete(&m,op,TW_DB_SUCCESS,0,binary,sizeof binary);
    assert(tw_db_manager_drop(&m,op)==TW_DB_OK);
    tw_db_manager_stop(&m);
    reject(&m,redis,TW_DB_REDIS_COMMAND,redis_get,sizeof redis_get,TW_DB_WRONG_STATE);
    close_resource(&m,redis,901);
    assert(tw_db_manager_destroy(&m)==TW_DB_OK);
}

static void owned_policy(void) {
    tw_db_manager m={0};
    char a[]="local", t[]="item.put"; uint32_t pm[]={TW_DB_MASK(TW_DB_I64)};
    tw_db_template table[]={ {t,TW_DB_MASK(TW_DB_EXECUTE),pm,1} };
    tw_db_alias original[]={ {a,TW_DB_SQL,0,0,table,1} };
    tw_db_policy p={policy.limits,original,1};
    assert(tw_db_manager_init(&m,&p,&cap)==TW_DB_OK);
    a[0]='x';t[0]='x';pm[0]=0;original[0].kind=TW_DB_REDIS;
    uint64_t sql=open_sql(&m,55);
    uint64_t op=submit(&m,sql,TW_DB_EXECUTE,put,sizeof put);
    start(&m,op,55);complete(&m,op,TW_DB_SUCCESS,0,NULL,0);
    assert(tw_db_manager_drop(&m,op)==TW_DB_OK);
    close_resource(&m,sql,55);
    assert(tw_db_manager_destroy(&m)==TW_DB_OK);
    assert(tw_db_manager_init(&m,&p,&cap)==TW_DB_INVALID_REQUEST && !m.impl);
    tw_db_alias duplicate[]={aliases[0],aliases[0]};
    p=(tw_db_policy){policy.limits,duplicate,2};
    assert(tw_db_manager_init(&m,&p,&cap)==TW_DB_INVALID_REQUEST && !m.impl);
    tw_db_capacity tiny=cap;tiny.host_bytes=1;
    assert(tw_db_manager_init(&m,&policy,&tiny)==TW_DB_LIMIT&&!m.impl);
    tiny=cap;tiny.operations=UINT32_MAX;tiny.result_bytes=SIZE_MAX;
    assert(tw_db_manager_init(&m,&policy,&tiny)==TW_DB_LIMIT&&!m.impl);
    tiny=cap;tiny.connections=0;
    assert(tw_db_manager_init(&m,&policy,&tiny)==TW_DB_OK);
    reject(&m,0,TW_DB_OPEN_SQL,local,sizeof local,TW_DB_LIMIT);
    assert(tw_db_manager_destroy(&m)==TW_DB_OK);
}

typedef struct allocation_probe { size_t attempts,live,fail; } allocation_probe;
static void *allocate(void *ctx,size_t n) {
    allocation_probe *p=ctx;
    if(++p->attempts==p->fail)return NULL;
    void *v=malloc(n);if(v)++p->live;return v;
}
static void deallocate(void *ctx,void *ptr) {
    allocation_probe *p=ctx;
    assert(p->live>0);--p->live;free(ptr);
}
static void allocation_faults(void) {
    for(size_t fail=1;fail<=10;++fail) {
        allocation_probe p={.fail=fail};
        turbowasm_runtime_config config={0};
        config.allocator.context=&p;
        config.allocator.allocate=allocate;
        config.allocator.deallocate=deallocate;
        turbowasm_runtime_scope scope=turbowasm_runtime_scope_enter(&config);
        tw_db_manager m={0};
        tw_db_status s=tw_db_manager_init(&m,&policy,&cap);
        if(s==TW_DB_OK) {
            uint64_t sql=open_sql(&m,99);
            close_resource(&m,sql,99);
            assert(tw_db_manager_destroy(&m)==TW_DB_OK);
        } else {
            assert(s==TW_DB_OUT_OF_MEMORY&&!m.impl);
        }
        turbowasm_runtime_scope_leave(scope);
        assert(p.live==0);
    }
    allocation_probe p={0};
    turbowasm_runtime_config config={0};
    config.allocator.context=&p;config.allocator.allocate=allocate;config.allocator.deallocate=deallocate;
    turbowasm_runtime_scope scope=turbowasm_runtime_scope_enter(&config);
    tw_db_manager m={0};
    assert(tw_db_manager_init(&m,&policy,&cap)==TW_DB_OK);
    size_t allocations=p.attempts;
    p.fail=allocations+1;
    uint64_t sql=open_sql(&m,9);
    close_resource(&m,sql,9);
    assert(tw_db_manager_destroy(&m)==TW_DB_OK);
    turbowasm_runtime_scope_leave(scope);
    assert(p.attempts==allocations&&p.live==0);
}
int main(void) {
    reservation_and_receipts();
    dependencies_and_quarantine();
    drain_and_quota();
    owned_policy();
    allocation_faults();
    return 0;
}
