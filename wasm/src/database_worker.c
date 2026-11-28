#include "database_worker.h"
#include "database_policy.h"
#include "runtime_alloc.h"
#include <cflow/executor.h>
#include <core/clock.h>
#include <threads/threads.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct worker_impl worker_impl;
typedef struct worker_slot {
    worker_impl *worker;
    tw_db_worker_call call;
    tw_db_worker_result result;
    atomic_bool cancelled;
    bool occupied; /* owner only */
} worker_slot;

struct worker_impl {
    tw_db_bridge *bridge;
    tw_db_worker_config config;
    cflow_executor executor;
    cflow_mailbox completions;
    worker_slot *slots;
    uint8_t *results;
    const void *owner;
    tw_db_status fault;
};

static bool owner(const worker_impl *p) {
    return p->owner==sync_thread_current_token();
}

static uint8_t *result_bytes(worker_slot *s) {
    return s->worker->results+(size_t)(s-s->worker->slots)*s->worker->config.result_bytes;
}

static bool interrupted(void *context) {
    worker_slot *s=context;
    return atomic_load_explicit(&s->cancelled,memory_order_acquire)||
        hrtime()>=s->call.deadline_ns;
}

static void run(void *context) {
    worker_slot *s=context;
    if(interrupted(s)) {
        /* No provider callback was entered: cancellation is known no-effect. */
        s->result=(tw_db_worker_result){TW_DB_CANCELLED_NO_EFFECT,0,0};
        return;
    }
    s->result=s->worker->config.execute(s->worker->config.context,
        &s->call,result_bytes(s),s->worker->config.result_bytes);
}

static void cancelled(void *context) {
    worker_slot *s=context;
    s->result=(tw_db_worker_result){TW_DB_CANCELLED_NO_EFFECT,0,0};
}

static void finalized(void *context) {
    worker_slot *s=context;
    worker_impl *p=s->worker;
    uint64_t index=(uint64_t)(s-p->slots);
    const cflow_event_view event={1,&cmeta_type_uint64,&index};
    /* Every occupied slot owns one mailbox position. There is one producer
       per task and no other mailbox sender. Failure is an invariant breach.
       After publishing, do not touch the slot: the owner may reuse it. */
    if(cflow_mailbox_try_send(&p->completions,&event)!=CFLOW_MAILBOX_OK)
        abort();
}

static void fail_admission(worker_impl *p,uint64_t operation) {
    /* The provider was never entered, including a full/closed executor.
       Do not claim an accepted native effect or automatically retry. */
    if(tw_db_bridge_complete(p->bridge,operation,TW_DB_FAILED_NO_EFFECT,0,NULL,0)!=TW_DB_OK)
        abort();
}

static void dispatch_start(void *context,tw_db_bridge *bridge,const tw_db_work *work) {
    worker_impl *p=context;
    if(!owner(p)||bridge!=p->bridge) abort();
    if(p->fault!=TW_DB_OK) {fail_admission(p,work->operation);return;}
    worker_slot *s=NULL;
    for(size_t i=0;i<p->config.operations;++i) {
        if(!p->slots[i].occupied) {s=&p->slots[i];break;}
    }
    const uint64_t now=hrtime();
    if(!s||p->config.timeout_ns>UINT64_MAX-now) {
        fail_admission(p,work->operation);return;
    }
    s->occupied=true;
    s->call=(tw_db_worker_call){*work,now+p->config.timeout_ns,interrupted,s};
    s->result=(tw_db_worker_result){0};
    atomic_store_explicit(&s->cancelled,false,memory_order_release);
    const cflow_executor_task task={run,cancelled,finalized,s};
    if(cflow_executor_try_post_task(&p->executor,&task)!=CFLOW_ADMISSION_ACCEPTED) {
        s->occupied=false;
        fail_admission(p,work->operation);
    }
}

static void dispatch_cancel(void *context,tw_db_bridge *bridge,uint64_t operation) {
    worker_impl *p=context;
    if(!owner(p)||bridge!=p->bridge) abort();
    for(size_t i=0;i<p->config.operations;++i) {
        worker_slot *s=&p->slots[i];
        if(s->occupied&&s->call.work.operation==operation) {
            atomic_store_explicit(&s->cancelled,true,memory_order_release);
            return;
        }
    }
    /* The bridge notifies only a still-running admitted native dispatch. */
    abort();
}

tw_db_status tw_db_worker_init(tw_db_worker *worker,tw_db_bridge *bridge,
    const tw_db_worker_config *config,tw_db_dispatch *out_dispatch) {
    if(!worker||worker->impl||!bridge||bridge->impl||!config||!out_dispatch||
        !config->execute||!config->operations||!config->result_bytes||
        !config->timeout_ns) return TW_DB_INVALID_REQUEST;
    size_t budget=config->host_bytes;
    if(!tw_db_budget_take(&budget,1,sizeof(worker_impl))||
        !tw_db_budget_take(&budget,config->operations,sizeof(worker_slot))||
        !tw_db_budget_take(&budget,config->operations,config->result_bytes)||
        (uint64_t)config->operations+1u>SIZE_MAX) return TW_DB_LIMIT;
    worker_impl *p=turbowasm_rt_calloc(1,sizeof(*p));
    if(!p)return TW_DB_OUT_OF_MEMORY;
    p->bridge=bridge;p->config=*config;p->owner=sync_thread_current_token();
    p->slots=turbowasm_rt_calloc(config->operations,sizeof(*p->slots));
    p->results=turbowasm_rt_calloc(config->operations,config->result_bytes);
    if(!p->slots||!p->results)goto failed;
    for(size_t i=0;i<config->operations;++i) {
        p->slots[i].worker=p;
        atomic_init(&p->slots[i].cancelled,false);
    }
    const cflow_event_type schema={1,&cmeta_type_uint64};
    if(cflow_mailbox_init(&p->completions,&schema,1,config->operations)!=CFLOW_MAILBOX_OK)
        goto failed;
    /* One extra executor reservation accommodates the last finalize return
       tail after its record was consumed. It is not another database slot. */
    if(!cflow_executor_serial_init_with_capacity(&p->executor,(size_t)config->operations+1u)) {
        cflow_mailbox_destroy(&p->completions);goto failed;
    }
    worker->impl=p;
    *out_dispatch=(tw_db_dispatch){p,dispatch_start,dispatch_cancel};
    return TW_DB_OK;
failed:
    turbowasm_rt_free(p->results);
    turbowasm_rt_free(p->slots);
    turbowasm_rt_free(p);
    return TW_DB_OUT_OF_MEMORY;
}

cflow_waitable tw_db_worker_waitable(tw_db_worker *worker) {
    worker_impl *p=worker->impl;
    return cflow_mailbox_as_waitable(&p->completions);
}

tw_db_status tw_db_worker_progress(tw_db_worker *worker) {
    worker_impl *p=worker->impl;
    if(!owner(p))return TW_DB_WRONG_STATE;
    if(p->fault!=TW_DB_OK)return p->fault;
    uint64_t index=0;
    cflow_event_id id=0;
    const cmeta_type_desc *type=NULL;
    cflow_mailbox_status s=cflow_mailbox_try_receive(&p->completions,&id,&type,&index,sizeof index);
    if(s==CFLOW_MAILBOX_EMPTY)return TW_DB_PENDING;
    if(s!=CFLOW_MAILBOX_OK||id!=1||index>=p->config.operations||!p->slots[index].occupied)
        abort();
    worker_slot *slot=&p->slots[index];
    tw_db_worker_result receipt=slot->result;
    if(receipt.size>p->config.result_bytes) {
        p->fault=TW_DB_LIMIT;return p->fault;
    }
    tw_db_status status=tw_db_bridge_complete(p->bridge,slot->call.work.operation,
        receipt.outcome,receipt.native_resource,result_bytes(slot),receipt.size);
    if(status!=TW_DB_OK) {p->fault=status;return status;}
    if(receipt.size)memset(result_bytes(slot),0,receipt.size);
    slot->occupied=false;
    return TW_DB_OK;
}

tw_db_status tw_db_worker_destroy(tw_db_worker *worker) {
    worker_impl *p=worker->impl;
    if(!p)return TW_DB_OK;
    if(!owner(p))return TW_DB_WRONG_STATE;
    for(size_t i=0;i<p->config.operations;++i)
        if(p->slots[i].occupied)return TW_DB_BUSY;
    if(!cflow_executor_shutdown(&p->executor)||!cflow_executor_wait_idle(&p->executor))
        return TW_DB_BUSY;
    cflow_executor_destroy(&p->executor);
    cflow_mailbox_destroy(&p->completions);
    turbowasm_rt_free(p->results);
    turbowasm_rt_free(p->slots);
    turbowasm_rt_free(p);
    worker->impl=NULL;
    return TW_DB_OK;
}
