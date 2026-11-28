#include "database_orm.h"
#include "database_policy.h"
#include "database_sql_result.h"
#include "runtime_alloc.h"
#include <orm.h>
#include <orm_runtime.h>
#include <orm_driver_ops.h>
#include <threads/threads.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sql_slot {
    orm_connection_t *connection;
    size_t alias;
    bool transaction_active;
} sql_slot;

typedef enum sql_tx_state {
    SQL_TX_ACTIVE=1, SQL_TX_COMMIT_UNKNOWN, SQL_TX_ENDED,
    SQL_TX_CLEANUP_BLOCKED
} sql_tx_state;

typedef struct sql_tx_slot {
    orm_transaction_t *transaction;
    size_t connection_index;
    sql_tx_state state;
} sql_tx_slot;

typedef struct sql_cursor_slot {
    tw_db_sql_result result;
    size_t alias;
} sql_cursor_slot;

typedef struct sql_impl {
    tw_db_orm_config config;
    tw_db_limits limits;
    orm_runtime_t *runtime;
    sql_slot *slots;
    sql_cursor_slot *cursors;
    sql_tx_slot *transactions;
    size_t live, live_cursors, live_transactions;
    const void *lane;
    char timeout[16];
    uint32_t isolation_mask;
} sql_impl;

static tw_db_worker_result result(tw_db_outcome outcome) {
    return (tw_db_worker_result){outcome,0,0};
}

static bool absolute_path(const char *path) {
    if(!path||!*path)return false;
#ifdef _WIN32
    /* Explicit drive-rooted or UNC path; no current-drive relative paths. */
    if(((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z'))&&
        path[1]==':'&&(path[2]=='/'||path[2]=='\\'))return true;
    return (path[0]=='/'&&path[1]=='/')||(path[0]=='\\'&&path[1]=='\\');
#else
    return path[0]=='/';
#endif
}

static uint32_t supported_isolations(tw_db_orm_driver driver) {
    return driver==TW_DB_ORM_SQLITE?TW_DB_MASK(TW_DB_ISOLATION_DEFAULT):
        TW_DB_MASK(TW_DB_ISOLATION_READ_COMMITTED)|
        TW_DB_MASK(TW_DB_ISOLATION_REPEATABLE_READ)|
        TW_DB_MASK(TW_DB_ISOLATION_SERIALIZABLE);
}

static bool mysql_config_valid(const tw_db_mysql_settings *m) {
    return m->host&&*m->host&&m->username&&*m->username&&m->password&&
        m->database&&*m->database&&m->port&&absolute_path(m->ca_file)&&
        m->server_name&&*m->server_name;
}

static const char *driver_id(const sql_impl *p) {
    return p->config.driver==TW_DB_ORM_SQLITE?"sqlite":"drivers/mysql";
}

tw_db_status tw_db_orm_init(tw_db_orm *provider,const tw_db_policy *policy,
    const tw_db_orm_config *config) {
    if(!provider||provider->impl||!policy||!config||!config->connections||
        (config->driver!=TW_DB_ORM_SQLITE&&config->driver!=TW_DB_ORM_MYSQL)||
        !absolute_path(config->driver_path)||!config->aliases||
        !policy->aliases||config->alias_count!=policy->alias_count||
        !config->max_sql_bytes||!config->max_parameter_bytes||
        !config->max_result_bytes||!config->max_columns||
        (config->driver==TW_DB_ORM_SQLITE&&config->timeout_ms>INT32_MAX)||
        (config->driver==TW_DB_ORM_MYSQL&&!config->timeout_ms))
        return TW_DB_INVALID_REQUEST;
    uint32_t isolation_mask=0;
    for(size_t i=0;i<config->alias_count;++i) {
        const tw_db_alias *a=&policy->aliases[i];
        const tw_db_orm_alias *n=&config->aliases[i];
        if(a->kind!=TW_DB_SQL||a->redis_command_mask||
            (a->isolation_mask&~supported_isolations(config->driver))||
            (a->isolation_mask&&(!config->transactions||n->read_only))||
            (config->driver==TW_DB_ORM_SQLITE&&!absolute_path(n->source.filename))||
            (config->driver==TW_DB_ORM_MYSQL&&!mysql_config_valid(&n->source.mysql))||
            n->statement_count!=a->template_count||
            (n->statement_count&&(!n->statements||!a->templates)))
            return TW_DB_INVALID_REQUEST;
        isolation_mask|=a->isolation_mask;
        for(size_t j=0;j<n->statement_count;++j) {
            uint32_t op=a->templates[j].opcode_mask;
            if((op!=TW_DB_MASK(TW_DB_EXECUTE)&&op!=TW_DB_MASK(TW_DB_QUERY))||
                (op==TW_DB_MASK(TW_DB_EXECUTE)&&n->read_only)||
                (op==TW_DB_MASK(TW_DB_QUERY)&&(!config->cursors||!policy->limits.rows))||
                !n->statements[j]||!n->statements[j][0]||
                strlen(n->statements[j])>config->max_sql_bytes)
                return TW_DB_INVALID_REQUEST;
        }
    }
    size_t budget=config->host_bytes;
    if(!tw_db_budget_take(&budget,1,sizeof(sql_impl))||
        !tw_db_budget_take(&budget,config->connections,sizeof(sql_slot))||
        !tw_db_budget_take(&budget,config->cursors,sizeof(sql_cursor_slot))||
        !tw_db_budget_take(&budget,config->transactions,sizeof(sql_tx_slot)))return TW_DB_LIMIT;
    sql_impl *p=turbowasm_rt_calloc(1,sizeof(*p));
    if(!p)return TW_DB_OUT_OF_MEMORY;
    p->slots=turbowasm_rt_calloc(config->connections,sizeof(*p->slots));
    if(config->cursors)p->cursors=turbowasm_rt_calloc(config->cursors,sizeof(*p->cursors));
    if(config->transactions)
        p->transactions=turbowasm_rt_calloc(config->transactions,sizeof(*p->transactions));
    if(!p->slots||(config->cursors&&!p->cursors)||
        (config->transactions&&!p->transactions)) {
        turbowasm_rt_free(p->transactions);turbowasm_rt_free(p->cursors);
        turbowasm_rt_free(p->slots);turbowasm_rt_free(p);
        return TW_DB_OUT_OF_MEMORY;
    }
    p->config=*config;p->limits=policy->limits;p->isolation_mask=isolation_mask;
    (void)snprintf(p->timeout,sizeof p->timeout,"%u",(unsigned)config->timeout_ms);
    provider->impl=p;
    return TW_DB_OK;
}

/* Runtime is created and unloaded on the same serial worker as its leases.
 * Failure to drain an unpublished/runtime owner is fail-fast, not a fabricated
 * successful close. A published connection close failure retains its slot. */
static void release_runtime(sql_impl *p) {
    orm_error_t error;
    orm_error_init(&error);
    if(orm_runtime_close(p->runtime,&error)!=ORM_STATUS_OK)abort();
    orm_runtime_release(p->runtime);p->runtime=NULL;
}

static bool create_runtime(sql_impl *p) {
    orm_error_t error;
    orm_error_init(&error);
    orm_runtime_config_t cfg;
    orm_runtime_config_init(&cfg);
    cfg.max_drivers=1;
    cfg.max_connections=p->config.connections;
    if(orm_runtime_create(&cfg,&p->runtime,&error)!=ORM_STATUS_OK)return false;
    orm_driver_load_config_t load={0};
    load.struct_size=(uint32_t)sizeof load;
    load.abi_version=ORM_RUNTIME_ABI_VERSION;
    load.module_path=orm_view(p->config.driver_path);
    load.expected_driver_id=orm_view(driver_id(p));
    orm_driver_info_t info={0};
    if(orm_runtime_load_driver(p->runtime,&load,&error)!=ORM_STATUS_OK||
        orm_runtime_driver_info(p->runtime,orm_view(driver_id(p)),&info,&error)!=ORM_STATUS_OK||
        !(info.capabilities&ORM_DRIVER_CAP_RAW_SQL)||
        (p->config.cursors&&!(info.capabilities&ORM_DRIVER_CAP_SELECT))||
        (p->config.transactions&&!(info.capabilities&ORM_DRIVER_CAP_TRANSACTION))||
        !(info.execution_models&ORM_DRIVER_EXEC_CALLER_BLOCKING)||
        ((p->isolation_mask&TW_DB_MASK(TW_DB_ISOLATION_READ_COMMITTED))&&
            !(info.capabilities&ORM_DRIVER_CAP_READ_COMMITTED))||
        ((p->isolation_mask&TW_DB_MASK(TW_DB_ISOLATION_REPEATABLE_READ))&&
            !(info.capabilities&ORM_DRIVER_CAP_REPEATABLE_READ))||
        ((p->isolation_mask&TW_DB_MASK(TW_DB_ISOLATION_SERIALIZABLE))&&
            !(info.capabilities&ORM_DRIVER_CAP_SERIALIZABLE))) {
        release_runtime(p);return false;
    }
    return true;
}

static tw_db_worker_result open_connection(sql_impl *p,const tw_db_worker_call *call) {
    const size_t alias=call->work.request.alias_index;
    if(alias>=p->config.alias_count)return result(TW_DB_FAILED_NO_EFFECT);
    size_t i=0;
    while(i<p->config.connections&&p->slots[i].connection)++i;
    if(i==p->config.connections)return result(TW_DB_FAILED_NO_EFFECT);
    if(call->interrupted(call->interrupt_context))return result(TW_DB_CANCELLED_NO_EFFECT);
    if(!p->runtime&&!create_runtime(p))return result(TW_DB_FAILED_NO_EFFECT);
    const tw_db_orm_alias *a=&p->config.aliases[alias];
    orm_config_t cfg;
    orm_config(&cfg);
    orm_option_t options[8];
    char port[8];
    if(p->config.driver==TW_DB_ORM_SQLITE) {
        options[0]=(orm_option_t){orm_view("filename"),orm_view(a->source.filename)};
        options[1]=(orm_option_t){orm_view("open_mode"),orm_view(a->read_only?"read_only":"read_write")};
        options[2]=(orm_option_t){orm_view("busy_timeout_ms"),orm_view(p->timeout)};
        cfg.option_count=3;
    } else {
        const tw_db_mysql_settings *m=&a->source.mysql;
        (void)snprintf(port,sizeof port,"%u",(unsigned)m->port);
        options[0]=(orm_option_t){orm_view("host"),orm_view(m->host)};
        options[1]=(orm_option_t){orm_view("port"),orm_view(port)};
        options[2]=(orm_option_t){orm_view("username"),orm_view(m->username)};
        options[3]=(orm_option_t){orm_view("password"),orm_view(m->password)};
        options[4]=(orm_option_t){orm_view("database"),orm_view(m->database)};
        options[5]=(orm_option_t){orm_view("ca_file"),orm_view(m->ca_file)};
        options[6]=(orm_option_t){orm_view("server_name"),orm_view(m->server_name)};
        options[7]=(orm_option_t){orm_view("timeout_ms"),orm_view(p->timeout)};
        cfg.option_count=8;
    }
    cfg.driver=orm_view(driver_id(p));cfg.options=options;
    cfg.max_parameters=p->limits.parameters;
    cfg.max_columns=p->config.max_columns;
    cfg.max_query_bytes=p->config.max_sql_bytes;
    cfg.max_parameter_bytes=p->config.max_parameter_bytes;
    cfg.max_result_rows=p->limits.rows;
    cfg.max_result_bytes=p->config.max_result_bytes;
    orm_error_t error;orm_error_init(&error);
    orm_connection_t *connection=NULL;
    if(orm_runtime_connect(p->runtime,&cfg,&connection,&error)!=ORM_STATUS_OK) {
        if(!p->live)release_runtime(p);
        return result(TW_DB_FAILED_NO_EFFECT);
    }
    p->slots[i]=(sql_slot){.connection=connection,.alias=alias};++p->live;
    return (tw_db_worker_result){TW_DB_SUCCESS,(uintptr_t)(i+1),0};
}

static uint64_t load_le64(const uint8_t *b) {
    uint64_t v=0;
    for(unsigned i=0;i<8;++i)v|=(uint64_t)b[i]<<(8u*i);
    return v;
}

static orm_value_t bind_value(tw_db_value v) {
    switch(v.kind) {
    case TW_DB_NULL:return orm_null();
    case TW_DB_BOOL:return orm_bool(v.payload.data[0]);
    case TW_DB_I64:{
        uint64_t bits=load_le64(v.payload.data);int64_t n;
        memcpy(&n,&bits,sizeof n);return orm_i64(n);
    }
    case TW_DB_U64:return orm_u64(load_le64(v.payload.data));
    case TW_DB_F64:{
        uint64_t bits=load_le64(v.payload.data);double n;
        _Static_assert(sizeof(double)==sizeof(uint64_t),"binary64 required");
        memcpy(&n,&bits,sizeof n);return orm_f64(n);
    }
    case TW_DB_TEXT:return orm_text_v(vstr_from_buf((const char *)v.payload.data,v.payload.size));
    case TW_DB_BLOB:return orm_blob(v.payload.data,v.payload.size);
    default:abort(); /* already validated canonical request */
    }
}

static tw_db_worker_result execute_sql(sql_impl *p,sql_slot *s,
    orm_transaction_t *tx,const tw_db_worker_call *call,uint8_t *bytes,size_t capacity) {
    const tw_db_request *q=&call->work.request;
    const tw_db_orm_alias *a=&p->config.aliases[s->alias];
    const bool query_rows=q->opcode==TW_DB_QUERY;
    if((!query_rows&&a->read_only)||q->template_index>=a->statement_count||
        capacity<(query_rows?16u:8u))return result(TW_DB_FAILED_NO_EFFECT);
    size_t cursor_index=0;
    if(query_rows) {
        while(cursor_index<p->config.cursors&&p->cursors[cursor_index].result.native)
            ++cursor_index;
        if(cursor_index==p->config.cursors)return result(TW_DB_FAILED_NO_EFFECT);
    }
    orm_error_t error;orm_error_init(&error);
    orm_query_t *query=NULL;
    if(orm_raw(s->connection,orm_view(a->statements[q->template_index]),&query,&error)!=ORM_STATUS_OK)
        return result(TW_DB_FAILED_NO_EFFECT);
    tw_db_worker_result out=result(TW_DB_FAILED_NO_EFFECT);
    tw_db_reader reader={q->arguments.data,q->arguments.size};
    for(uint32_t i=0;i<q->argument_count;++i) {
        tw_db_value value;
        if(!tw_db_read_value(&reader,p->limits.value_bytes,&value))goto done;
        if(orm_query_bind(query,bind_value(value),&error)!=ORM_STATUS_OK)goto done;
    }
    if(reader.remaining)goto done;
    if(call->interrupted(call->interrupt_context)) {
        out=result(TW_DB_CANCELLED_NO_EFFECT);goto done;
    }
    /* After entering execution, failure does not prove absence of effects
       (e.g. native errors after a write). Conservatively quarantine. */
    orm_result_t *native=NULL;
    out=result(TW_DB_OUTCOME_UNKNOWN);
    orm_status_t status=tx?orm_query_execute_in_transaction(query,tx,&native,&error):
        orm_query_execute(query,&native,&error);
    if(status==ORM_STATUS_OK) {
        if(query_rows) {
            /* Host-granted side-effect-free SELECT; a projection/limit
               failure after successful execution produces no resource and
               no mutation. Native execution failures remain conservative. */
            out=result(TW_DB_FAILED_NO_EFFECT);
            sql_cursor_slot *c=&p->cursors[cursor_index];
            size_t written=0;
            if(tw_db_sql_result_open(&c->result,native,p->limits.rows,
                p->config.max_columns,p->limits.value_bytes,bytes,capacity,&written)) {
                c->alias=s->alias;
                ++p->live_cursors;
                out=(tw_db_worker_result){TW_DB_SUCCESS,(uintptr_t)(cursor_index+1),written};
                native=NULL; /* the cursor owns the native snapshot */
            }
        } else {
            uint64_t affected=0;
            if(orm_result_affected_rows(native,&affected,&error)==ORM_STATUS_OK) {
                for(unsigned i=0;i<8;++i)bytes[i]=(uint8_t)(affected>>(8u*i));
                out=(tw_db_worker_result){TW_DB_SUCCESS,0,8};
            }
        }
        if(native)orm_result_destroy(native);
    }
done:
    if(orm_query_close(query,&error)!=ORM_STATUS_OK)abort();
    orm_query_release(query);
    return out;
}

/* Reserve the provider's fixed slot before native BEGIN. SQLite's native
 * default uses the ORM SERIALIZABLE carrier, not a selectable isolation
 * promise. MySQL uses only the selected advertised isolation enum. */
static tw_db_worker_result begin_transaction(sql_impl *p,sql_slot *s,
    const tw_db_worker_call *call) {
    uint32_t isolation=call->work.request.isolation;
    if(isolation>TW_DB_ISOLATION_SERIALIZABLE||
        !(supported_isolations(p->config.driver)&TW_DB_MASK(isolation))||
        p->config.aliases[s->alias].read_only||s->transaction_active)
        return result(TW_DB_FAILED_NO_EFFECT);
    orm_isolation_t native;
    switch(isolation) {
    case TW_DB_ISOLATION_DEFAULT:
    case TW_DB_ISOLATION_SERIALIZABLE:native=ORM_ISOLATION_SERIALIZABLE;break;
    case TW_DB_ISOLATION_READ_COMMITTED:native=ORM_ISOLATION_READ_COMMITTED;break;
    case TW_DB_ISOLATION_REPEATABLE_READ:native=ORM_ISOLATION_REPEATABLE_READ;break;
    default:return result(TW_DB_FAILED_NO_EFFECT);
    }
    size_t index=0;
    while(index<p->config.transactions&&p->transactions[index].transaction)++index;
    if(index==p->config.transactions)return result(TW_DB_FAILED_NO_EFFECT);
    if(call->interrupted(call->interrupt_context))return result(TW_DB_CANCELLED_NO_EFFECT);
    orm_error_t error;orm_error_init(&error);
    orm_transaction_t *transaction=NULL;
    if(orm_transaction_begin(s->connection,native,
        &transaction,&error)!=ORM_STATUS_OK)
        return result(TW_DB_OUTCOME_UNKNOWN);
    p->transactions[index]=(sql_tx_slot){transaction,(size_t)(s-p->slots),SQL_TX_ACTIVE};
    s->transaction_active=true;
    ++p->live_transactions;
    return (tw_db_worker_result){TW_DB_SUCCESS,(uintptr_t)(index+1),0};
}

/* Success consumes the native transaction only after checked close/release.
 * A failed COMMIT is never reissued. If the native API explicitly reports
 * COMMIT_UNKNOWN, checked close is permitted without another commit or
 * rollback; otherwise CLOSE attempts rollback of the retained active owner.
 * A failed rollback or failed checked close is sticky: do not replay
 * uncertain cleanup or free the native handle. */
static tw_db_worker_result finish_transaction(sql_impl *p,sql_tx_slot *t,
    const tw_db_worker_call *call) {
    const tw_db_opcode op=call->work.request.opcode;
    if(t->state==SQL_TX_CLEANUP_BLOCKED)return result(TW_DB_OUTCOME_UNKNOWN);
    if(op!=TW_DB_CLOSE&&t->state!=SQL_TX_ACTIVE)return result(TW_DB_FAILED_NO_EFFECT);
    orm_error_t error;orm_error_init(&error);
    if(t->state==SQL_TX_ACTIVE) {
        if(op!=TW_DB_CLOSE&&call->interrupted(call->interrupt_context))
            return result(TW_DB_CANCELLED_NO_EFFECT);
        orm_status_t status=op==TW_DB_COMMIT?
            orm_transaction_commit(t->transaction,&error):
            orm_transaction_rollback(t->transaction,&error);
        if(status!=ORM_STATUS_OK) {
            if(op==TW_DB_COMMIT) {
                if(status==ORM_STATUS_COMMIT_UNKNOWN)t->state=SQL_TX_COMMIT_UNKNOWN;
            } else {
                t->state=SQL_TX_CLEANUP_BLOCKED;
            }
            return result(TW_DB_OUTCOME_UNKNOWN);
        }
        t->state=SQL_TX_ENDED;
    }
    if(orm_transaction_close(t->transaction,&error)!=ORM_STATUS_OK) {
        t->state=SQL_TX_CLEANUP_BLOCKED;
        return result(TW_DB_OUTCOME_UNKNOWN);
    }
    orm_transaction_release(t->transaction);
    p->slots[t->connection_index].transaction_active=false;
    *t=(sql_tx_slot){0};
    --p->live_transactions;
    return result(TW_DB_SUCCESS);
}

tw_db_worker_result tw_db_orm_execute(void *context,const tw_db_worker_call *call,
    uint8_t *bytes,size_t capacity) {
    tw_db_orm *provider=context;sql_impl *p=provider->impl;
    const void *lane=sync_thread_current_token();
    if(!p->lane)p->lane=lane;
    if(p->lane!=lane)abort();
    if(call->work.request.opcode==TW_DB_OPEN_SQL)return open_connection(p,call);
    uintptr_t owner=call->work.native_owner;
    if(call->work.native_kind==TW_DB_CURSOR) {
        if(!owner||owner>p->config.cursors)return result(TW_DB_FAILED_NO_EFFECT);
        sql_cursor_slot *c=&p->cursors[owner-1];
        if(!c->result.native||c->alias!=call->work.request.alias_index)
            return result(TW_DB_FAILED_NO_EFFECT);
        if(call->work.request.opcode==TW_DB_FETCH) {
            if(call->interrupted(call->interrupt_context))return result(TW_DB_CANCELLED_NO_EFFECT);
            size_t written=0;
            if(!tw_db_sql_result_fetch(&c->result,call->work.request.rows,
                p->limits.value_bytes,bytes,capacity,&written))return result(TW_DB_FAILED_NO_EFFECT);
            return (tw_db_worker_result){TW_DB_SUCCESS,0,written};
        }
        if(call->work.request.opcode==TW_DB_CLOSE) {
            tw_db_sql_result_close(&c->result);*c=(sql_cursor_slot){0};
            --p->live_cursors;
            return result(TW_DB_SUCCESS);
        }
        return result(TW_DB_FAILED_NO_EFFECT);
    }
    if(call->work.native_kind==TW_DB_TRANSACTION) {
        if(!owner||owner>p->config.transactions)return result(TW_DB_FAILED_NO_EFFECT);
        sql_tx_slot *t=&p->transactions[owner-1];
        if(!t->transaction)return result(TW_DB_FAILED_NO_EFFECT);
        sql_slot *s=&p->slots[t->connection_index];
        if(!s->connection||!s->transaction_active||s->alias!=call->work.request.alias_index)
            return result(TW_DB_FAILED_NO_EFFECT);
        if(call->work.request.opcode==TW_DB_COMMIT||
            call->work.request.opcode==TW_DB_ROLLBACK||call->work.request.opcode==TW_DB_CLOSE)
            return finish_transaction(p,t,call);
        if(t->state!=SQL_TX_ACTIVE)return result(TW_DB_FAILED_NO_EFFECT);
        if(call->work.request.opcode==TW_DB_EXECUTE||call->work.request.opcode==TW_DB_QUERY)
            return execute_sql(p,s,t->transaction,call,bytes,capacity);
        return result(TW_DB_FAILED_NO_EFFECT);
    }
    if(call->work.native_kind!=TW_DB_SQL||!owner||owner>p->config.connections)
        return result(TW_DB_FAILED_NO_EFFECT);
    sql_slot *s=&p->slots[owner-1];
    if(!s->connection||s->alias!=call->work.request.alias_index||s->transaction_active)
        return result(TW_DB_FAILED_NO_EFFECT);
    if(call->work.request.opcode==TW_DB_BEGIN)return begin_transaction(p,s,call);
    if(call->work.request.opcode==TW_DB_EXECUTE||call->work.request.opcode==TW_DB_QUERY)
        return execute_sql(p,s,NULL,call,bytes,capacity);
    if(call->work.request.opcode==TW_DB_CLOSE) {
        orm_error_t error;orm_error_init(&error);
        if(orm_connection_close(s->connection,&error)!=ORM_STATUS_OK)
            return result(TW_DB_OUTCOME_UNKNOWN);
        orm_connection_release(s->connection);*s=(sql_slot){0};
        if(--p->live==0)release_runtime(p);
        return result(TW_DB_SUCCESS);
    }
    return result(TW_DB_FAILED_NO_EFFECT);
}

tw_db_status tw_db_orm_destroy(tw_db_orm *provider) {
    sql_impl *p=provider->impl;
    if(!p)return TW_DB_OK;
    if(p->live||p->runtime||p->live_cursors||p->live_transactions)return TW_DB_BUSY;
    turbowasm_rt_free(p->transactions);turbowasm_rt_free(p->cursors);turbowasm_rt_free(p->slots);turbowasm_rt_free(p);provider->impl=NULL;
    return TW_DB_OK;
}
