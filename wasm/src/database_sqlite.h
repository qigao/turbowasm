#ifndef TURBOWASM_DATABASE_SQLITE_H
#define TURBOWASM_DATABASE_SQLITE_H

#include "database_worker.h"

/* M2b private native SQLite provider. The host supplies immutable paths and
 * trusted single-statement DML or side-effect-free SELECT templates in the
 * same order as its policy. QUERY grants must not contain mutating functions,
 * RETURNING commands, or other state-changing statements.
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
    uint32_t connections, cursors;
    size_t host_bytes;
    uint32_t busy_timeout_ms;
    uint32_t max_columns;
    size_t max_sql_bytes, max_parameter_bytes, max_result_bytes;
} tw_db_sqlite_config;

/* OPEN_SQL, named EXECUTE/QUERY, FETCH and CLOSE are enabled in this slice.
 * Reject transaction and Redis grants rather than advertising incomplete
 * operations. Read-only aliases allow QUERY templates only and use native
 * read_only open mode. The host supplies trusted, side-effect-free SELECTs;
 * this is not a raw guest SQL classifier. Database schema preparation belongs
 * to the host. Each retained cursor owns one ORM result subject to the native
 * configured row, column and byte limits; native allocation overhead is
 * separate from the private slot-table host_bytes budget. */
tw_db_status tw_db_sqlite_init(tw_db_sqlite *sqlite,const tw_db_policy *policy,
    const tw_db_sqlite_config *config);
tw_db_worker_result tw_db_sqlite_execute(void *context,const tw_db_worker_call *call,
    uint8_t *bytes,size_t capacity);
/* Call only after bridge drain and worker join; no native cleanup on the
 * WASM owner. BUSY preserves a live native connection/runtime. */
tw_db_status tw_db_sqlite_destroy(tw_db_sqlite *sqlite);

#endif
