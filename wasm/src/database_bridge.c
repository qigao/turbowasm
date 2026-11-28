#include "database_bridge.h"
#include "database_policy.h"
#include "runtime_alloc.h"
#include <string.h>

typedef struct bridge_impl {
    tw_db_manager manager;
    tw_db_dispatch dispatch;
    const void *instance_identity;
    void *execution_identity;
    uint8_t *scratch;
    size_t request_capacity,result_capacity;
    bool bound,stopped,detached,inside_resume,notified;
    uint64_t waiting;
    turbowasm_host_wait wait;
} bridge_impl;

tw_db_status tw_db_bridge_init(tw_db_bridge *bridge,const tw_db_policy *policy,
    const tw_db_capacity *capacity,const tw_db_dispatch *dispatch) {
    if (!bridge||bridge->impl||!policy||!capacity||!dispatch||
        !dispatch->start||!dispatch->cancel||!policy->limits.request_bytes||
        !capacity->result_bytes) return TW_DB_INVALID_REQUEST;
    tw_db_capacity manager_capacity=*capacity;
    size_t scratch=policy->limits.request_bytes;
    if (scratch<capacity->result_bytes) scratch=capacity->result_bytes;
    if (!tw_db_budget_take(&manager_capacity.host_bytes,1,sizeof(bridge_impl))||
        !tw_db_budget_take(&manager_capacity.host_bytes,1,scratch)) return TW_DB_LIMIT;
    bridge_impl *p=turbowasm_rt_calloc(1,sizeof(*p));
    if (!p) return TW_DB_OUT_OF_MEMORY;
    p->scratch=turbowasm_rt_malloc(scratch);
    if (!p->scratch) { turbowasm_rt_free(p); return TW_DB_OUT_OF_MEMORY; }
    tw_db_status s=tw_db_manager_init(&p->manager,policy,&manager_capacity);
    if (s!=TW_DB_OK) {
        turbowasm_rt_free(p->scratch); turbowasm_rt_free(p); return s;
    }
    p->dispatch=*dispatch;
    p->request_capacity=policy->limits.request_bytes;
    p->result_capacity=capacity->result_bytes;
    bridge->impl=p;
    return TW_DB_OK;
}

tw_db_status tw_db_bridge_bind(tw_db_bridge *bridge,const turbowasm_instance *instance) {
    bridge_impl *p=bridge->impl;
    if (!instance||!instance->impl||p->bound||p->stopped||p->detached)
        return TW_DB_WRONG_STATE;
    p->instance_identity=instance->impl;
    p->bound=true;
    return TW_DB_OK;
}

static bool authorized(bridge_impl *p,turbowasm_host_call *call) {
    const turbowasm_instance *caller=turbowasm_host_call_instance(call);
    return p->bound&&!p->stopped&&!p->detached&&caller&&
        caller->impl==p->instance_identity;
}

static void cancel(tw_db_bridge *b,uint64_t operation,tw_db_status *out) {
    bridge_impl *p=b->impl;
    bool notify=false;
    *out=tw_db_manager_cancel(&p->manager,operation,&notify);
    if (*out==TW_DB_OK&&notify) p->dispatch.cancel(p->dispatch.context,b,operation);
}

tw_db_status tw_db_bridge_complete(tw_db_bridge *bridge,uint64_t operation,
    tw_db_outcome outcome,uintptr_t native_resource,const uint8_t *bytes,size_t size) {
    bridge_impl *p=bridge->impl;
    return tw_db_manager_complete(&p->manager,operation,outcome,native_resource,bytes,size);
}

turbowasm_status tw_db_bridge_resume(tw_db_bridge *bridge,turbowasm_execution *execution,
    const turbowasm_execution_options *options) {
    bridge_impl *p=bridge->impl;
    if (!p->bound||p->detached||!execution||!execution->impl||p->inside_resume||
        (p->execution_identity&&p->execution_identity!=execution->impl))
        return TURBOWASM_INVALID_ARGUMENT;
    /* The trusted host supplies an execution of the bound instance. Imports
       independently verify the live caller identity before touching state. */
    p->execution_identity=execution->impl;
    p->inside_resume=true;
    turbowasm_status status=turbowasm_execution_resume(execution,options);
    p->inside_resume=false;
    if(status!=TURBOWASM_YIELDED) p->execution_identity=NULL;
    return status;
}

tw_db_status tw_db_bridge_wake(tw_db_bridge *bridge,turbowasm_execution *execution) {
    bridge_impl *p=bridge->impl;
    if (p->inside_resume||!execution||!execution->impl||
        p->execution_identity!=execution->impl||!p->waiting||p->notified)
        return TW_DB_WRONG_STATE;
    tw_db_receipt receipt;
    tw_db_status s=tw_db_manager_receipt(&p->manager,p->waiting,&receipt);
    if(s!=TW_DB_OK) return s;
    turbowasm_host_wait actual;
    if(!turbowasm_execution_pending_host_wait(execution,&actual)||
        actual.generation!=p->wait.generation||
        actual.operation_token!=p->wait.operation_token)
        return TW_DB_WRONG_STATE;
    if(turbowasm_execution_complete_host_wait(execution,p->wait,0)!=TURBOWASM_OK)
        return TW_DB_WRONG_STATE;
    p->notified=true;
    return TW_DB_OK;
}

tw_db_status tw_db_bridge_destroy_execution(tw_db_bridge *bridge,turbowasm_execution *execution) {
    bridge_impl *p=bridge->impl;
    if(!execution||p->inside_resume||
        (p->execution_identity&&p->execution_identity!=execution->impl))
        return TW_DB_WRONG_STATE;
    turbowasm_execution_destroy(execution);
    p->execution_identity=NULL;
    return TW_DB_OK;
}

void tw_db_bridge_stop(tw_db_bridge *bridge) {
    bridge_impl *p=bridge->impl;
    p->stopped=true;
    tw_db_manager_stop(&p->manager);
}

tw_db_status tw_db_bridge_detach(tw_db_bridge *bridge) {
    bridge_impl *p=bridge->impl;
    if(!p->stopped||p->inside_resume||p->execution_identity||p->waiting)
        return TW_DB_BUSY;
    p->instance_identity=NULL;
    p->detached=true;
    return TW_DB_OK;
}

tw_db_manager *tw_db_bridge_cleanup_manager(tw_db_bridge *bridge) {
    bridge_impl *p=bridge->impl;
    return p->detached?&p->manager:NULL;
}

tw_db_status tw_db_bridge_destroy(tw_db_bridge *bridge) {
    bridge_impl *p=bridge->impl;
    if(!p) return TW_DB_OK;
    if((p->bound&&!p->detached)||p->execution_identity||p->waiting)
        return TW_DB_BUSY;
    tw_db_status s=tw_db_manager_destroy(&p->manager);
    if(s!=TW_DB_OK) return s;
    turbowasm_rt_free(p->scratch);
    turbowasm_rt_free(p);
    bridge->impl=NULL;
    return TW_DB_OK;
}

static turbowasm_value i32(uint32_t n) {
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I32,.as.i32=(int32_t)n};
}
static turbowasm_value i64(uint64_t n) {
    int64_t bits;
    memcpy(&bits,&n,sizeof bits);
    return (turbowasm_value){.kind=TURBOWASM_VALUE_I64,.as.i64=bits};
}
static void reply(turbowasm_value *r,size_t *count,tw_db_status s) {
    r[0]=i32(s); *count=1;
}
static void pair64(turbowasm_value *r,size_t *count,tw_db_status s,uint64_t value) {
    reply(r,count,s); r[1]=i64(s==TW_DB_OK?value:0); *count=2;
}
static void pair32(turbowasm_value *r,size_t *count,tw_db_status s,uint32_t value) {
    reply(r,count,s); r[1]=i32(s==TW_DB_OK?value:0); *count=2;
}

/* All signatures are checked by the linker/runtime. Each import authenticates
 * the caller before any memory access, lookup or provider effect. */
#define DB_CALLBACK(name) static turbowasm_status name(void *context,turbowasm_host_call *call, \
    const turbowasm_value *args,size_t argc,turbowasm_value *r,size_t capacity, \
    size_t *count,turbowasm_trap *trap) { \
    tw_db_bridge *bridge=context; bridge_impl *p=bridge->impl; \
    (void)argc;(void)capacity;*count=0;*trap=TURBOWASM_TRAP_NONE

DB_CALLBACK(submit);
    if(!authorized(p,call)) {pair64(r,count,TW_DB_DENIED,0);return TURBOWASM_OK;}
    uint64_t length=(uint64_t)args[3].as.i64;
    if(length>p->request_capacity) {pair64(r,count,TW_DB_LIMIT,0);return TURBOWASM_OK;}
    turbowasm_status status=turbowasm_host_call_memory_read64(call,0,
        (uint64_t)args[2].as.i64,p->scratch,(size_t)length,trap);
    if(status!=TURBOWASM_OK) return status;
    uint64_t operation=0;
    tw_db_status s=tw_db_manager_submit(&p->manager,(uint64_t)args[1].as.i64,
        (uint32_t)args[0].as.i32,p->scratch,(size_t)length,&operation);
    if(length) memset(p->scratch,0,(size_t)length);
    if(s==TW_DB_OK) {
        tw_db_work work;
        /* No failure is expected after successful reservation; propagate an
           invariant violation rather than reporting an unsubmitted success. */
        s=tw_db_manager_start(&p->manager,operation,&work);
        if(s!=TW_DB_OK) return TURBOWASM_INVALID_ARGUMENT;
        p->dispatch.start(p->dispatch.context,bridge,&work);
    }
    pair64(r,count,s,operation);return TURBOWASM_OK;
}
DB_CALLBACK(poll);
    tw_db_operation_state state=0;
    tw_db_status s=authorized(p,call)?tw_db_manager_poll(&p->manager,
        (uint64_t)args[0].as.i64,&state):TW_DB_DENIED;
    pair32(r,count,s,state);return TURBOWASM_OK;
}
DB_CALLBACK(await_operation);
    if(!authorized(p,call)) {pair32(r,count,TW_DB_DENIED,0);return TURBOWASM_OK;}
    uint64_t operation=(uint64_t)args[0].as.i64;
    tw_db_operation_state state=0;
    tw_db_status s=tw_db_manager_poll(&p->manager,operation,&state);
    if(s!=TW_DB_OK||state==TW_DB_TERMINAL) {pair32(r,count,s,state);return TURBOWASM_OK;}
    if(!p->inside_resume||!p->execution_identity||!turbowasm_host_call_can_wait(call)) {
        pair32(r,count,TW_DB_UNSUPPORTED,0);return TURBOWASM_OK;
    }
    if(p->waiting) {pair32(r,count,TW_DB_BUSY,0);return TURBOWASM_OK;}
    p->waiting=operation; p->notified=false;
    int completion=0;
    turbowasm_status status=turbowasm_host_call_wait(call,(uintptr_t)p,&p->wait,&completion);
    p->waiting=0;p->wait=(turbowasm_host_wait){0};p->notified=false;
    if(status!=TURBOWASM_OK) {
        cancel(bridge,operation,&s);
        /* The retained callback is unwound, but the independent operation
           still owns native work until the host observes its terminal drain. */
        return status;
    }
    s=tw_db_manager_poll(&p->manager,operation,&state);
    if(s!=TW_DB_OK||state!=TW_DB_TERMINAL) return TURBOWASM_INVALID_ARGUMENT;
    pair32(r,count,s,state);return TURBOWASM_OK;
}
DB_CALLBACK(receipt);
    tw_db_receipt rec={0};
    tw_db_status s=authorized(p,call)?tw_db_manager_receipt(&p->manager,
        (uint64_t)args[0].as.i64,&rec):TW_DB_DENIED;
    pair32(r,count,s,rec.outcome);r[2]=i64(s==TW_DB_OK?rec.size:0);*count=3;
    return TURBOWASM_OK;
}
DB_CALLBACK(read_result);
    if(!authorized(p,call)) {pair64(r,count,TW_DB_DENIED,0);return TURBOWASM_OK;}
    uint64_t offset=(uint64_t)args[1].as.i64,limit=(uint64_t)args[3].as.i64;
    if(offset>SIZE_MAX) {pair64(r,count,TW_DB_INVALID_REQUEST,0);return TURBOWASM_OK;}
    if(limit>p->result_capacity)limit=p->result_capacity;
    size_t copied=0;
    tw_db_status s=tw_db_manager_read(&p->manager,(uint64_t)args[0].as.i64,
        (size_t)offset,p->scratch,(size_t)limit,&copied);
    if(s==TW_DB_OK) {
        turbowasm_status status=turbowasm_host_call_memory_write64(call,0,
            (uint64_t)args[2].as.i64,p->scratch,copied,trap);
        if(copied)memset(p->scratch,0,copied);
        if(status!=TURBOWASM_OK)return status;
    }
    pair64(r,count,s,copied);return TURBOWASM_OK;
}
DB_CALLBACK(take_resource);
    uint64_t resource=0;
    tw_db_status s=authorized(p,call)?tw_db_manager_take(&p->manager,
        (uint64_t)args[0].as.i64,&resource):TW_DB_DENIED;
    pair64(r,count,s,resource);return TURBOWASM_OK;
}
DB_CALLBACK(cancel_operation);
    tw_db_status s=TW_DB_DENIED;
    if(authorized(p,call))cancel(bridge,(uint64_t)args[0].as.i64,&s);
    reply(r,count,s);return TURBOWASM_OK;
}
DB_CALLBACK(drop_operation);
    uint64_t op=(uint64_t)args[0].as.i64;
    tw_db_status s=!authorized(p,call)?TW_DB_DENIED:
        p->waiting==op?TW_DB_BUSY:tw_db_manager_drop(&p->manager,op);
    reply(r,count,s);return TURBOWASM_OK;
}
#undef DB_CALLBACK

turbowasm_status tw_db_bridge_define(tw_db_bridge *bridge,turbowasm_linker *linker) {
    static const turbowasm_value_kind one[]={TURBOWASM_VALUE_I64};
    static const turbowasm_value_kind sub[]={TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I64,
        TURBOWASM_VALUE_I64,TURBOWASM_VALUE_I64};
    static const turbowasm_value_kind rd[]={TURBOWASM_VALUE_I64,TURBOWASM_VALUE_I64,
        TURBOWASM_VALUE_I64,TURBOWASM_VALUE_I64};
    static const turbowasm_value_kind res[]={TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I64};
    static const turbowasm_value_kind state[]={TURBOWASM_VALUE_I32,TURBOWASM_VALUE_I32,
        TURBOWASM_VALUE_I64};
    const struct {const char *name;turbowasm_host_function_type type;
        turbowasm_host_function_fn fn;} imports[]={
        {"submit",{sub,4,res,2},submit},{"poll",{one,1,state,2},poll},
        {"await",{one,1,state,2},await_operation},{"receipt",{one,1,state,3},receipt},
        {"read",{rd,4,res,2},read_result},{"take_resource",{one,1,res,2},take_resource},
        {"cancel",{one,1,res,1},cancel_operation},{"drop",{one,1,res,1},drop_operation}
    };
    const turbowasm_name ns={(const uint8_t *)"turbodb:db",10};
    for(size_t i=0;i<sizeof imports/sizeof imports[0];++i) {
        turbowasm_name field={(const uint8_t *)imports[i].name,
            (uint32_t)strlen(imports[i].name)};
        turbowasm_status s=turbowasm_linker_define_host_function(linker,ns,field,
            &imports[i].type,imports[i].fn,bridge);
        if(s!=TURBOWASM_OK)return s;
    }
    return TURBOWASM_OK;
}
