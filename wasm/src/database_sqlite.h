#ifndef TURBOWASM_DATABASE_SQLITE_H
#define TURBOWASM_DATABASE_SQLITE_H

#include "database_worker.h"

/* M2a private native SQLite provider. The host supplies immutable paths and
 * trusted single-statement DML templates in the same order as its policy.
 * There is no guest SQL string, native path or credential. Configuration
 * storage is borrowed through worker join and provider destruction. */
typedef struct tw_db_sqlite { void *impl; } tw_db_sqlite;

typedef struct tw_db_sqlite_alias {
    const char *filename;
    bool read_only;
    const char *const *statements;
    size_t statement_count;
} tw_db_sqlite_alias;

typedef struct tw_db_sqlite_config {
    const char *driver_path;
    const tw_db_sqlite_alias *aliases;
    size_t alias_count;
    uint32_t connections;
    size_t host_bytes;
    uint32_t busy_timeout_ms;
    uint32_t max_columns;
    size_t max_sql_bytes, max_parameter_bytes, max_result_bytes;
} tw_db_sqlite_config;

/* Only OPEN_SQL, named EXECUTE and CLOSE are enabled in this slice.
 * Reject a policy that grants transaction, query or Redis operations rather
 * than accepting a partly implemented capability. Database schema preparation
 * belongs to the host. Native ORM allocations are bounded by its own limits. */
tw_db_status tw_db_sqlite_init(tw_db_sqlite *sqlite,const tw_db_policy *policy,
    const tw_db_sqlite_config *config);
tw_db_worker_result tw_db_sqlite_execute(void *context,const tw_db_worker_call *call,
    uint8_t *bytes,size_t capacity);
/* Call only after bridge drain and worker join; no native cleanup on the
 * WASM owner. BUSY preserves a live native connection/runtime. */
tw_db_status tw_db_sqlite_destroy(tw_db_sqlite *sqlite);

#endif
