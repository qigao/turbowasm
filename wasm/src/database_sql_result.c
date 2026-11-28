#include "database_sql_result.h"
#include <string.h>

static bool encode_cell(tw_db_writer *w,const orm_result_t *result,uint64_t row,
    uint32_t column,uint32_t max_value_bytes,orm_error_t *error) {
    orm_value_kind_t kind;
    if(orm_result_value_kind(result,row,column,&kind,error)!=ORM_STATUS_OK)return false;
    tw_db_value v={0};
    uint8_t numeric[8];
    uint64_t bits=0;
    switch(kind) {
    case ORM_VALUE_NULL:
        v.kind=TW_DB_NULL;
        break;
    case ORM_VALUE_BOOLEAN:{
        uint8_t value;
        if(orm_result_get_boolean(result,row,column,&value,error)!=ORM_STATUS_OK)return false;
        numeric[0]=value;
        v=(tw_db_value){TW_DB_BOOL,{numeric,1}};
        break;
    }
    case ORM_VALUE_INT64:{
        int64_t value;
        if(orm_result_get_int64(result,row,column,&value,error)!=ORM_STATUS_OK)return false;
        memcpy(&bits,&value,sizeof bits);
        v.kind=TW_DB_I64;
        break;
    }
    case ORM_VALUE_UINT64:
        if(orm_result_get_uint64(result,row,column,&bits,error)!=ORM_STATUS_OK)return false;
        v.kind=TW_DB_U64;
        break;
    case ORM_VALUE_DOUBLE:{
        double value;
        _Static_assert(sizeof(double)==sizeof(uint64_t),"binary64 required");
        if(orm_result_get_double(result,row,column,&value,error)!=ORM_STATUS_OK)return false;
        memcpy(&bits,&value,sizeof bits);
        v.kind=TW_DB_F64;
        break;
    }
    case ORM_VALUE_TEXT:{
        orm_string_view_t value={0};
        if(orm_result_get_text(result,row,column,&value,error)!=ORM_STATUS_OK)return false;
        v=(tw_db_value){TW_DB_TEXT,{(const uint8_t *)value.data,value.len}};
        break;
    }
    case ORM_VALUE_BLOB:{
        orm_blob_t value={0};
        if(orm_result_get_blob(result,row,column,&value,error)!=ORM_STATUS_OK)return false;
        v=(tw_db_value){TW_DB_BLOB,{value.data,value.size}};
        break;
    }
    default:return false;
    }
    if(v.kind==TW_DB_I64||v.kind==TW_DB_U64||v.kind==TW_DB_F64) {
        for(unsigned i=0;i<8;++i)numeric[i]=(uint8_t)(bits>>(8u*i));
        v.payload=(tw_db_bytes){numeric,8};
    }
    return tw_db_write_value(w,max_value_bytes,v);
}

static bool encode_row(tw_db_writer *writer,const tw_db_sql_result *cursor,
    uint64_t row,uint32_t max_value_bytes,orm_error_t *error) {
    tw_db_writer w=*writer;
    if(!tw_db_write_u32(&w,cursor->columns))return false;
    for(uint32_t i=0;i<cursor->columns;++i)
        if(!encode_cell(&w,cursor->native,row,i,max_value_bytes,error))return false;
    *writer=w;
    return true;
}

bool tw_db_sql_result_open(tw_db_sql_result *out,orm_result_t *native,
    uint32_t max_rows,uint32_t max_columns,uint32_t max_value_bytes,
    uint8_t *bytes,size_t capacity,size_t *written) {
    if(!native||!bytes||capacity<16)return false;
    orm_error_t error;orm_error_init(&error);
    uint64_t rows=0,columns=0;
    if(orm_result_row_count(native,&rows,&error)!=ORM_STATUS_OK||
        orm_result_column_count(native,&columns,&error)!=ORM_STATUS_OK||
        rows>max_rows||!columns||columns>max_columns)return false;
    tw_db_sql_result cursor={native,rows,0,(uint32_t)columns};
    /* Native result is immutable. Each row must fit by itself, so a cursor
       cannot get stuck on a row larger than the precharged result slot. */
    for(uint64_t row=0;row<rows;++row) {
        tw_db_writer scratch={bytes,capacity,8};
        if(!encode_row(&scratch,&cursor,row,max_value_bytes,&error))return false;
    }
    tw_db_writer w={bytes,capacity,0};
    if(!tw_db_write_u32(&w,cursor.columns)||!tw_db_write_u32(&w,rows?1u:0u)||
        !tw_db_write_u64(&w,rows))return false;
    if(rows)for(uint32_t i=0;i<cursor.columns;++i) {
        orm_string_view_t name={0};
        if(orm_result_column_name(native,i,&name,&error)!=ORM_STATUS_OK||
            !tw_db_write_text(&w,(tw_db_bytes){(const uint8_t *)name.data,name.len}))return false;
    }
    *written=w.size;
    *out=cursor;
    return true;
}

bool tw_db_sql_result_fetch(tw_db_sql_result *cursor,uint32_t rows,
    uint32_t max_value_bytes,uint8_t *bytes,size_t capacity,size_t *written) {
    if(!cursor->native||!rows||!bytes||capacity<8)return false;
    orm_error_t error;orm_error_init(&error);
    tw_db_writer w={bytes,capacity,8};
    uint64_t next=cursor->next;
    uint32_t count=0;
    while(next<cursor->rows&&count<rows) {
        /* open validated each row and its type/size. If the remaining
           page is too small, keep the next complete row for the next fetch. */
        if(!encode_row(&w,cursor,next,max_value_bytes,&error))break;
        ++next;
        ++count;
    }
    if(!count&&next<cursor->rows)return false;
    tw_db_writer header={bytes,8,0};
    if(!tw_db_write_u32(&header,count)||
        !tw_db_write_u32(&header,next==cursor->rows?1u:0u))return false;
    cursor->next=next;
    *written=w.size;
    return true;
}

void tw_db_sql_result_close(tw_db_sql_result *cursor) {
    orm_result_destroy(cursor->native);
    *cursor=(tw_db_sql_result){0};
}
