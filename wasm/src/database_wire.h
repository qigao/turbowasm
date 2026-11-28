#ifndef TURBOWASM_DATABASE_WIRE_H
#define TURBOWASM_DATABASE_WIRE_H

/* Private M1 admission core. Not an installed ABI or database provider. */
#include <link.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum tw_db_opcode {
    TW_DB_OPEN_SQL = 1, TW_DB_OPEN_REDIS = 2, TW_DB_BEGIN = 3,
    TW_DB_EXECUTE = 4, TW_DB_QUERY = 5, TW_DB_FETCH = 6,
    TW_DB_COMMIT = 7, TW_DB_ROLLBACK = 8, TW_DB_REDIS_COMMAND = 9,
    TW_DB_CLOSE = 10
} tw_db_opcode;

typedef enum tw_db_status {
    TW_DB_OK = 0, TW_DB_INVALID_REQUEST = 1, TW_DB_DENIED = 2,
    TW_DB_INVALID_HANDLE = 3, TW_DB_LIMIT = 4, TW_DB_UNSUPPORTED = 5,
    TW_DB_PENDING = 6, TW_DB_WRONG_STATE = 7, TW_DB_BUSY = 8,
    TW_DB_OUT_OF_MEMORY = 9
} tw_db_status;

typedef enum tw_db_outcome {
    TW_DB_SUCCESS = 1, TW_DB_FAILED_NO_EFFECT = 2,
    TW_DB_CANCELLED_NO_EFFECT = 3, TW_DB_OUTCOME_UNKNOWN = 4
} tw_db_outcome;

typedef enum tw_db_kind {
    TW_DB_NONE = 0, TW_DB_SQL = 1, TW_DB_REDIS = 2,
    TW_DB_TRANSACTION = 3, TW_DB_CURSOR = 4
} tw_db_kind;

typedef enum tw_db_value_kind {
    TW_DB_NULL = 0, TW_DB_BOOL = 1, TW_DB_I64 = 2, TW_DB_U64 = 3,
    TW_DB_F64 = 4, TW_DB_TEXT = 5, TW_DB_BLOB = 6
} tw_db_value_kind;

typedef enum tw_db_redis_command {
    TW_DB_GET = 1, TW_DB_SET = 2, TW_DB_DEL = 3,
    TW_DB_EXISTS = 4, TW_DB_HGET = 5, TW_DB_HSET = 6
} tw_db_redis_command;

enum { TW_DB_ISOLATION_DEFAULT = 0, TW_DB_ISOLATION_READ_UNCOMMITTED = 1,
       TW_DB_ISOLATION_READ_COMMITTED = 2, TW_DB_ISOLATION_REPEATABLE_READ = 3,
       TW_DB_ISOLATION_SERIALIZABLE = 4 };

#define TW_DB_MASK(n) (UINT32_C(1) << (n))

typedef struct tw_db_bytes {
    const uint8_t *data;
    size_t size;
} tw_db_bytes;

typedef struct tw_db_reader {
    const uint8_t *data;
    size_t remaining;
} tw_db_reader;

typedef struct tw_db_value {
    tw_db_value_kind kind;
    tw_db_bytes payload;
} tw_db_value;

typedef struct tw_db_limits {
    size_t request_bytes;
    uint32_t name_bytes, value_bytes, parameters, redis_arguments, rows;
} tw_db_limits;

/* Host policy is already validated, immutable and borrowed in M1a.
 * M1b owns its copy and validates lifetime, capacity and uniqueness.
 * No pointers or policy indices originate from the guest. */
typedef struct tw_db_template {
    const char *name;
    uint32_t opcode_mask;
    const uint32_t *parameter_masks;
    uint32_t parameter_count;
} tw_db_template;

typedef struct tw_db_alias {
    const char *name;
    tw_db_kind kind;
    uint32_t isolation_mask;
    uint32_t redis_command_mask;
    const tw_db_template *templates;
    size_t template_count;
} tw_db_alias;

typedef struct tw_db_policy {
    tw_db_limits limits;
    const tw_db_alias *aliases;
    size_t alias_count;
} tw_db_policy;

/* Trusted resource-table lookup result. M1b must obtain this from the
 * instance's live resource table, never decode it from guest memory. */
typedef struct tw_db_owner {
    tw_db_kind kind;
    size_t alias_index;
} tw_db_owner;

typedef struct tw_db_request {
    tw_db_opcode opcode;
    size_t alias_index, template_index;
    uint32_t argument_count, command, isolation, rows;
    tw_db_bytes arguments;
} tw_db_request;

/* Zero allocation. Reader views borrow the supplied immutable request bytes.
 * A failed read does not advance the reader or publish an output. */
bool tw_db_read_value(tw_db_reader *reader, uint32_t max_bytes, tw_db_value *out);
bool tw_db_read_bytes(tw_db_reader *reader, uint32_t max_bytes, tw_db_bytes *out);

/* Canonical result writer. Each write checks full bounds and leaves the
 * logical writer position unchanged on failure. No allocation. */
typedef struct tw_db_writer {
    uint8_t *data;
    size_t capacity, size;
} tw_db_writer;
bool tw_db_write_u32(tw_db_writer *writer,uint32_t value);
bool tw_db_write_u64(tw_db_writer *writer,uint64_t value);
bool tw_db_write_bytes(tw_db_writer *writer,tw_db_bytes bytes);
bool tw_db_write_text(tw_db_writer *writer,tw_db_bytes bytes);
bool tw_db_write_value(tw_db_writer *writer,uint32_t max_bytes,tw_db_value value);

/* Decode and authorize a fully copied request. On error out is unchanged.
 * No provider invocation, allocation, or external effect occurs here. */
tw_db_status tw_db_admit(const tw_db_policy *policy, tw_db_owner owner,
    uint32_t opcode, const uint8_t *bytes, size_t size, tw_db_request *out);

/* Protected copy from guest memory zero into owner-reserved scratch.
 * Caller identity must match the configured instance before any guest read.
 * On runtime failure return the Runtime status/trap (e.g. OOB); otherwise
 * return TURBOWASM_OK and publish the database status. The request borrows
 * scratch until the caller has transferred it into operation-owned storage. */
turbowasm_status tw_db_admit_guest(const tw_db_policy *policy,
    const turbowasm_instance *instance, turbowasm_host_call *call,
    tw_db_owner owner, uint32_t opcode, uint64_t address, uint64_t length,
    uint8_t *scratch, size_t scratch_capacity, tw_db_request *out,
    tw_db_status *status, turbowasm_trap *trap);

#endif
