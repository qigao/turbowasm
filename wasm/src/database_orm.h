#ifndef TURBOWASM_DATABASE_ORM_H
#define TURBOWASM_DATABASE_ORM_H

#include "database_worker.h"

/* Private native SQL provider, one explicitly selected driver per instance. The host supplies immutable paths and
 * trusted single-statement DML or side-effect-free SELECT templates in the
 * same order as its policy. QUERY grants must not contain mutating functions,
 * RETURNING commands, or other state-changing statements.
 * There is no guest SQL string, native path or credential. Configuration
 * storage is borrowed through worker join and provider destruction. */
typedef struct tw_db_orm { void *impl; } tw_db_orm;

typedef enum tw_db_orm_driver {
    TW_DB_ORM_SQLITE=1, TW_DB_ORM_MYSQL
} tw_db_orm_driver;

typedef struct tw_db_mysql_settings {
    const char *host, *username, *password, *database;
    const char *ca_file, *server_name;
    uint16_t port;
} tw_db_mysql_settings;

typedef struct tw_db_orm_alias {
    bool read_only;
    const char *const *statements;
    size_t statement_count;
    union {
        const char *filename; /* SQLite: pre-existing absolute path. */
        tw_db_mysql_settings mysql; /* MySQL: verified TLS, host-owned account. */
    } source;
} tw_db_orm_alias;

typedef struct tw_db_orm_config {
    tw_db_orm_driver driver;
    const char *driver_path;
    const tw_db_orm_alias *aliases;
    size_t alias_count;
    uint32_t connections, cursors, transactions;
    size_t host_bytes;
    uint32_t timeout_ms;
    uint32_t max_columns;
    size_t max_sql_bytes, max_parameter_bytes, max_result_bytes;
} tw_db_orm_config;

/* OPEN_SQL, named EXECUTE/QUERY, FETCH, BEGIN/COMMIT/ROLLBACK and CLOSE.
 * SQLite admits default isolation only; MySQL admits explicitly advertised
 * READ_COMMITTED, REPEATABLE_READ and SERIALIZABLE only. No default MySQL
 * isolation is invented. Read-only aliases permit SELECT grants only;
 * SQLite opens native read_only, MySQL requires a host-provisioned SELECT-only
 * server account (no client-side claim of server privilege verification).
 * Native ORM limits bound retained snapshots separately from slot storage.
 * Host configuration is immutable and must outlive the worker and provider.
 * Credentials never cross guest memory or appear in guest receipts. */
tw_db_status tw_db_orm_init(tw_db_orm *provider,const tw_db_policy *policy,
    const tw_db_orm_config *config);
tw_db_worker_result tw_db_orm_execute(void *context,const tw_db_worker_call *call,
    uint8_t *bytes,size_t capacity);
/* Call only after bridge drain and worker join; no native cleanup on the
 * WASM owner. BUSY preserves a live native connection/runtime. */
tw_db_status tw_db_orm_destroy(tw_db_orm *provider);

#endif
