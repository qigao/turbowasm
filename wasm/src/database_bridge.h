#ifndef TURBOWASM_DATABASE_BRIDGE_H
#define TURBOWASM_DATABASE_BRIDGE_H
#include "database_lifecycle.h"
#include <execution.h>

/* Private owner-affine composition. Not an installed SDK. One capability
 * per guest, one resumable execution at a time. All entry points and provider
 * callbacks run on the same host owner; workers post bounded completions to it.
 * No callback may reenter guest execution. */
typedef struct tw_db_bridge { void *impl; } tw_db_bridge;

typedef struct tw_db_dispatch {
    void *context;
    /* Accepted work is dispatched once. Copy the work descriptor if retaining
     * it; its request views remain valid until accepted terminal completion.
     * Return promptly; either complete inline or arrange owner-side progress.
     * Every accepted operation owes exactly one drained terminal receipt. */
    void (*start)(void *context, tw_db_bridge *bridge, const tw_db_work *work);
    /* Notification only. Does not consume the operation or prove rollback. */
    void (*cancel)(void *context, tw_db_bridge *bridge, uint64_t operation);
} tw_db_dispatch;

tw_db_status tw_db_bridge_init(tw_db_bridge *bridge,const tw_db_policy *policy,
    const tw_db_capacity *capacity,const tw_db_dispatch *dispatch);
/* Register before linked instance creation. Until bind, all imports deny
 * access (including a module start function). If registration fails, discard
 * the linker before destroying the bridge; partial registrations are borrowed. */
turbowasm_status tw_db_bridge_define(tw_db_bridge *bridge,turbowasm_linker *linker);
tw_db_status tw_db_bridge_bind(tw_db_bridge *bridge,const turbowasm_instance *instance);

/* A resumable execution must use these owner entry points. Await requires
 * the currently resumed execution; one-shot await of pending work returns
 * UNSUPPORTED without redispatching. Only wake records a Runtime completion;
 * it never resumes guest instructions. */
turbowasm_status tw_db_bridge_resume(tw_db_bridge *bridge,turbowasm_execution *execution,
    const turbowasm_execution_options *options);
tw_db_status tw_db_bridge_wake(tw_db_bridge *bridge,turbowasm_execution *execution);
tw_db_status tw_db_bridge_destroy_execution(tw_db_bridge *bridge,turbowasm_execution *execution);

tw_db_status tw_db_bridge_complete(tw_db_bridge *bridge,uint64_t operation,
    tw_db_outcome outcome,uintptr_t native_resource,const uint8_t *bytes,size_t size);

/* Stop guest access, then destroy its instance/linker before detach. A live
 * resumable execution must first be destroyed using the owner entry point.
 * Detach is irreversible. Trusted owner cleanup may access the manager only
 * after detach, to drain operations and resources; no guest disposal proof.
 * The bridge and provider context must outlive all native work and callbacks.
 * Checked destroy never frees undrained storage. */
void tw_db_bridge_stop(tw_db_bridge *bridge);
tw_db_status tw_db_bridge_detach(tw_db_bridge *bridge);
tw_db_manager *tw_db_bridge_cleanup_manager(tw_db_bridge *bridge);
tw_db_status tw_db_bridge_destroy(tw_db_bridge *bridge);
#endif
