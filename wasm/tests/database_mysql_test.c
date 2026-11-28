#include "database_guest_test.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef TW_DB_TEST_MIR
#include "instance_internal.h"
#include "jit/mir_backend.h"
#define TEST_TABLE "items_mir"
#else
#define TEST_TABLE "items_interp"
#endif

static const uint32_t masks[]={TW_DB_MASK(TW_DB_I64),TW_DB_MASK(TW_DB_BLOB)};
static const tw_db_template writable[]={
    {"item.put",TW_DB_MASK(TW_DB_EXECUTE),masks,2},
    {"item.get",TW_DB_MASK(TW_DB_QUERY),masks,1}
};
static const tw_db_template readable[]={
    {"item.get",TW_DB_MASK(TW_DB_QUERY),masks,1}
};
static const tw_db_alias aliases[]={
    {"mysql",TW_DB_SQL,TW_DB_MASK(TW_DB_ISOLATION_READ_COMMITTED),0,writable,2},
    {"readonly",TW_DB_SQL,0,0,readable,1},
    {"bad.tls",TW_DB_SQL,0,0,NULL,0}
};
static const tw_db_policy policy={{128,24,64,4,3,16},aliases,3};
static const tw_db_capacity cap={2,1,1,4,128,32768};
static const uint8_t mysql_alias[]={5,0,0,0,'m','y','s','q','l'};
static const uint8_t readonly_alias[]={8,0,0,0,'r','e','a','d','o','n','l','y'};
static const uint8_t wrong_tls[]={7,0,0,0,'b','a','d','.','t','l','s'};
static const uint8_t put[]={8,0,0,0,'i','t','e','m','.','p','u','t',2,0,0,0,
    TW_DB_I64,8,0,0,0,7,0,0,0,0,0,0,0,TW_DB_BLOB,4,0,0,0,0,0xff,0x61,0};
static const uint8_t get[]={8,0,0,0,'i','t','e','m','.','g','e','t',1,0,0,0,
    TW_DB_I64,8,0,0,0,7,0,0,0,0,0,0,0};
static const uint8_t isolation[]={TW_DB_ISOLATION_READ_COMMITTED,0,0,0};
static const uint8_t default_isolation[]={0,0,0,0};
static const uint8_t one_row[]={1,0,0,0};
static const uint8_t metadata[]={3,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,
    2,0,0,0,'i','d',7,0,0,0,'p','a','y','l','o','a','d',
    1,0,0,0,'n'};
static const uint8_t empty_metadata[]={3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const uint8_t row[]={1,0,0,0,1,0,0,0,3,0,0,0,
    TW_DB_I64,8,0,0,0,7,0,0,0,0,0,0,0,TW_DB_BLOB,4,0,0,0,0,0xff,0x61,0,
    TW_DB_NULL,0,0,0,0};
static const uint8_t eof[]={0,0,0,0,1,0,0,0};
static const uint8_t affected[]={1,0,0,0,0,0,0,0};

typedef struct fixture {
    tw_db_orm provider;
    tw_db_worker worker;
    tw_db_bridge bridge;
    turbowasm_module module;
    turbowasm_linker linker;
    turbowasm_instance instance;
    wake_signal wake;
} fixture;

static const char *required(const char *name) {
    const char *value=getenv(name);
    if(!value||!*value) {
        fprintf(stderr,"Missing required live MySQL test setting: %s\n",name);
        abort();
    }
    return value;
}
static uint64_t issue(fixture *f,tw_db_opcode code,uint64_t owner,
    const uint8_t *bytes,size_t size) {
    uint64_t op=submit(&f->instance,code,owner,bytes,size,TW_DB_OK);
    fprintf(stderr,"MySQL %s opcode=%u op=%" PRIu64 " owner=%" PRIu64 "\n",
        TEST_TABLE,(unsigned)code,op,owner);
    settle(&f->bridge,&f->worker,&f->wake,&f->instance,op);
    return op;
}
static uint64_t open_alias(fixture *f,const uint8_t *bytes,size_t size) {
    uint64_t op=issue(f,TW_DB_OPEN_SQL,0,bytes,size);
    receipt(&f->instance,op,TW_DB_SUCCESS,0);
    return take(&f->instance,op);
}
static uint64_t begin(fixture *f,uint64_t db) {
    uint64_t op=issue(f,TW_DB_BEGIN,db,isolation,sizeof isolation);
    receipt(&f->instance,op,TW_DB_SUCCESS,0);return take(&f->instance,op);
}
static void command(fixture *f,uint64_t owner,uint8_t id) {
    uint8_t request[sizeof put];memcpy(request,put,sizeof put);request[21]=id;
    uint64_t op=issue(f,TW_DB_EXECUTE,owner,request,sizeof request);
    check_bytes(&f->instance,op,affected,sizeof affected);
    single(&f->instance,DROP,op,TW_DB_OK);
}
static uint64_t query(fixture *f,uint64_t owner,uint8_t id,bool exists) {
    uint8_t request[sizeof get];memcpy(request,get,sizeof get);request[21]=id;
    uint64_t op=issue(f,TW_DB_QUERY,owner,request,sizeof request);
    check_bytes(&f->instance,op,exists?metadata:empty_metadata,
        exists?sizeof metadata:sizeof empty_metadata);
    uint64_t cursor=take(&f->instance,op);
    op=issue(f,TW_DB_FETCH,cursor,one_row,sizeof one_row);
    if(exists) {
        uint8_t expected[sizeof row];memcpy(expected,row,sizeof row);expected[17]=id;
        check_bytes(&f->instance,op,expected,sizeof expected);
    } else check_bytes(&f->instance,op,eof,sizeof eof);
    single(&f->instance,DROP,op,TW_DB_OK);
    return cursor;
}
static void close_ok(fixture *f,uint64_t resource,tw_db_opcode code) {
    finish_ok(&f->bridge,&f->worker,&f->wake,&f->instance,resource,code);
}
static void verify(fixture *f,uint64_t owner,uint8_t id,bool exists) {
    close_ok(f,query(f,owner,id,exists),TW_DB_CLOSE);
}

int main(void) {
    const char *root=required("TURBODB_ROOT");
    char driver[4096];
    int n=snprintf(driver,sizeof driver,"%s/lib/turbodb/drivers/turbodb_driver_mysql.so",root);
    assert(n>0&&(size_t)n<sizeof driver);
    const char *host=required("TW_MYSQL_HOST"),*ca=required("TW_MYSQL_CA_FILE");
    const char *name=required("TW_MYSQL_SERVER_NAME"),*db=required("TW_MYSQL_DATABASE");
    const char *port_text=required("TW_MYSQL_PORT");char *end=NULL;
    unsigned long port=strtoul(port_text,&end,10);
    assert(end!=port_text&&!*end&&port>0&&port<=UINT16_MAX);
    tw_db_mysql_settings writer={host,required("TW_MYSQL_USER"),
        required("TW_MYSQL_PASSWORD"),db,ca,name,(uint16_t)port};
    tw_db_mysql_settings reader=writer;
    reader.username=required("TW_MYSQL_READ_USER");reader.password=required("TW_MYSQL_READ_PASSWORD");
    tw_db_mysql_settings bad=writer;bad.server_name="wrong-host.invalid";
    const char *const writes[]={
        "INSERT INTO " TEST_TABLE "(id,payload) VALUES(?1,?2)",
        "SELECT id,payload,n FROM " TEST_TABLE " WHERE id=?1"
    };
    const char *const reads[]={writes[1]};
    tw_db_orm_alias native[]={
        {.statements=writes,.statement_count=2,.source.mysql=writer},
        {.read_only=true,.statements=reads,.statement_count=1,.source.mysql=reader},
        {.source.mysql=bad}
    };
    const tw_db_orm_config config={TW_DB_ORM_MYSQL,driver,native,3,2,1,1,16384,
        4000,8,4096,4096,65536};
    fixture f={0};
    /* An incomplete or downgraded host TLS config is rejected before load. */
    native[0].source.mysql.ca_file=NULL;
    assert(tw_db_orm_init(&f.provider,&policy,&config)==TW_DB_INVALID_REQUEST&&!f.provider.impl);
    native[0].source.mysql=writer;
    assert(tw_db_orm_init(&f.provider,&policy,&config)==TW_DB_OK);
    tw_db_dispatch dispatch={0};
    const tw_db_worker_config worker_config={4,128,16384,UINT64_C(30000000000),
        tw_db_orm_execute,&f.provider};
    assert(tw_db_worker_init(&f.worker,&f.bridge,&worker_config,&dispatch)==TW_DB_OK);
    assert(tw_db_bridge_init(&f.bridge,&policy,&cap,&dispatch)==TW_DB_OK);
    assert(turbowasm_module_load_borrowed(&f.module,database_bridge_bytes,
        sizeof database_bridge_bytes)==TURBOWASM_OK);
    assert(turbowasm_linker_init(&f.linker)==TURBOWASM_OK);
    assert(tw_db_bridge_define(&f.bridge,&f.linker)==TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(&f.instance,&f.module,&f.linker)==TURBOWASM_OK);
#ifdef TW_DB_TEST_MIR
    turbowasm_jit_backend jit={0};
    assert(turbowasm_mir_backend_create(&jit)==TURBOWASM_OK);
    assert(turbowasm_jit_instance_attach_backend(f.instance.impl,&jit,1)==TURBOWASM_OK);
#endif
    assert(tw_db_bridge_bind(&f.bridge,&f.instance)==TW_DB_OK);
    sync_mutex_init(&f.wake.mutex);sync_cond_init(&f.wake.cond);

    /* The real server has a trusted CA but a different DNS identity. */
    uint64_t op=issue(&f,TW_DB_OPEN_SQL,0,wrong_tls,sizeof wrong_tls);
    receipt(&f.instance,op,TW_DB_FAILED_NO_EFFECT,0);
    single(&f.instance,DROP,op,TW_DB_OK);

    uint64_t sql=open_alias(&f,mysql_alias,sizeof mysql_alias);
    uint64_t ro=open_alias(&f,readonly_alias,sizeof readonly_alias);
    assert(submit(&f.instance,TW_DB_EXECUTE,ro,put,sizeof put,TW_DB_DENIED)==0);
    assert(submit(&f.instance,TW_DB_BEGIN,sql,default_isolation,sizeof default_isolation,TW_DB_DENIED)==0);
    assert(submit(&f.instance,TW_DB_BEGIN,ro,isolation,sizeof isolation,TW_DB_DENIED)==0);

    command(&f,sql,7);
    verify(&f,ro,7,true);
    uint64_t tx=begin(&f,sql);
    command(&f,tx,11);
    uint64_t cursor=query(&f,tx,11,true);
    assert(submit(&f.instance,TW_DB_COMMIT,tx,NULL,0,TW_DB_BUSY)==0);
    close_ok(&f,cursor,TW_DB_CLOSE);
    verify(&f,ro,11,false);
    close_ok(&f,tx,TW_DB_COMMIT);
    assert(submit(&f.instance,TW_DB_COMMIT,tx,NULL,0,TW_DB_INVALID_HANDLE)==0);
    verify(&f,ro,11,true);

    tx=begin(&f,sql);command(&f,tx,12);close_ok(&f,tx,TW_DB_ROLLBACK);
    verify(&f,ro,12,false);
    /* A server duplicate-key error cannot be replayed as successful work. */
    op=issue(&f,TW_DB_EXECUTE,sql,put,sizeof put);
    receipt(&f.instance,op,TW_DB_OUTCOME_UNKNOWN,0);single(&f.instance,DROP,op,TW_DB_OK);
    assert(submit(&f.instance,TW_DB_EXECUTE,sql,put,sizeof put,TW_DB_WRONG_STATE)==0);
    close_ok(&f,sql,TW_DB_CLOSE);
    sql=open_alias(&f,mysql_alias,sizeof mysql_alias);

    /* Real abandoned transaction and a child snapshot. No commit on shutdown. */
    tx=begin(&f,sql);command(&f,tx,13);cursor=query(&f,tx,13,true);
    assert(cursor);
    close_ok(&f,ro,TW_DB_CLOSE);
#ifdef TW_DB_TEST_MIR
    turbowasm_instance_impl *p=f.instance.impl;
    assert(p->jit_functions[SUBMIT].state==TURBOWASM_JIT_COMPILED);
    assert(p->jit_functions[AWAIT].state==TURBOWASM_JIT_COMPILED);
    assert(p->jit_functions[READ].state==TURBOWASM_JIT_COMPILED);
#endif
    tw_db_bridge_stop(&f.bridge);
    turbowasm_instance_destroy(&f.instance);turbowasm_linker_destroy(&f.linker);
    turbowasm_module_destroy(&f.module);assert(tw_db_bridge_detach(&f.bridge)==TW_DB_OK);
    const tw_db_kind order[]={TW_DB_CURSOR,TW_DB_TRANSACTION,TW_DB_SQL};
    tw_db_shutdown_action action;
    for(size_t i=0;i<sizeof order/sizeof order[0];++i) {
        assert(tw_db_bridge_shutdown_step(&f.bridge,&action)==TW_DB_OK&&
            action.kind==TW_DB_SHUTDOWN_DISPATCH&&action.work.native_kind==order[i]);
        assert(tw_db_bridge_shutdown_step(&f.bridge,&action)==TW_DB_OK&&action.kind==TW_DB_SHUTDOWN_WAIT);
        deliver(&f.worker,&f.wake);
        assert(tw_db_bridge_shutdown_step(&f.bridge,&action)==TW_DB_OK&&
            action.kind==TW_DB_SHUTDOWN_RETIRED&&action.receipt.outcome==TW_DB_SUCCESS);
    }
    assert(tw_db_bridge_shutdown_step(&f.bridge,&action)==TW_DB_OK&&action.kind==TW_DB_SHUTDOWN_DONE);
    cflow_waitable wait=tw_db_worker_waitable(&f.worker);cflow_waitable_cancel(&wait);
    assert(tw_db_worker_destroy(&f.worker)==TW_DB_OK);
    assert(tw_db_bridge_destroy(&f.bridge)==TW_DB_OK);
    assert(tw_db_orm_destroy(&f.provider)==TW_DB_OK);
    sync_cond_destroy(&f.wake.cond);sync_mutex_destroy(&f.wake.mutex);
    return 0;
}
