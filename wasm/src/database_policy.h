#ifndef TURBOWASM_DATABASE_POLICY_H
#define TURBOWASM_DATABASE_POLICY_H
#include "database_wire.h"

typedef struct tw_db_owned_policy {
    tw_db_policy policy;
    tw_db_alias *aliases;
    tw_db_template *templates;
    uint32_t *masks;
    char *names;
} tw_db_owned_policy;

/* Logical allocation accounting. All allocation sizes are reserved before
 * allocating; partial construction is released by the owning initializer. */
tw_db_status tw_db_policy_copy(tw_db_owned_policy *owned,
    const tw_db_policy *source, size_t *budget);
void tw_db_policy_destroy(tw_db_owned_policy *owned);
bool tw_db_budget_take(size_t *budget, size_t count, size_t size);
#endif
