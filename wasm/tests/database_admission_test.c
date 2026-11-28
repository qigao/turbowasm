#include "database_wire.h"
#include "fixtures/database_admission.h"
#ifdef TW_DB_TEST_MIR
#include "instance_internal.h"
#include "jit/mir_backend.h"
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static const uint32_t put_masks[] = {TW_DB_MASK(TW_DB_I64),
    TW_DB_MASK(TW_DB_NULL) | TW_DB_MASK(TW_DB_BLOB)};
static const uint32_t get_masks[] = {TW_DB_MASK(TW_DB_I64)};
static const tw_db_template templates[] = {
    {"item.get", TW_DB_MASK(TW_DB_QUERY), get_masks, 1},
    {"item.put", TW_DB_MASK(TW_DB_EXECUTE), put_masks, 2}
};
static const tw_db_template readonly_templates[] = {
    {"item.get", TW_DB_MASK(TW_DB_QUERY), get_masks, 1}
};
static const tw_db_alias aliases[] = {
    {"local", TW_DB_SQL, TW_DB_MASK(TW_DB_ISOLATION_DEFAULT), 0, templates, 2},
    {"orders", TW_DB_SQL, 0, 0, readonly_templates, 1},
    {"cache", TW_DB_REDIS, 0, TW_DB_MASK(TW_DB_GET) | TW_DB_MASK(TW_DB_SET) |
        TW_DB_MASK(TW_DB_HGET) | TW_DB_MASK(TW_DB_HSET), NULL, 0}
};
static const tw_db_policy policy = {{128, 24, 64, 4, 3, 16}, aliases, 3};
static const tw_db_owner none = {TW_DB_NONE, 0}, sql = {TW_DB_SQL, 0},
    readonly_sql = {TW_DB_SQL, 1}, redis = {TW_DB_REDIS, 2},
    transaction = {TW_DB_TRANSACTION, 0}, cursor = {TW_DB_CURSOR, 0};
static const uint8_t open_local[] = {5,0,0,0,'l','o','c','a','l'};
static const uint8_t open_cache[] = {5,0,0,0,'c','a','c','h','e'};
static const uint8_t open_denied[] = {6,0,0,0,'s','e','c','r','e','t'};
static const uint8_t open_bad[] = {5,0,0,0,'a','.','.','b','c'};
static const uint8_t put[] = {8,0,0,0,'i','t','e','m','.','p','u','t', 2,0,0,0,
    TW_DB_I64,8,0,0,0, 1,0,0,0,0,0,0,0,
    TW_DB_BLOB,3,0,0,0, 0,'x',0xff};
static const uint8_t get[] = {8,0,0,0,'i','t','e','m','.','g','e','t', 1,0,0,0,
    TW_DB_I64,8,0,0,0, 1,0,0,0,0,0,0,0};
static const uint8_t redis_set[] = {TW_DB_SET,0,0,0, 2,0,0,0,
    3,0,0,0,'a',0,'b', 2,0,0,0,0,0xff};
static const uint8_t redis_del[] = {TW_DB_DEL,0,0,0, 1,0,0,0, 1,0,0,0,'k'};

static void reject(tw_db_owner owner, uint32_t opcode,
    const uint8_t *bytes, size_t size, tw_db_status expected) {
    tw_db_request q = {.rows = 12345, .alias_index = 99};
    assert(tw_db_admit(&policy, owner, opcode, bytes, size, &q) == expected);
    assert(q.rows == 12345 && q.alias_index == 99);
}

static void wire(void) {
    tw_db_request q = {0};
    assert(tw_db_admit(&policy, none, TW_DB_OPEN_SQL, open_local,
        sizeof open_local, &q) == TW_DB_OK && q.alias_index == 0);
    assert(tw_db_admit(&policy, none, TW_DB_OPEN_REDIS, open_cache,
        sizeof open_cache, &q) == TW_DB_OK && q.alias_index == 2);
    reject(none, TW_DB_OPEN_SQL, open_cache, sizeof open_cache, TW_DB_DENIED);
    reject(none, TW_DB_OPEN_SQL, open_denied, sizeof open_denied, TW_DB_DENIED);
    reject(none, TW_DB_OPEN_SQL, open_bad, sizeof open_bad, TW_DB_INVALID_REQUEST);
    reject(sql, TW_DB_OPEN_SQL, open_local, sizeof open_local, TW_DB_INVALID_HANDLE);
    for (size_t i=0; i<sizeof open_local; ++i)
        reject(none,TW_DB_OPEN_SQL,open_local,i,TW_DB_INVALID_REQUEST);
    assert(tw_db_admit(&policy, transaction, TW_DB_EXECUTE, put,
        sizeof put, &q) == TW_DB_OK);
    assert(q.alias_index == 0 && q.template_index == 1 && q.argument_count == 2);
    tw_db_reader reader = {q.arguments.data, q.arguments.size};
    tw_db_value value;
    assert(tw_db_read_value(&reader, 64, &value) && value.kind == TW_DB_I64);
    assert(tw_db_read_value(&reader, 64, &value) && value.kind == TW_DB_BLOB);
    assert(value.payload.size == 3 && value.payload.data[0] == 0 &&
           value.payload.data[2] == 0xff && reader.remaining == 0);
    assert(tw_db_admit(&policy, readonly_sql, TW_DB_QUERY, get, sizeof get, &q) == TW_DB_OK);
    reject(readonly_sql,TW_DB_EXECUTE,put,sizeof put,TW_DB_DENIED);
    reject(sql,TW_DB_QUERY,put,sizeof put,TW_DB_DENIED);
    reject(cursor,TW_DB_EXECUTE,put,sizeof put,TW_DB_INVALID_HANDLE);
    reject((tw_db_owner){TW_DB_SQL,2},TW_DB_EXECUTE,put,sizeof put,TW_DB_INVALID_HANDLE);
    uint8_t changed[sizeof put + 1];
    memcpy(changed,put,sizeof put);
    changed[16] = TW_DB_U64;
    reject(sql,TW_DB_EXECUTE,changed,sizeof put,TW_DB_INVALID_REQUEST);
    memcpy(changed,put,sizeof put);
    changed[12] = 5;
    reject(sql,TW_DB_EXECUTE,changed,sizeof put,TW_DB_LIMIT);
    for (size_t i=0; i<sizeof put; ++i)
        reject(sql,TW_DB_EXECUTE,put,i,TW_DB_INVALID_REQUEST);
    memcpy(changed,put,sizeof put);
    changed[sizeof put] = 0;
    reject(sql,TW_DB_EXECUTE,changed,sizeof changed,TW_DB_INVALID_REQUEST);
    const uint8_t zero[] = {0,0,0,0}, one[] = {1,0,0,0}, many[] = {17,0,0,0};
    assert(tw_db_admit(&policy,sql,TW_DB_BEGIN,zero,4,&q)==TW_DB_OK);
    reject(readonly_sql,TW_DB_BEGIN,zero,4,TW_DB_DENIED);
    reject(sql,TW_DB_BEGIN,one,4,TW_DB_DENIED);
    reject(cursor,TW_DB_FETCH,zero,4,TW_DB_INVALID_REQUEST);
    reject(cursor,TW_DB_FETCH,many,4,TW_DB_LIMIT);
    assert(tw_db_admit(&policy,cursor,TW_DB_FETCH,one,4,&q)==TW_DB_OK && q.rows==1);
    assert(tw_db_admit(&policy,transaction,TW_DB_COMMIT,NULL,0,&q)==TW_DB_OK);
    assert(tw_db_admit(&policy,transaction,TW_DB_ROLLBACK,NULL,0,&q)==TW_DB_OK);
    assert(tw_db_admit(&policy,redis,TW_DB_CLOSE,NULL,0,&q)==TW_DB_OK);
    reject(sql,TW_DB_COMMIT,NULL,0,TW_DB_INVALID_HANDLE);
    reject(sql,TW_DB_CLOSE,one,4,TW_DB_INVALID_REQUEST);
    reject(none,99,NULL,0,TW_DB_UNSUPPORTED);
    assert(tw_db_admit(&policy,redis,TW_DB_REDIS_COMMAND,redis_set,
        sizeof redis_set,&q)==TW_DB_OK && q.command==TW_DB_SET);
    reader = (tw_db_reader){q.arguments.data,q.arguments.size};
    tw_db_bytes b;
    assert(tw_db_read_bytes(&reader,64,&b) && b.size==3 && b.data[1]==0);
    assert(tw_db_read_bytes(&reader,64,&b) && b.size==2 && b.data[1]==0xff);
    reject(redis,TW_DB_REDIS_COMMAND,redis_del,sizeof redis_del,TW_DB_DENIED);
    uint8_t bad[sizeof redis_set];
    memcpy(bad,redis_set,sizeof bad); bad[0]=99;
    reject(redis,TW_DB_REDIS_COMMAND,bad,sizeof bad,TW_DB_UNSUPPORTED);
    memcpy(bad,redis_set,sizeof bad); bad[4]=4;
    reject(redis,TW_DB_REDIS_COMMAND,bad,sizeof bad,TW_DB_LIMIT);
    memcpy(bad,redis_set,sizeof bad); bad[8]=0;
    reject(redis,TW_DB_REDIS_COMMAND,bad,sizeof bad,TW_DB_INVALID_REQUEST);

    const uint8_t values[] = {TW_DB_NULL,0,0,0,0, TW_DB_BLOB,0,0,0,0,
        TW_DB_TEXT,3,0,0,0,0xe4,0xb8,0xad, TW_DB_TEXT,1,0,0,0,0};
    reader = (tw_db_reader){values,sizeof values};
    assert(tw_db_read_value(&reader,64,&value) && value.kind==TW_DB_NULL && value.payload.size==0);
    assert(tw_db_read_value(&reader,64,&value) && value.kind==TW_DB_BLOB && value.payload.size==0);
    assert(tw_db_read_value(&reader,64,&value) && value.kind==TW_DB_TEXT && value.payload.size==3);
    assert(tw_db_read_value(&reader,64,&value) && value.kind==TW_DB_TEXT && value.payload.data[0]==0);
    assert(reader.remaining==0);
    const uint8_t malformed[][9] = {
        {TW_DB_TEXT,2,0,0,0,0xc0,0x80}, {TW_DB_TEXT,3,0,0,0,0xed,0xa0,0x80},
        {TW_DB_TEXT,4,0,0,0,0xf4,0x90,0x80,0x80}, {TW_DB_TEXT,1,0,0,0,0xe2},
        {TW_DB_BOOL,1,0,0,0,2}, {TW_DB_NULL,1,0,0,0,0},
        {TW_DB_I64,3,0,0,0,1,2,3}, {255,0,0,0,0}, {TW_DB_BLOB,255,255,255,255}
    };
    for(size_t i=0;i<sizeof malformed/sizeof malformed[0];++i) {
        reader=(tw_db_reader){malformed[i],sizeof malformed[i]};
        value=(tw_db_value){TW_DB_BOOL,{NULL,7}};
        assert(!tw_db_read_value(&reader,64,&value));
        assert(reader.data==malformed[i] && reader.remaining==sizeof malformed[i]);
        assert(value.kind==TW_DB_BOOL && value.payload.size==7);
    }
}

typedef struct fixture_context {
    const turbowasm_instance *instance;
    uint8_t scratch[128];
    unsigned admitted;
} fixture_context;

static turbowasm_status admission(void *context, turbowasm_host_call *call,
    const turbowasm_value *args, size_t argc, turbowasm_value *results,
    size_t capacity, size_t *count, turbowasm_trap *trap) {
    fixture_context *f = context;
    assert(argc==4 && capacity>=1);
    *count=0;
    *trap=TURBOWASM_TRAP_NONE;
    uint64_t token=(uint64_t)args[1].as.i64;
    /* Fixture-owned resolved identities; no real resource table yet. */
    tw_db_owner owner;
    switch(token) {
    case 0: owner=none; break;
    case 11: owner=sql; break;
    case 12: owner=readonly_sql; break;
    case 13: owner=redis; break;
    default: owner=(tw_db_owner){TW_DB_CURSOR,SIZE_MAX}; break;
    }
    tw_db_request q={0};
    tw_db_status db;
    turbowasm_status status=tw_db_admit_guest(&policy,f->instance,call,owner,
        (uint32_t)args[0].as.i32,(uint64_t)args[2].as.i64,(uint64_t)args[3].as.i64,
        f->scratch,sizeof f->scratch,&q,&db,trap);
    if(status!=TURBOWASM_OK) return status;
    if(db==TW_DB_OK) ++f->admitted;
    results[0]=(turbowasm_value){.kind=TURBOWASM_VALUE_I32,.as.i32=(int32_t)db};
    *count=1;
    return TURBOWASM_OK;
}

static turbowasm_name name(const char *s) {
    return (turbowasm_name){(const uint8_t *)s,(uint32_t)strlen(s)};
}
static turbowasm_value i32(uint32_t v) {
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I32,.as.i32=(int32_t)v};
}
static turbowasm_value i64(uint64_t v) {
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I64,.as.i64=(int64_t)v};
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
static void store(turbowasm_instance *i,const uint8_t *bytes,size_t size) {
    for(size_t n=0;n<size;++n) {
        turbowasm_value a[]={i32(8192+(uint32_t)n),i32(bytes[n])};
        size_t count=0;
        turbowasm_trap trap;
        assert(turbowasm_instance_invoke(i,2,a,2,NULL,0,&count,&trap)==TURBOWASM_OK);
        assert(count==0 && trap==TURBOWASM_TRAP_NONE);
    }
}
static void invoke(turbowasm_instance *i,uint32_t op,uint64_t owner,
    uint64_t address,uint64_t length,turbowasm_status expected,tw_db_status db) {
    turbowasm_value a[]={i32(op),i64(owner),i64(address),i64(length)},v={0};
    size_t count=0;
    turbowasm_trap trap=TURBOWASM_TRAP_NONE;
    turbowasm_status s=turbowasm_instance_invoke(i,1,a,4,&v,1,&count,&trap);
    assert(s==expected);
    if(s==TURBOWASM_OK) {
        assert(count==1 && trap==TURBOWASM_TRAP_NONE && v.kind==TURBOWASM_VALUE_I32);
        assert(v.as.i32==(int32_t)db);
    } else {
        assert(s==TURBOWASM_TRAPPED && trap==TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
    }
}

static void guest(void) {
    turbowasm_module module={0};
    turbowasm_instance first={0},second={0},missing={0};
    turbowasm_linker linker={0};
    fixture_context f={.instance=&first};
    const turbowasm_value_kind p[]={TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I64,
        TURBOWASM_VALUE_I64,TURBOWASM_VALUE_I64}, result[]={TURBOWASM_VALUE_I32};
    const turbowasm_host_function_type type={p,4,result,1};
    assert(turbowasm_module_load_borrowed(&module,database_admission_bytes,
        sizeof database_admission_bytes)==TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker)==TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(&missing,&module,&linker)==TURBOWASM_LINK_ERROR);
    assert(!missing.impl);
    assert(turbowasm_linker_define_host_function(&linker,name("fixture:db"),name("admit"),
        &type,admission,&f)==TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(&first,&module,&linker)==TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(&second,&module,&linker)==TURBOWASM_OK);
    attach(&first); attach(&second);
    store(&first,open_local,sizeof open_local);
    store(&second,open_local,sizeof open_local);
    invoke(&first,TW_DB_OPEN_SQL,0,8192,sizeof open_local,TURBOWASM_OK,TW_DB_OK);
    invoke(&second,TW_DB_OPEN_SQL,0,8192,sizeof open_local,TURBOWASM_OK,TW_DB_DENIED);
    invoke(&second,TW_DB_OPEN_SQL,0,UINT64_MAX,8,TURBOWASM_OK,TW_DB_DENIED);
    invoke(&first,TW_DB_OPEN_SQL,0,65535,9,TURBOWASM_TRAPPED,TW_DB_OK);
    invoke(&first,TW_DB_OPEN_SQL,0,UINT64_C(0x100002000),9,TURBOWASM_TRAPPED,TW_DB_OK);
    invoke(&first,TW_DB_OPEN_SQL,0,8192,UINT64_MAX,TURBOWASM_OK,TW_DB_LIMIT);
    store(&first,put,sizeof put);
    invoke(&first,TW_DB_EXECUTE,11,8192,sizeof put,TURBOWASM_OK,TW_DB_OK);
    invoke(&first,TW_DB_EXECUTE,12,8192,sizeof put,TURBOWASM_OK,TW_DB_DENIED);
    invoke(&first,TW_DB_EXECUTE,77,8192,sizeof put,TURBOWASM_OK,TW_DB_INVALID_HANDLE);
    invoke(&first,TW_DB_EXECUTE,11,8192,sizeof put-1,TURBOWASM_OK,TW_DB_INVALID_REQUEST);
    store(&first,redis_set,sizeof redis_set);
    invoke(&first,TW_DB_REDIS_COMMAND,13,8192,sizeof redis_set,TURBOWASM_OK,TW_DB_OK);
    assert(f.admitted==3);
#ifdef TW_DB_TEST_MIR
    assert(((turbowasm_instance_impl *)first.impl)->jit_functions[1].state==TURBOWASM_JIT_COMPILED);
    assert(((turbowasm_instance_impl *)second.impl)->jit_functions[1].state==TURBOWASM_JIT_COMPILED);
#endif
    turbowasm_instance_destroy(&second);
    turbowasm_instance_destroy(&first);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
}


static void result_writer(void) {
    uint8_t bytes[32];
    memset(bytes,0xaa,sizeof bytes);
    tw_db_writer w={bytes,sizeof bytes,0};
    assert(tw_db_write_u32(&w,UINT32_C(0x12345678)));
    assert(w.size==4&&bytes[0]==0x78&&bytes[3]==0x12);
    assert(tw_db_write_u64(&w,UINT64_C(0x0102030405060708)));
    assert(w.size==12&&bytes[4]==8&&bytes[11]==1);
    assert(tw_db_write_value(&w,8,(tw_db_value){TW_DB_NULL,{NULL,0}}));
    assert(tw_db_write_value(&w,8,(tw_db_value){TW_DB_BLOB,{NULL,0}}));
    assert(w.size==22&&bytes[12]==TW_DB_NULL&&bytes[17]==TW_DB_BLOB);
    const uint8_t invalid[]={0xc0,0x80};
    assert(!tw_db_write_text(&w,(tw_db_bytes){invalid,sizeof invalid}));
    assert(!tw_db_write_value(&w,8,(tw_db_value){TW_DB_TEXT,{invalid,sizeof invalid}}));
    uint8_t too_large[12]={0};
    assert(!tw_db_write_bytes(&w,(tw_db_bytes){too_large,sizeof too_large}));
    assert(!tw_db_write_value(&w,8,(tw_db_value){TW_DB_BLOB,{too_large,sizeof too_large}}));
    assert(w.size==22&&bytes[22]==0xaa);
    const uint8_t text[]={0xe4,0xb8,0xad};
    assert(tw_db_write_value(&w,8,(tw_db_value){TW_DB_TEXT,{text,sizeof text}}));
    tw_db_reader reader={bytes+12,w.size-12};
    tw_db_value v;
    assert(tw_db_read_value(&reader,8,&v)&&v.kind==TW_DB_NULL);
    assert(tw_db_read_value(&reader,8,&v)&&v.kind==TW_DB_BLOB&&v.payload.size==0);
    assert(tw_db_read_value(&reader,8,&v)&&v.kind==TW_DB_TEXT&&
        v.payload.size==sizeof text&&!memcmp(v.payload.data,text,sizeof text)&&!reader.remaining);
}
int main(void) {
    result_writer(); wire(); guest(); return 0; }
