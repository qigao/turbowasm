#ifndef TURBOWASM_DATABASE_SQL_RESULT_H
#define TURBOWASM_DATABASE_SQL_RESULT_H

#include "database_wire.h"
#include <orm.h>

/* Native bounded ORM snapshot projection. Only the private provider sees
 * orm_result_t. The guest sees exact counted LE wire bytes. */
typedef struct tw_db_sql_result {
    orm_result_t *native;
    uint64_t rows, next;
    uint32_t columns;
} tw_db_sql_result;

/* QUERY receipt: u32 columns, u32 flags (bit 0 names available), u64 total
 * rows, then counted UTF-8 names when available. Empty ORM snapshots expose
 * their column count, not synthesized names. Preflight every row against
 * the available FETCH result capacity before publishing a cursor. */
bool tw_db_sql_result_open(tw_db_sql_result *out,orm_result_t *native,
    uint32_t max_rows,uint32_t max_columns,uint32_t max_value_bytes,
    uint8_t *bytes,size_t capacity,size_t *written);

/* FETCH receipt: u32 rows, u32 EOF (0 or 1), then row records consisting of
 * u32 column count followed by canonical tagged values. Do not advance on
 * failure; reduce the batch only at a complete row boundary. */
bool tw_db_sql_result_fetch(tw_db_sql_result *cursor,uint32_t rows,
    uint32_t max_value_bytes,uint8_t *bytes,size_t capacity,size_t *written);
void tw_db_sql_result_close(tw_db_sql_result *cursor);

#endif
