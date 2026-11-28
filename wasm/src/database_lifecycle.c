#include "database_lifecycle.h"
#include "database_policy.h"
#include "runtime_alloc.h"
#include <string.h>

enum slot_state { SLOT_FREE, SLOT_RESERVED, SLOT_HELD, SLOT_LIVE, SLOT_QUARANTINED };
#define NO_SLOT SIZE_MAX

typedef struct resource_slot {
    uint64_t token;
    enum slot_state state;
    tw_db_kind kind;
    size_t alias, parent, children;
    bool active;
    uintptr_t native;
} resource_slot;

typedef struct operation_slot {
    uint64_t token, dispatch_token;
    bool discarding;
    tw_db_operation_state state;
    tw_db_request request;
    size_t owner, produced, request_size;
    tw_db_receipt receipt;
} operation_slot;

typedef struct manager_impl {
    tw_db_owned_policy policy;
    tw_db_capacity capacity;
    resource_slot *resources;
    operation_slot *operations;
    uint8_t *requests, *results;
    size_t resource_count;
    uint64_t next_token, shutdown_close, shutdown_resource;
    bool stopped;
} manager_impl;

static void destroy_impl(manager_impl *p) {
    tw_db_policy_destroy(&p->policy);
    turbowasm_rt_free(p->results);
    turbowasm_rt_free(p->requests);
    turbowasm_rt_free(p->operations);
    turbowasm_rt_free(p->resources);
    turbowasm_rt_free(p);
}

tw_db_status tw_db_manager_init(tw_db_manager *manager,
    const tw_db_policy *policy, const tw_db_capacity *capacity) {
    if (manager->impl || !policy || !capacity || !capacity->operations ||
        !capacity->result_bytes || !policy->limits.request_bytes)
        return TW_DB_INVALID_REQUEST;
    size_t budget=capacity->host_bytes;
    uint64_t count=(uint64_t)capacity->connections+capacity->transactions+capacity->cursors;
    if (count>SIZE_MAX ||
        !tw_db_budget_take(&budget,1,sizeof(manager_impl)) ||
        !tw_db_budget_take(&budget,(size_t)count,sizeof(resource_slot)) ||
        !tw_db_budget_take(&budget,capacity->operations,sizeof(operation_slot)) ||
        !tw_db_budget_take(&budget,capacity->operations,policy->limits.request_bytes) ||
        !tw_db_budget_take(&budget,capacity->operations,capacity->result_bytes))
        return TW_DB_LIMIT;
    manager_impl *p=turbowasm_rt_calloc(1,sizeof(*p));
    if (!p) return TW_DB_OUT_OF_MEMORY;
    p->capacity=*capacity;
    p->resource_count=(size_t)count;
    tw_db_status s=tw_db_policy_copy(&p->policy,policy,&budget);
    if (s!=TW_DB_OK) { destroy_impl(p); return s; }
    if (count) p->resources=turbowasm_rt_calloc((size_t)count,sizeof(*p->resources));
    p->operations=turbowasm_rt_calloc(capacity->operations,sizeof(*p->operations));
    p->requests=turbowasm_rt_calloc(capacity->operations,policy->limits.request_bytes);
    p->results=turbowasm_rt_calloc(capacity->operations,capacity->result_bytes);
    if ((count&&!p->resources)||!p->operations||!p->requests||!p->results) {
        destroy_impl(p); return TW_DB_OUT_OF_MEMORY;
    }
    p->next_token=1;
    manager->impl=p;
    return TW_DB_OK;
}

void tw_db_manager_stop(tw_db_manager *manager) {
    ((manager_impl *)manager->impl)->stopped=true;
}

tw_db_status tw_db_manager_destroy(tw_db_manager *manager) {
    manager_impl *p=manager->impl;
    if (!p) return TW_DB_OK;
    for (size_t i=0;i<p->resource_count;++i)
        if (p->resources[i].state!=SLOT_FREE) return TW_DB_BUSY;
    for (size_t i=0;i<p->capacity.operations;++i)
        if (p->operations[i].token) return TW_DB_BUSY;
    destroy_impl(p); manager->impl=NULL;
    return TW_DB_OK;
}

static size_t find_resource(const manager_impl *p,uint64_t token) {
    if (token) for(size_t i=0;i<p->resource_count;++i) {
        const resource_slot *r=&p->resources[i];
        if (r->token==token && (r->state==SLOT_LIVE||r->state==SLOT_QUARANTINED))
            return i;
    }
    return NO_SLOT;
}

static size_t find_operation(const manager_impl *p,uint64_t token) {
    if (token) for(size_t i=0;i<p->capacity.operations;++i)
        if (p->operations[i].token==token) return i;
    return NO_SLOT;
}

static size_t free_operation(const manager_impl *p) {
    for(size_t i=0;i<p->capacity.operations;++i)
        if (!p->operations[i].token) return i;
    return NO_SLOT;
}

static bool connection(tw_db_kind kind) {
    return kind==TW_DB_SQL||kind==TW_DB_REDIS;
}

static size_t free_resource(const manager_impl *p,tw_db_kind kind) {
    size_t free_slot=NO_SLOT, used=0;
    size_t max=connection(kind)?p->capacity.connections:
        kind==TW_DB_TRANSACTION?p->capacity.transactions:p->capacity.cursors;
    for(size_t i=0;i<p->resource_count;++i) {
        const resource_slot *r=&p->resources[i];
        if (r->state==SLOT_FREE) { if(free_slot==NO_SLOT) free_slot=i; }
        else if (connection(kind)?connection(r->kind):r->kind==kind) ++used;
    }
    return used<max?free_slot:NO_SLOT;
}

static tw_db_kind produced_kind(uint32_t opcode) {
    switch(opcode) {
    case TW_DB_OPEN_SQL: return TW_DB_SQL;
    case TW_DB_OPEN_REDIS: return TW_DB_REDIS;
    case TW_DB_BEGIN: return TW_DB_TRANSACTION;
    case TW_DB_QUERY: return TW_DB_CURSOR;
    default: return TW_DB_NONE;
    }
}

static bool consumes(tw_db_opcode opcode) {
    return opcode==TW_DB_CLOSE||opcode==TW_DB_COMMIT||opcode==TW_DB_ROLLBACK;
}

static uint8_t *request_bytes(manager_impl *p,size_t i) {
    return p->requests+i*p->policy.policy.limits.request_bytes;
}
static uint8_t *result_bytes(manager_impl *p,size_t i) {
    return p->results+i*p->capacity.result_bytes;
}

tw_db_status tw_db_manager_submit(tw_db_manager *manager,
    uint64_t owner,uint32_t opcode,const uint8_t *bytes,size_t size,
    uint64_t *out_operation) {
    manager_impl *p=manager->impl;
    if (p->stopped && opcode!=TW_DB_CLOSE) return TW_DB_WRONG_STATE;
    if (size>p->policy.policy.limits.request_bytes) return TW_DB_LIMIT;
    if (size&&!bytes) return TW_DB_INVALID_REQUEST;
    size_t ri=NO_SLOT;
    tw_db_owner resolved={TW_DB_NONE,0};
    if (owner) {
        ri=find_resource(p,owner);
        if (ri==NO_SLOT) return TW_DB_INVALID_HANDLE;
        const resource_slot *r=&p->resources[ri];
        if (r->active||r->children) return TW_DB_BUSY;
        if (r->state==SLOT_QUARANTINED&&opcode!=TW_DB_CLOSE)
            return TW_DB_WRONG_STATE;
        resolved=(tw_db_owner){r->kind,r->alias};
    }
    size_t oi=free_operation(p);
    if (oi==NO_SLOT) return TW_DB_LIMIT;
    uint8_t *copy=request_bytes(p,oi);
    if (size) memmove(copy,bytes,size);
    tw_db_request q={0};
    tw_db_status s=tw_db_admit(&p->policy.policy,resolved,opcode,copy,size,&q);
    if (s!=TW_DB_OK) { if(size) memset(copy,0,size); return s; }
    tw_db_kind kind=produced_kind(opcode);
    size_t child=NO_SLOT;
    if (kind!=TW_DB_NONE) child=free_resource(p,kind);
    /* Keep zero reserved as invalid; stop before counter wrap. */
    uint64_t ids=kind==TW_DB_NONE?1:2;
    if ((kind!=TW_DB_NONE&&child==NO_SLOT)||p->next_token>UINT64_MAX-ids) {
        if(size) memset(copy,0,size);
        return TW_DB_LIMIT;
    }
    operation_slot *o=&p->operations[oi];
    *o=(operation_slot){.token=p->next_token,.dispatch_token=p->next_token,.state=TW_DB_ADMITTED,
        .request=q,.owner=ri,.produced=child,.request_size=size};
    ++p->next_token;
    if (ri!=NO_SLOT) p->resources[ri].active=true;
    if (child!=NO_SLOT) {
        p->resources[child]=(resource_slot){.token=p->next_token++,
            .state=SLOT_RESERVED,.kind=kind,.alias=q.alias_index,.parent=ri};
        if (ri!=NO_SLOT) ++p->resources[ri].children;
    }
    *out_operation=o->token;
    return TW_DB_OK;
}

tw_db_status tw_db_manager_start(tw_db_manager *manager,uint64_t operation,tw_db_work *out) {
    manager_impl *p=manager->impl;
    size_t i=find_operation(p,operation);
    if(i==NO_SLOT) return TW_DB_INVALID_HANDLE;
    operation_slot *o=&p->operations[i];
    if(o->state!=TW_DB_ADMITTED) return TW_DB_WRONG_STATE;
    o->state=TW_DB_RUNNING;
    *out=(tw_db_work){o->dispatch_token,o->request,
        o->owner==NO_SLOT?0:p->resources[o->owner].native,
        o->owner==NO_SLOT?TW_DB_NONE:p->resources[o->owner].kind};
    return TW_DB_OK;
}

tw_db_status tw_db_manager_poll(tw_db_manager *manager,uint64_t operation,
    tw_db_operation_state *out) {
    manager_impl *p=manager->impl;
    size_t i=find_operation(p,operation);
    if(i==NO_SLOT) return TW_DB_INVALID_HANDLE;
    *out=p->operations[i].state;
    return TW_DB_OK;
}

/* Only drained resources are removed. Child counts pin their parent slots
 * even before publication; a parent cannot be reused while a child exists. */
static void retire_resource(manager_impl *p,size_t index) {
    resource_slot *r=&p->resources[index];
    if(r->parent!=NO_SLOT) --p->resources[r->parent].children;
    *r=(resource_slot){0};
}

static void quarantine(manager_impl *p,size_t index) {
    while(index!=NO_SLOT) {
        resource_slot *r=&p->resources[index];
        r->state=SLOT_QUARANTINED;
        index=r->parent;
    }
}

static void finish(manager_impl *p,operation_slot *o,tw_db_outcome outcome,
    uintptr_t native_resource,size_t size) {
    if(o->produced!=NO_SLOT) {
        if(outcome==TW_DB_SUCCESS) {
            resource_slot *r=&p->resources[o->produced];
            r->state=SLOT_HELD;
            r->native=native_resource;
        } else {
            retire_resource(p,o->produced);
            o->produced=NO_SLOT;
        }
    }
    if(o->owner!=NO_SLOT) {
        size_t i=o->owner;
        p->resources[i].active=false;
        if(outcome==TW_DB_SUCCESS&&consumes(o->request.opcode)) retire_resource(p,i);
        else if(outcome==TW_DB_OUTCOME_UNKNOWN) quarantine(p,i);
        o->owner=NO_SLOT;
    }
    o->receipt=(tw_db_receipt){outcome,size};
    o->state=TW_DB_TERMINAL;
}

tw_db_status tw_db_manager_complete(tw_db_manager *manager,uint64_t operation,
    tw_db_outcome outcome,uintptr_t native_resource,const uint8_t *bytes,size_t size) {
    manager_impl *p=manager->impl;
    size_t i=NO_SLOT;
    if(operation) for(size_t n=0;n<p->capacity.operations;++n)
        if(p->operations[n].token&&p->operations[n].dispatch_token==operation) {i=n;break;}
    if(i==NO_SLOT) return TW_DB_INVALID_HANDLE;
    operation_slot *o=&p->operations[i];
    if(o->state!=TW_DB_RUNNING&&o->state!=TW_DB_CANCEL_REQUESTED)
        return TW_DB_WRONG_STATE;
    if(o->discarding) {
        if(outcome<TW_DB_SUCCESS||outcome>TW_DB_OUTCOME_UNKNOWN||native_resource||size)
            return TW_DB_INVALID_REQUEST;
        if(outcome==TW_DB_SUCCESS) {
            retire_resource(p,o->produced);
            o->produced=NO_SLOT;
        } else if(outcome==TW_DB_OUTCOME_UNKNOWN) {
            /* Keep the unpublished resource HELD, not discoverable by a
               guessed guest token. Only its ancestors are quarantined. */
            quarantine(p,p->resources[o->produced].parent);
        }
        o->receipt=(tw_db_receipt){outcome,0};
        o->state=TW_DB_TERMINAL;
        return TW_DB_OK;
    }
    if(outcome<TW_DB_SUCCESS||outcome>TW_DB_OUTCOME_UNKNOWN||
        (size&&!bytes)||
        ((outcome==TW_DB_SUCCESS&&o->produced!=NO_SLOT)!=(native_resource!=0))||
        (outcome!=TW_DB_SUCCESS&&size))
        return TW_DB_INVALID_REQUEST;
    if(size>p->capacity.result_bytes) return TW_DB_LIMIT;
    if(size) memmove(result_bytes(p,i),bytes,size);
    finish(p,o,outcome,native_resource,size);
    return TW_DB_OK;
}

tw_db_status tw_db_manager_cancel(tw_db_manager *manager,uint64_t operation,
    bool *out_notify) {
    manager_impl *p=manager->impl;
    size_t i=find_operation(p,operation);
    if(i==NO_SLOT) return TW_DB_INVALID_HANDLE;
    operation_slot *o=&p->operations[i];
    if(o->discarding) return TW_DB_WRONG_STATE;
    bool notify=false;
    switch(o->state) {
    case TW_DB_ADMITTED:
        finish(p,o,TW_DB_CANCELLED_NO_EFFECT,0,0);
        break;
    case TW_DB_RUNNING:
        o->state=TW_DB_CANCEL_REQUESTED;
        notify=true;
        break;
    case TW_DB_CANCEL_REQUESTED: case TW_DB_TERMINAL:
        break;
    default: return TW_DB_WRONG_STATE;
    }
    *out_notify=notify;
    return TW_DB_OK;
}

tw_db_status tw_db_manager_receipt(tw_db_manager *manager,uint64_t operation,
    tw_db_receipt *out) {
    manager_impl *p=manager->impl;
    size_t i=find_operation(p,operation);
    if(i==NO_SLOT) return TW_DB_INVALID_HANDLE;
    const operation_slot *o=&p->operations[i];
    if(o->state!=TW_DB_TERMINAL) return TW_DB_PENDING;
    *out=o->receipt;
    return TW_DB_OK;
}

tw_db_status tw_db_manager_read(tw_db_manager *manager,uint64_t operation,
    size_t offset,uint8_t *dst,size_t capacity,size_t *out_copied) {
    manager_impl *p=manager->impl;
    size_t i=find_operation(p,operation);
    if(i==NO_SLOT) return TW_DB_INVALID_HANDLE;
    const operation_slot *o=&p->operations[i];
    if(o->state!=TW_DB_TERMINAL) return TW_DB_PENDING;
    if(offset>o->receipt.size) return TW_DB_INVALID_REQUEST;
    size_t n=o->receipt.size-offset;
    if(n>capacity) n=capacity;
    if(n&&!dst) return TW_DB_INVALID_REQUEST;
    if(n) memmove(dst,result_bytes(p,i)+offset,n);
    *out_copied=n;
    return TW_DB_OK;
}

static tw_db_status held_resource(manager_impl *p,uint64_t token,operation_slot **out) {
    size_t i=find_operation(p,token);
    if(i==NO_SLOT) return TW_DB_INVALID_HANDLE;
    operation_slot *o=&p->operations[i];
    if(o->state!=TW_DB_TERMINAL) return TW_DB_PENDING;
    if(o->produced==NO_SLOT) return TW_DB_WRONG_STATE;
    *out=o;
    return TW_DB_OK;
}

tw_db_status tw_db_manager_take(tw_db_manager *manager,uint64_t operation,
    uint64_t *out_resource) {
    manager_impl *p=manager->impl;
    operation_slot *o;
    tw_db_status s=held_resource(p,operation,&o);
    if(s!=TW_DB_OK) return s;
    if(o->discarding) return TW_DB_WRONG_STATE;
    resource_slot *r=&p->resources[o->produced];
    r->state=SLOT_LIVE;
    *out_resource=r->token;
    o->produced=NO_SLOT;
    return TW_DB_OK;
}


tw_db_status tw_db_manager_begin_discard(tw_db_manager *manager,uint64_t operation,
    tw_db_work *out) {
    manager_impl *p=manager->impl;
    operation_slot *o;
    tw_db_status s=held_resource(p,operation,&o);
    if(s!=TW_DB_OK) return s;
    if(o->discarding) return TW_DB_WRONG_STATE;
    if(p->next_token==UINT64_MAX) return TW_DB_LIMIT;
    size_t i=(size_t)(o-p->operations);
    resource_slot *r=&p->resources[o->produced];
    /* The guest explicitly relinquished the original result. Reuse its
       reserved slot, but never its native completion identity. */
    if(o->request_size) memset(request_bytes(p,i),0,o->request_size);
    if(o->receipt.size) memset(result_bytes(p,i),0,o->receipt.size);
    o->request_size=0;
    o->receipt=(tw_db_receipt){0};
    o->request=(tw_db_request){.opcode=TW_DB_CLOSE,.alias_index=r->alias};
    o->dispatch_token=p->next_token++;
    o->discarding=true;
    o->state=TW_DB_RUNNING;
    *out=(tw_db_work){o->dispatch_token,o->request,r->native,r->kind};
    return TW_DB_OK;
}

tw_db_status tw_db_manager_untaken(tw_db_manager *manager,uint64_t operation,
    uintptr_t *out_native) {
    manager_impl *p=manager->impl;
    operation_slot *o;
    tw_db_status s=held_resource(p,operation,&o);
    if(s!=TW_DB_OK) return s;
    *out_native=p->resources[o->produced].native;
    return TW_DB_OK;
}

tw_db_status tw_db_manager_disposed(tw_db_manager *manager,uint64_t operation) {
    manager_impl *p=manager->impl;
    operation_slot *o;
    tw_db_status s=held_resource(p,operation,&o);
    if(s!=TW_DB_OK) return s;
    retire_resource(p,o->produced);
    o->produced=NO_SLOT;
    return TW_DB_OK;
}

tw_db_status tw_db_manager_drop(tw_db_manager *manager,uint64_t operation) {
    manager_impl *p=manager->impl;
    size_t i=find_operation(p,operation);
    if(i==NO_SLOT) return TW_DB_INVALID_HANDLE;
    operation_slot *o=&p->operations[i];
    if(o->state!=TW_DB_TERMINAL||o->produced!=NO_SLOT) return TW_DB_BUSY;
    if(o->request_size) memset(request_bytes(p,i),0,o->request_size);
    if(o->receipt.size) memset(result_bytes(p,i),0,o->receipt.size);
    *o=(operation_slot){0};
    return TW_DB_OK;
}

tw_db_status tw_db_manager_request_drop(tw_db_manager *manager,
    uint64_t operation,tw_db_work *out_work) {
    manager_impl *p=manager->impl;
    *out_work=(tw_db_work){0};
    size_t i=find_operation(p,operation);
    if(i==NO_SLOT) return TW_DB_INVALID_HANDLE;
    operation_slot *o=&p->operations[i];
    if(o->state!=TW_DB_TERMINAL) return o->discarding?TW_DB_PENDING:TW_DB_BUSY;
    if(o->produced==NO_SLOT) return tw_db_manager_drop(manager,operation);
    if(o->discarding) return TW_DB_WRONG_STATE;
    tw_db_status s=tw_db_manager_begin_discard(manager,operation,out_work);
    return s==TW_DB_OK?TW_DB_PENDING:s;
}

tw_db_status tw_db_manager_shutdown_next(tw_db_manager *manager,tw_db_shutdown_action *out) {
    manager_impl *p=manager->impl;
    if(!p->stopped) return TW_DB_WRONG_STATE;
    tw_db_shutdown_action a={0};
    /* A close initiated by shutdown is never replayed, even if it failed
       without effect. Keep its receipt and owner available to host policy. */
    if(p->shutdown_close) {
        a.operation=p->shutdown_close;
        tw_db_status s=tw_db_manager_receipt(manager,a.operation,&a.receipt);
        if(s==TW_DB_PENDING) {a.kind=TW_DB_SHUTDOWN_WAIT;*out=a;return TW_DB_OK;}
        if(s!=TW_DB_OK) return s;
        if(a.receipt.outcome!=TW_DB_SUCCESS) {
            a.kind=TW_DB_SHUTDOWN_BLOCKED;*out=a;return TW_DB_OK;
        }
        s=tw_db_manager_drop(manager,a.operation);
        if(s!=TW_DB_OK) return s;
        p->shutdown_close=0;
        p->shutdown_resource=0;
        a.kind=TW_DB_SHUTDOWN_RETIRED;*out=a;return TW_DB_OK;
    }

    /* Request cancellation once for every admitted business operation.
       Cleanup work is not cancelled. Drain all before touching resources. */
    bool pending=false;
    for(size_t i=0;i<p->capacity.operations;++i) {
        operation_slot *o=&p->operations[i];
        if(!o->token||o->state==TW_DB_TERMINAL) continue;
        if(o->discarding||o->state==TW_DB_CANCEL_REQUESTED) {pending=true;continue;}
        a.operation=o->token;
        bool notify=false;
        tw_db_status s=tw_db_manager_cancel(manager,a.operation,&notify);
        if(s!=TW_DB_OK) return s;
        if(notify) a.kind=TW_DB_SHUTDOWN_CANCEL;
        else {
            /* Admitted, not dispatched: its cancelled receipt can be retired
               now, not silently converted into a WAIT action. */
            s=tw_db_manager_receipt(manager,a.operation,&a.receipt);
            if(s!=TW_DB_OK) return s;
            s=tw_db_manager_drop(manager,a.operation);
            if(s!=TW_DB_OK) return s;
            a.kind=TW_DB_SHUTDOWN_RETIRED;
        }
        *out=a;return TW_DB_OK;
    }
    if(pending) {a.kind=TW_DB_SHUTDOWN_WAIT;*out=a;return TW_DB_OK;}

    /* Reclaim terminal operations, including unpublished resources. These
       retain their parent until their separate native close has drained. */
    for(size_t i=0;i<p->capacity.operations;++i) {
        operation_slot *o=&p->operations[i];
        if(!o->token) continue;
        a.operation=o->token;
        a.receipt=o->receipt;
        if(o->produced!=NO_SLOT) {
            if(o->discarding) {a.kind=TW_DB_SHUTDOWN_BLOCKED;*out=a;return TW_DB_OK;}
            tw_db_status s=tw_db_manager_begin_discard(manager,a.operation,&a.work);
            if(s!=TW_DB_OK) return s;
            a.kind=TW_DB_SHUTDOWN_DISPATCH;
        } else {
            tw_db_status s=tw_db_manager_drop(manager,a.operation);
            if(s!=TW_DB_OK) return s;
            a.kind=TW_DB_SHUTDOWN_RETIRED;
        }
        *out=a;return TW_DB_OK;
    }

    /* Only live, unpinned leaves are eligible. A child close consumes its
       native identity before the parent can be submitted. */
    bool remaining=false;
    for(size_t i=0;i<p->resource_count;++i) {
        resource_slot *r=&p->resources[i];
        if(r->state==SLOT_FREE) continue;
        remaining=true;
        if((r->state!=SLOT_LIVE&&r->state!=SLOT_QUARANTINED)||r->children||r->active)
            continue;
        tw_db_status s=tw_db_manager_submit(manager,r->token,TW_DB_CLOSE,NULL,0,&a.operation);
        if(s!=TW_DB_OK) return s;
        p->shutdown_close=a.operation;
        p->shutdown_resource=r->token;
        s=tw_db_manager_start(manager,a.operation,&a.work);
        if(s!=TW_DB_OK) return s;
        a.kind=TW_DB_SHUTDOWN_DISPATCH;*out=a;return TW_DB_OK;
    }
    if(remaining) return TW_DB_WRONG_STATE; /* impossible owner graph */
    a.kind=TW_DB_SHUTDOWN_DONE;*out=a;return TW_DB_OK;
}

tw_db_status tw_db_manager_confirm_shutdown_close(tw_db_manager *manager,uint64_t operation) {
    manager_impl *p=manager->impl;
    if(!p->stopped||!operation||operation!=p->shutdown_close)
        return TW_DB_WRONG_STATE;
    tw_db_receipt receipt;
    tw_db_status s=tw_db_manager_receipt(manager,operation,&receipt);
    if(s!=TW_DB_OK) return s;
    if(receipt.outcome==TW_DB_SUCCESS) return TW_DB_WRONG_STATE;
    size_t i=find_resource(p,p->shutdown_resource);
    if(i==NO_SLOT||p->resources[i].active||p->resources[i].children)
        return TW_DB_WRONG_STATE;
    retire_resource(p,i);
    /* Preserve the original failure for the next RETIRED audit action. */
    p->shutdown_close=0;
    p->shutdown_resource=0;
    return TW_DB_OK;
}
