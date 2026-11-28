#include "database_guest_test.h"
#include <orm.h>
#include <orm_runtime.h>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

static const uint32_t masks[]={TW_DB_MASK(TW_DB_I64),TW_DB_MASK(TW_DB_BLOB),TW_DB_MASK(TW_DB_I64)};
static const tw_db_template templates[]={
    {"item.put",TW_DB_MASK(TW_DB_EXECUTE),masks,2},
    {"item.get",TW_DB_MASK(TW_DB_QUERY),masks,1},
    {"item.ref",TW_DB_MASK(TW_DB_EXECUTE),masks,3}
};
static const tw_db_template read_templates[]={
    {"item.get",TW_DB_MASK(TW_DB_QUERY),masks,1},
    {"item.large",TW_DB_MASK(TW_DB_QUERY),NULL,0}
};
static const tw_db_alias aliases[]={
    {"local",TW_DB_SQL,TW_DB_MASK(TW_DB_ISOLATION_DEFAULT),0,templates,3},{"readonly",TW_DB_SQL,0,0,read_templates,2}
};
static const tw_db_policy policy={{128,24,64,4,3,16},aliases,2};
enum { PAGE_BYTES=112 };
static const tw_db_capacity cap={2,1,1,4,PAGE_BYTES,32768};
static const uint8_t local[]={5,0,0,0,'l','o','c','a','l'};
static const uint8_t readonly[]={8,0,0,0,'r','e','a','d','o','n','l','y'};
static const uint8_t put[]={8,0,0,0,'i','t','e','m','.','p','u','t',2,0,0,0,
    TW_DB_I64,8,0,0,0,7,0,0,0,0,0,0,0,TW_DB_BLOB,4,0,0,0,0,0xff,0x61,0};
static const uint8_t payload[]={0,0xff,0x61,0};
static const uint8_t get_query[]={8,0,0,0,'i','t','e','m','.','g','e','t',1,0,0,0,
    TW_DB_I64,8,0,0,0,7,0,0,0,0,0,0,0};
static const uint8_t large_query[]={10,0,0,0,'i','t','e','m','.','l','a','r','g','e',0,0,0,0};
static const uint8_t two_rows[]={2,0,0,0};
static const uint8_t default_isolation[]={0,0,0,0};
static const uint8_t serializable[]={TW_DB_ISOLATION_SERIALIZABLE,0,0,0};
/* Golden metadata: count, name-presence flag, total rows, counted names. */
static const uint8_t metadata[]={
    7,0,0,0,1,0,0,0,2,0,0,0,0,0,0,0,
    2,0,0,0,'i','d',
    7,0,0,0,'p','a','y','l','o','a','d',
    1,0,0,0,'n',
    10,0,0,0,'e','m','p','t','y','_','b','l','o','b',
    10,0,0,0,'e','m','p','t','y','_','t','e','x','t',
    1,0,0,0,'f',3,0,0,0,'t','x','t'
};
static const uint8_t empty_metadata[]={7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const uint8_t eof_page[]={0,0,0,0,1,0,0,0};
static const uint8_t first_page[]={
    1,0,0,0,0,0,0,0,7,0,0,0,
    TW_DB_I64,8,0,0,0,7,0,0,0,0,0,0,0,
    TW_DB_BLOB,4,0,0,0,0,0xff,0x61,0,
    TW_DB_NULL,0,0,0,0,TW_DB_BLOB,0,0,0,0,TW_DB_TEXT,0,0,0,0,
    TW_DB_F64,8,0,0,0,0,0,0,0,0,0,0xf8,0x3f,
    TW_DB_TEXT,3,0,0,0,0xe4,0xb8,0xad
};

#ifdef _WIN32
/* The CMeta explicit-path loader uses DLL_LOAD_DIR | DEFAULT_DIRS. PATH
 * permits the test executable's initial imports, but is not a trusted
 * plugin dependency directory. Admit the selected SDK and dynamic vcpkg
 * triplet bin roots, including the SQLite driver dependency closure.
 * Keep these cookies alive through the final ORM runtime/plugin unload. */
static DLL_DIRECTORY_COOKIE add_runtime_bin(const char *variable,const char *target) {
    const char *root=getenv(variable);
    assert(root&&*root);
    char path[4096];
    int n=target?snprintf(path,sizeof path,"%s/%s/bin",root,target):
        snprintf(path,sizeof path,"%s/bin",root);
    assert(n>0&&(size_t)n<sizeof path);
    wchar_t wide[4096];
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,
        (int)(sizeof wide/sizeof wide[0]));
    assert(count>0);
    for(int i=0;i<count;++i)if(wide[i]==L'/')wide[i]=L'\\';
    DLL_DIRECTORY_COOKIE cookie=AddDllDirectory(wide);
    if(!cookie)
        fprintf(stderr,"AddDllDirectory failed for %s (Win32 error %lu)\n",
            path,(unsigned long)GetLastError());
    assert(cookie);
    return cookie;
}
#endif

/* Trusted host setup and post-join persistence verification. Neither the SQL
 * strings nor the filesystem paths are supplied by a guest. */
static void host_database(const char *driver,const char *filename,bool setup) {
    orm_error_t error;orm_error_init(&error);
    orm_runtime_config_t cfg;orm_runtime_config_init(&cfg);
    orm_runtime_t *rt=NULL;
    assert(orm_runtime_create(&cfg,&rt,&error)==ORM_STATUS_OK);
    orm_driver_load_config_t load={0};
    load.struct_size=(uint32_t)sizeof load;load.abi_version=ORM_RUNTIME_ABI_VERSION;
    load.module_path=orm_view(driver);load.expected_driver_id=orm_view("sqlite");
    orm_status_t status=orm_runtime_load_driver(rt,&load,&error);
    if(status!=ORM_STATUS_OK)
        fprintf(stderr,"SQLite driver load failed: status=%d detail=%s module=%s\n",
            (int)status,error.message,driver);
    assert(status==ORM_STATUS_OK);
    orm_config_t connect;orm_config(&connect);
    const orm_option_t options[]={{orm_view("filename"),orm_view(filename)},
        {orm_view("open_mode"),orm_view(setup?"read_write_create":"read_only")}};
    connect.driver=orm_view("sqlite");connect.options=options;connect.option_count=2;
    orm_connection_t *db=NULL;
    assert(orm_runtime_connect(rt,&connect,&db,&error)==ORM_STATUS_OK);
    orm_query_t *q=NULL;orm_result_t *result=NULL;
    const char *sql=setup?"CREATE TABLE items(id INTEGER PRIMARY KEY,payload BLOB NOT NULL,"
        "parent_id INTEGER REFERENCES items(id) DEFERRABLE INITIALLY DEFERRED)":
        "SELECT id,payload FROM items ORDER BY id";
    assert(orm_raw(db,orm_view(sql),&q,&error)==ORM_STATUS_OK);
    assert(orm_query_execute(q,&result,&error)==ORM_STATUS_OK);
    if(!setup) {
        uint64_t rows=0;int64_t id=0;orm_blob_t blob={0};
        assert(orm_result_row_count(result,&rows,&error)==ORM_STATUS_OK&&rows==2);
        const int64_t ids[]={7,11};
        for(uint64_t row=0;row<rows;++row) {
            assert(orm_result_get_int64(result,row,0,&id,&error)==ORM_STATUS_OK&&id==ids[row]);
            assert(orm_result_get_blob(result,row,1,&blob,&error)==ORM_STATUS_OK&&
                blob.size==sizeof payload&&memcmp(blob.data,payload,sizeof payload)==0);
        }
    }
    orm_result_destroy(result);
    assert(orm_query_close(q,&error)==ORM_STATUS_OK);orm_query_release(q);
    assert(orm_connection_close(db,&error)==ORM_STATUS_OK);orm_connection_release(db);
    assert(orm_runtime_close(rt,&error)==ORM_STATUS_OK);orm_runtime_release(rt);
}


/* Exercise normal transaction paths through actual guest imports. Every
 * terminal operation releases its receipt independently from its resource. */
static uint64_t begin_tx(tw_db_bridge *bridge,tw_db_worker *worker,wake_signal *wake,
    turbowasm_instance *instance,uint64_t db) {
    uint64_t op=submit(instance,TW_DB_BEGIN,db,default_isolation,sizeof default_isolation,TW_DB_OK);
    settle(bridge,worker,wake,instance,op);
    receipt(instance,op,TW_DB_SUCCESS,0);
    return take(instance,op);
}
static void write_id(tw_db_bridge *bridge,tw_db_worker *worker,wake_signal *wake,
    turbowasm_instance *instance,uint64_t owner,uint8_t id) {
    uint8_t request[sizeof put];memcpy(request,put,sizeof put);request[21]=id;
    const uint8_t affected[]={1,0,0,0,0,0,0,0};
    uint64_t op=submit(instance,TW_DB_EXECUTE,owner,request,sizeof request,TW_DB_OK);
    settle(bridge,worker,wake,instance,op);check_bytes(instance,op,affected,sizeof affected);
    single(instance,DROP,op,TW_DB_OK);
}
static uint64_t select_id(tw_db_bridge *bridge,tw_db_worker *worker,wake_signal *wake,
    turbowasm_instance *instance,uint64_t owner,uint8_t id,bool exists) {
    uint8_t request[sizeof get_query];memcpy(request,get_query,sizeof get_query);request[21]=id;
    uint64_t op=submit(instance,TW_DB_QUERY,owner,request,sizeof request,TW_DB_OK);
    settle(bridge,worker,wake,instance,op);
    check_bytes(instance,op,exists?metadata:empty_metadata,
        exists?sizeof metadata:sizeof empty_metadata);
    return take(instance,op);
}
int main(void) {
    const char *root=getenv("TURBODB_ROOT");assert(root&&*root);
    char driver[4096],cwd[4096],filename[4096];
#ifdef _WIN32
    assert(_getcwd(cwd,sizeof cwd)!=NULL);
    const char *suffix=".dll";
#else
    assert(getcwd(cwd,sizeof cwd)!=NULL);
    const char *suffix=".so";
#endif
    int n=snprintf(driver,sizeof driver,"%s/lib/turbodb/drivers/turbodb_driver_sqlite%s",root,suffix);
    assert(n>0&&(size_t)n<sizeof driver);
    n=snprintf(filename,sizeof filename,"%s/turbowasm-db-%d.sqlite",cwd,process_id());
    assert(n>0&&(size_t)n<sizeof filename);
#ifdef _WIN32
    const char *triplet=getenv("VCPKG_TARGET_TRIPLET");
    assert(triplet&&*triplet);
    /* Match the release producer's selected runtime closure. Its Windows
       LLVM-MinGW triplets are dynamic: sqlite3 and its dependencies live in
       the vcpkg triplet bin, not in the TurboDB SDK. Do not search PATH or
       the other architecture's installed tree. */
    const DLL_DIRECTORY_COOKIE sdk_dirs[]={
        add_runtime_bin("TURBODB_ROOT",NULL),
        add_runtime_bin("SALTS_BASE_ROOT",NULL),
        add_runtime_bin("SALTS_UTILS_ROOT",NULL),
        add_runtime_bin("SALTSCORE_ROOT",NULL),
        add_runtime_bin("VCPKG_INSTALLED_DIR",triplet)
    };
#endif
    host_database(driver,filename,true);

    const char *select_sql=
        "SELECT items.id+offsets.n AS id,payload,NULL AS n,X'' AS empty_blob,"
        "'' AS empty_text,1.5 AS f,CAST(X'E4B8AD' AS TEXT) AS txt FROM items "
        "CROSS JOIN (SELECT 0 AS n UNION ALL SELECT 1) AS offsets "
        "WHERE items.id=?1 ORDER BY offsets.n";
    const char *const statements[]={
        "INSERT INTO items(id,payload) VALUES(?1,?2)",select_sql,
        "INSERT INTO items(id,payload,parent_id) VALUES(?1,?2,?3)"
    };
    const char *const read_statements[]={select_sql,"SELECT zeroblob(65) AS large_value"};
    const tw_db_orm_alias native_aliases[]={
        {.read_only=false,.statements=statements,.statement_count=3,.source.filename=filename},
        {.read_only=true,.statements=read_statements,.statement_count=2,.source.filename=filename}
    };
    const tw_db_orm_config native_config={TW_DB_ORM_SQLITE,driver,native_aliases,2,2,1,1,16384,
        1000,8,4096,4096,65536};
    tw_db_orm sqlite={0};
    tw_db_alias invalid_aliases[2]={aliases[0],aliases[1]};
    tw_db_policy invalid_policy={policy.limits,invalid_aliases,2};
    invalid_aliases[0].isolation_mask=TW_DB_MASK(TW_DB_ISOLATION_SERIALIZABLE);
    assert(tw_db_orm_init(&sqlite,&invalid_policy,&native_config)==TW_DB_INVALID_REQUEST);
    invalid_aliases[0]=aliases[0];
    invalid_aliases[1].isolation_mask=TW_DB_MASK(TW_DB_ISOLATION_DEFAULT);
    assert(tw_db_orm_init(&sqlite,&invalid_policy,&native_config)==TW_DB_INVALID_REQUEST);
    assert(tw_db_orm_init(&sqlite,&policy,&native_config)==TW_DB_OK);
    tw_db_worker worker={0};tw_db_bridge bridge={0};tw_db_dispatch dispatch={0};
    const tw_db_worker_config wc={4,PAGE_BYTES,16384,UINT64_C(30000000000),
        tw_db_orm_execute,&sqlite};
    assert(tw_db_worker_init(&worker,&bridge,&wc,&dispatch)==TW_DB_OK);
    assert(tw_db_bridge_init(&bridge,&policy,&cap,&dispatch)==TW_DB_OK);
    turbowasm_module module={0};turbowasm_linker linker={0};turbowasm_instance instance={0};
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
    wake_signal wake={0};sync_mutex_init(&wake.mutex);sync_cond_init(&wake.cond);

    uint64_t op=submit(&instance,TW_DB_OPEN_SQL,0,local,sizeof local,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    receipt(&instance,op,TW_DB_SUCCESS,0);uint64_t db=take(&instance,op);
    op=submit(&instance,TW_DB_EXECUTE,db,put,sizeof put,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    receipt(&instance,op,TW_DB_SUCCESS,8);
    turbowasm_value args[]={i64(op),i64(0),i64(1000),i64(8)},r[2];
    call(&instance,READ,args,4,r,2);assert(r[0].as.i32==TW_DB_OK&&r[1].as.i64==8);
    for(uint32_t i=0;i<8;++i) {
        turbowasm_value a=i32(1000+i),v;
        call(&instance,GET,&a,1,&v,1);assert(v.as.i32==(i==0?1:0));
    }
    single(&instance,DROP,op,TW_DB_OK);

    op=submit(&instance,TW_DB_OPEN_SQL,0,readonly,sizeof readonly,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);receipt(&instance,op,TW_DB_SUCCESS,0);
    uint64_t ro=take(&instance,op);
    assert(submit(&instance,TW_DB_EXECUTE,ro,put,sizeof put,TW_DB_DENIED)==0);
    op=submit(&instance,TW_DB_QUERY,ro,get_query,sizeof get_query,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    check_bytes(&instance,op,metadata,sizeof metadata);
    uint64_t cursor=take(&instance,op);
    assert(submit(&instance,TW_DB_CLOSE,ro,NULL,0,TW_DB_BUSY)==0);
    assert(submit(&instance,TW_DB_QUERY,db,get_query,sizeof get_query,TW_DB_LIMIT)==0);

    op=submit(&instance,TW_DB_FETCH,cursor,two_rows,sizeof two_rows,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    trap_read(&instance,op,sizeof first_page);
    check_bytes(&instance,op,first_page,sizeof first_page);
    single(&instance,DROP,op,TW_DB_OK);
    uint8_t second_page[sizeof first_page];
    memcpy(second_page,first_page,sizeof second_page);
    second_page[4]=1; /* EOF */
    second_page[17]=8; /* second row's integer id */
    op=submit(&instance,TW_DB_FETCH,cursor,two_rows,sizeof two_rows,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    check_bytes(&instance,op,second_page,sizeof second_page);
    single(&instance,DROP,op,TW_DB_OK);
    op=submit(&instance,TW_DB_FETCH,cursor,two_rows,sizeof two_rows,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    check_bytes(&instance,op,eof_page,sizeof eof_page);
    single(&instance,DROP,op,TW_DB_OK);
    op=submit(&instance,TW_DB_CLOSE,cursor,NULL,0,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    receipt(&instance,op,TW_DB_SUCCESS,0);single(&instance,DROP,op,TW_DB_OK);
    assert(submit(&instance,TW_DB_FETCH,cursor,two_rows,sizeof two_rows,TW_DB_INVALID_HANDLE)==0);

    /* Empty SELECT preserves native column count, not invented metadata. */
    uint8_t empty_query[sizeof get_query];
    memcpy(empty_query,get_query,sizeof empty_query);empty_query[21]=99;
    op=submit(&instance,TW_DB_QUERY,ro,empty_query,sizeof empty_query,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    check_bytes(&instance,op,empty_metadata,sizeof empty_metadata);
    cursor=take(&instance,op);
    op=submit(&instance,TW_DB_FETCH,cursor,two_rows,sizeof two_rows,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    check_bytes(&instance,op,eof_page,sizeof eof_page);single(&instance,DROP,op,TW_DB_OK);
    op=submit(&instance,TW_DB_CLOSE,cursor,NULL,0,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    receipt(&instance,op,TW_DB_SUCCESS,0);single(&instance,DROP,op,TW_DB_OK);

    /* Oversize value cannot publish a cursor that would never fit a page. */
    op=submit(&instance,TW_DB_QUERY,ro,large_query,sizeof large_query,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    receipt(&instance,op,TW_DB_FAILED_NO_EFFECT,0);
    single(&instance,DROP,op,TW_DB_OK);

    /* Real native held result disposal uses the existing same-slot CLOSE. */
    op=submit(&instance,TW_DB_QUERY,ro,get_query,sizeof get_query,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    check_bytes(&instance,op,metadata,sizeof metadata);
    single(&instance,DROP,op,TW_DB_PENDING);
    settle(&bridge,&worker,&wake,&instance,op);
    receipt(&instance,op,TW_DB_SUCCESS,0);
    single(&instance,DROP,op,TW_DB_OK);
    assert(submit(&instance,TW_DB_BEGIN,ro,default_isolation,sizeof default_isolation,TW_DB_DENIED)==0);
    assert(submit(&instance,TW_DB_BEGIN,db,serializable,sizeof serializable,TW_DB_DENIED)==0);
    uint64_t tx=begin_tx(&bridge,&worker,&wake,&instance,db);
    assert(submit(&instance,TW_DB_BEGIN,db,default_isolation,sizeof default_isolation,TW_DB_BUSY)==0);
    assert(submit(&instance,TW_DB_EXECUTE,db,put,sizeof put,TW_DB_BUSY)==0);
    assert(submit(&instance,TW_DB_CLOSE,db,NULL,0,TW_DB_BUSY)==0);
    write_id(&bridge,&worker,&wake,&instance,tx,11);
    cursor=select_id(&bridge,&worker,&wake,&instance,tx,11,true);
    assert(submit(&instance,TW_DB_COMMIT,tx,NULL,0,TW_DB_BUSY)==0);
    assert(submit(&instance,TW_DB_ROLLBACK,tx,NULL,0,TW_DB_BUSY)==0);
    uint8_t tx_page[sizeof first_page];
    memcpy(tx_page,first_page,sizeof tx_page);tx_page[17]=11;
    op=submit(&instance,TW_DB_FETCH,cursor,two_rows,sizeof two_rows,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    check_bytes(&instance,op,tx_page,sizeof tx_page);single(&instance,DROP,op,TW_DB_OK);
    finish_ok(&bridge,&worker,&wake,&instance,cursor,TW_DB_CLOSE);

    /* A second native read-only connection cannot see the uncommitted row. */
    cursor=select_id(&bridge,&worker,&wake,&instance,ro,11,false);
    finish_ok(&bridge,&worker,&wake,&instance,cursor,TW_DB_CLOSE);
    finish_ok(&bridge,&worker,&wake,&instance,tx,TW_DB_COMMIT);
    assert(submit(&instance,TW_DB_COMMIT,tx,NULL,0,TW_DB_INVALID_HANDLE)==0);
    cursor=select_id(&bridge,&worker,&wake,&instance,ro,11,true);
    finish_ok(&bridge,&worker,&wake,&instance,cursor,TW_DB_CLOSE);

    tx=begin_tx(&bridge,&worker,&wake,&instance,db);
    write_id(&bridge,&worker,&wake,&instance,tx,12);
    finish_ok(&bridge,&worker,&wake,&instance,tx,TW_DB_ROLLBACK);
    assert(submit(&instance,TW_DB_ROLLBACK,tx,NULL,0,TW_DB_INVALID_HANDLE)==0);
    cursor=select_id(&bridge,&worker,&wake,&instance,ro,12,false);
    finish_ok(&bridge,&worker,&wake,&instance,cursor,TW_DB_CLOSE);

    /* Untaken BEGIN is a real active transaction. DROP must issue rollback
       and drain it before returning capacity and unpinning its connection. */
    op=submit(&instance,TW_DB_BEGIN,db,default_isolation,sizeof default_isolation,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);receipt(&instance,op,TW_DB_SUCCESS,0);
    single(&instance,DROP,op,TW_DB_PENDING);
    settle(&bridge,&worker,&wake,&instance,op);receipt(&instance,op,TW_DB_SUCCESS,0);
    single(&instance,DROP,op,TW_DB_OK);
    finish_ok(&bridge,&worker,&wake,&instance,ro,TW_DB_CLOSE);

    /* Duplicate primary key fails after entering native execution. Preserve
       uncertainty instead of claiming no effect or replaying the command. */
    op=submit(&instance,TW_DB_EXECUTE,db,put,sizeof put,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);
    receipt(&instance,op,TW_DB_OUTCOME_UNKNOWN,0);
    single(&instance,DROP,op,TW_DB_OK);
    assert(submit(&instance,TW_DB_EXECUTE,db,put,sizeof put,TW_DB_WRONG_STATE)==0);
    finish_ok(&bridge,&worker,&wake,&instance,db,TW_DB_CLOSE);
    op=submit(&instance,TW_DB_OPEN_SQL,0,local,sizeof local,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);receipt(&instance,op,TW_DB_SUCCESS,0);
    db=take(&instance,op);

    /* Deferred FK failure occurs at actual SQLite COMMIT, not at INSERT.
       Keep the uncertain receipt and owner; prohibit commit replay. CLOSE
       rolls back the still-active native transaction and drains its lease. */
    tx=begin_tx(&bridge,&worker,&wake,&instance,db);
    uint8_t ref_request[sizeof put+13];
    memcpy(ref_request,put,sizeof put);
    memcpy(ref_request+4,"item.ref",8);ref_request[12]=3;ref_request[21]=14;
    const uint8_t missing_parent[]={TW_DB_I64,8,0,0,0,99,0,0,0,0,0,0,0};
    memcpy(ref_request+sizeof put,missing_parent,sizeof missing_parent);
    op=submit(&instance,TW_DB_EXECUTE,tx,ref_request,sizeof ref_request,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);receipt(&instance,op,TW_DB_SUCCESS,8);
    single(&instance,DROP,op,TW_DB_OK);
    op=submit(&instance,TW_DB_COMMIT,tx,NULL,0,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);receipt(&instance,op,TW_DB_OUTCOME_UNKNOWN,0);
    single(&instance,DROP,op,TW_DB_OK);
    assert(submit(&instance,TW_DB_COMMIT,tx,NULL,0,TW_DB_WRONG_STATE)==0);
    finish_ok(&bridge,&worker,&wake,&instance,tx,TW_DB_CLOSE);
    assert(submit(&instance,TW_DB_EXECUTE,db,put,sizeof put,TW_DB_WRONG_STATE)==0);
    finish_ok(&bridge,&worker,&wake,&instance,db,TW_DB_CLOSE);
    op=submit(&instance,TW_DB_OPEN_SQL,0,local,sizeof local,TW_DB_OK);
    settle(&bridge,&worker,&wake,&instance,op);receipt(&instance,op,TW_DB_SUCCESS,0);
    db=take(&instance,op);

    /* Abandoned transaction with a live child cursor: detached shutdown
       must close snapshot -> rollback transaction -> close connection. */
    tx=begin_tx(&bridge,&worker,&wake,&instance,db);
    write_id(&bridge,&worker,&wake,&instance,tx,13);
    cursor=select_id(&bridge,&worker,&wake,&instance,tx,13,true);
    assert(cursor);
#ifdef TW_DB_TEST_MIR
    turbowasm_instance_impl *p=instance.impl;
    assert(p->jit_functions[SUBMIT].state==TURBOWASM_JIT_COMPILED);
    assert(p->jit_functions[AWAIT].state==TURBOWASM_JIT_COMPILED);
    assert(p->jit_functions[READ].state==TURBOWASM_JIT_COMPILED);
#endif
    tw_db_bridge_stop(&bridge);
    turbowasm_instance_destroy(&instance);turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);assert(tw_db_bridge_detach(&bridge)==TW_DB_OK);
    tw_db_shutdown_action action={0};
    const tw_db_kind shutdown_order[]={TW_DB_CURSOR,TW_DB_TRANSACTION,TW_DB_SQL};
    for(size_t j=0;j<sizeof shutdown_order/sizeof shutdown_order[0];++j) {
        assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&
            action.kind==TW_DB_SHUTDOWN_DISPATCH);
        assert(action.work.request.opcode==TW_DB_CLOSE&&action.work.native_kind==shutdown_order[j]);
        assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&
            action.kind==TW_DB_SHUTDOWN_WAIT);
        deliver(&worker,&wake);
        assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&
            action.kind==TW_DB_SHUTDOWN_RETIRED&&action.receipt.outcome==TW_DB_SUCCESS);
    }
    assert(tw_db_bridge_shutdown_step(&bridge,&action)==TW_DB_OK&&action.kind==TW_DB_SHUTDOWN_DONE);
    cflow_waitable wait=tw_db_worker_waitable(&worker);cflow_waitable_cancel(&wait);
    assert(tw_db_worker_destroy(&worker)==TW_DB_OK);
    assert(tw_db_bridge_destroy(&bridge)==TW_DB_OK);
    assert(tw_db_orm_destroy(&sqlite)==TW_DB_OK);
    sync_cond_destroy(&wake.cond);sync_mutex_destroy(&wake.mutex);
    host_database(driver,filename,false);
    assert(remove(filename)==0);
#ifdef _WIN32
    for(size_t i=sizeof sdk_dirs/sizeof sdk_dirs[0];i>0;--i)
        assert(RemoveDllDirectory(sdk_dirs[i-1]));
#endif
    return 0;
}
