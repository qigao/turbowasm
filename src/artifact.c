#include "artifact.h"

#include "module_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    TW_ARTIFACT_HEADER_SIZE = 72u,
    TW_ARTIFACT_SECTION_HEADER_SIZE = 8u,
    TW_ARTIFACT_SUMMARY_SIZE = 88u,
    TW_ARTIFACT_MAGIC_0 = 'T',
    TW_ARTIFACT_MAGIC_1 = 'W',
    TW_ARTIFACT_MAGIC_2 = 'V',
    TW_ARTIFACT_MAGIC_3 = 'A'
};

enum {
    TW_FEATURE_CORE = UINT64_C(1) << 0,
    TW_FEATURE_BULK_MEMORY = UINT64_C(1) << 1,
    TW_FEATURE_REFERENCE_TYPES = UINT64_C(1) << 2,
    TW_FEATURE_SIMD = UINT64_C(1) << 3,
    TW_FEATURE_MULTI_MEMORY = UINT64_C(1) << 4,
    TW_FEATURE_CUSTOM_PAGE_SIZES = UINT64_C(1) << 5,
    TW_FEATURE_EXTENDED_CONST = UINT64_C(1) << 6,
    TW_FEATURE_RELAXED_SIMD = UINT64_C(1) << 7,
    TW_FEATURE_TAIL_CALL = UINT64_C(1) << 8,
    TW_FEATURE_EXCEPTION_HANDLING = UINT64_C(1) << 9,
    TW_FEATURE_THREADS = UINT64_C(1) << 10,
    TW_FEATURE_MEMORY64 = UINT64_C(1) << 11
};

typedef struct tw_writer {
    uint8_t *data;
    size_t size;
    size_t offset;
} tw_writer;

typedef struct tw_reader {
    const uint8_t *data;
    size_t size;
    size_t offset;
} tw_reader;

static bool put_u32(tw_writer *w, uint32_t v) {
    if (w == NULL || w->offset > w->size ||
        w->size - w->offset < 4u)
        return false;
    w->data[w->offset + 0u]=(uint8_t)v;
    w->data[w->offset + 1u]=(uint8_t)(v>>8u);
    w->data[w->offset + 2u]=(uint8_t)(v>>16u);
    w->data[w->offset + 3u]=(uint8_t)(v>>24u);
    w->offset += 4u;
    return true;
}

static bool put_u64(tw_writer *w, uint64_t v) {
    uint32_t i;
    if (w == NULL || w->offset > w->size ||
        w->size - w->offset < 8u)
        return false;
    for (i=0u;i<8u;++i)
        w->data[w->offset+i]=(uint8_t)(v>>(8u*i));
    w->offset += 8u;
    return true;
}

static bool put_bytes(tw_writer *w, const uint8_t *p, size_t n) {
    if (w == NULL || (n != 0u && p == NULL) ||
        w->offset > w->size || n > w->size - w->offset)
        return false;
    if (n != 0u)
        memcpy(w->data+w->offset,p,n);
    w->offset += n;
    return true;
}

static bool get_u32(tw_reader *r, uint32_t *out) {
    const uint8_t *p;
    if (r == NULL || out == NULL || r->offset > r->size ||
        r->size-r->offset < 4u)
        return false;
    p=r->data+r->offset;
    *out=(uint32_t)p[0] |
         ((uint32_t)p[1]<<8u) |
         ((uint32_t)p[2]<<16u) |
         ((uint32_t)p[3]<<24u);
    r->offset += 4u;
    return true;
}

static bool get_u64(tw_reader *r, uint64_t *out) {
    uint64_t v=0u;
    uint32_t i;
    if (r == NULL || out == NULL || r->offset > r->size ||
        r->size-r->offset < 8u)
        return false;
    for(i=0u;i<8u;++i)
        v |= (uint64_t)r->data[r->offset+i] << (8u*i);
    r->offset += 8u;
    *out=v;
    return true;
}

static bool get_bytes(tw_reader *r, uint8_t *out, size_t n) {
    if (r == NULL || (n != 0u && out == NULL) ||
        r->offset > r->size || n > r->size-r->offset)
        return false;
    if (n != 0u)
        memcpy(out,r->data+r->offset,n);
    r->offset += n;
    return true;
}

uint64_t turbowasm_artifact_current_feature_fingerprint(void) {
    return TW_FEATURE_CORE |
           TW_FEATURE_BULK_MEMORY |
           TW_FEATURE_REFERENCE_TYPES |
           TW_FEATURE_SIMD |
           TW_FEATURE_MULTI_MEMORY |
           TW_FEATURE_CUSTOM_PAGE_SIZES |
           TW_FEATURE_EXTENDED_CONST |
           TW_FEATURE_RELAXED_SIMD |
           TW_FEATURE_TAIL_CALL |
           TW_FEATURE_EXCEPTION_HANDLING |
           TW_FEATURE_THREADS |
           TW_FEATURE_MEMORY64;
}

static bool write_summary(
    tw_writer *w,
    const turbowasm_module_summary *s) {
    return put_u32(w,s->standard_section_mask) &&
           put_u64(w,(uint64_t)s->custom_section_count) &&
           put_u32(w,s->type_count) &&
           put_u32(w,s->imported_function_count) &&
           put_u32(w,s->imported_table_count) &&
           put_u32(w,s->imported_memory_count) &&
           put_u32(w,s->imported_global_count) &&
           put_u32(w,s->imported_tag_count) &&
           put_u32(w,s->function_count) &&
           put_u32(w,s->table_count) &&
           put_u32(w,s->memory_count) &&
           put_u32(w,s->global_count) &&
           put_u32(w,s->tag_count) &&
           put_u32(w,s->code_count) &&
           put_u32(w,s->export_count) &&
           put_u32(w,s->element_count) &&
           put_u32(w,s->has_start?1u:0u) &&
           put_u32(w,s->start_function_index) &&
           put_u32(w,s->has_data_count?1u:0u) &&
           put_u32(w,s->data_count) &&
           put_u32(w,s->data_segment_count);
}

static bool read_summary(
    tw_reader *r,
    turbowasm_module_summary *s) {
    uint64_t custom;
    uint32_t has_start;
    uint32_t has_data;
    if (r == NULL || s == NULL)
        return false;
    memset(s,0,sizeof(*s));
    if (!get_u32(r,&s->standard_section_mask) ||
        !get_u64(r,&custom) ||
        custom > (uint64_t)SIZE_MAX ||
        !get_u32(r,&s->type_count) ||
        !get_u32(r,&s->imported_function_count) ||
        !get_u32(r,&s->imported_table_count) ||
        !get_u32(r,&s->imported_memory_count) ||
        !get_u32(r,&s->imported_global_count) ||
        !get_u32(r,&s->imported_tag_count) ||
        !get_u32(r,&s->function_count) ||
        !get_u32(r,&s->table_count) ||
        !get_u32(r,&s->memory_count) ||
        !get_u32(r,&s->global_count) ||
        !get_u32(r,&s->tag_count) ||
        !get_u32(r,&s->code_count) ||
        !get_u32(r,&s->export_count) ||
        !get_u32(r,&s->element_count) ||
        !get_u32(r,&has_start) ||
        !get_u32(r,&s->start_function_index) ||
        !get_u32(r,&has_data) ||
        !get_u32(r,&s->data_count) ||
        !get_u32(r,&s->data_segment_count))
        return false;
    if (has_start > 1u || has_data > 1u)
        return false;
    s->custom_section_count=(size_t)custom;
    s->has_start=has_start!=0u;
    s->has_data_count=has_data!=0u;
    return true;
}

turbowasm_status turbowasm_artifact_measure(
    const turbowasm_module *module,
    size_t *out_size) {
    if (turbowasm_module_impl_get(module) == NULL || out_size == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    *out_size =
        TW_ARTIFACT_HEADER_SIZE +
        TW_ARTIFACT_SECTION_HEADER_SIZE +
        TW_ARTIFACT_SUMMARY_SIZE;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_artifact_write(
    const turbowasm_module *module,
    uint8_t *output,
    size_t capacity,
    size_t *out_size) {
    const turbowasm_module_impl *impl =
        turbowasm_module_impl_get(module);
    size_t required;
    uint8_t digest[TURBOWASM_SHA256_DIGEST_SIZE];
    tw_writer w;

    if (impl == NULL || out_size == NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    required =
        TW_ARTIFACT_HEADER_SIZE +
        TW_ARTIFACT_SECTION_HEADER_SIZE +
        TW_ARTIFACT_SUMMARY_SIZE;
    *out_size=required;
    if (output == NULL || capacity < required)
        return TURBOWASM_OUT_OF_MEMORY;

    turbowasm_sha256(impl->bytes,impl->size,digest);
    w.data=output; w.size=capacity; w.offset=0u;

    if (!put_bytes(&w,(const uint8_t *)"TWVA",4u) ||
        !put_u32(&w,TURBOWASM_ARTIFACT_SCHEMA_VERSION) ||
        !put_u32(&w,TW_ARTIFACT_HEADER_SIZE) ||
        !put_u32(&w,0u) ||
        !put_u64(&w,turbowasm_artifact_current_feature_fingerprint()) ||
        !put_u64(&w,(uint64_t)impl->size) ||
        !put_bytes(&w,digest,sizeof(digest)) ||
        !put_u32(&w,1u) ||
        !put_u32(&w,0u) ||
        !put_u32(&w,TURBOWASM_ARTIFACT_SECTION_SUMMARY) ||
        !put_u32(&w,TW_ARTIFACT_SUMMARY_SIZE) ||
        !write_summary(&w,&impl->summary) ||
        w.offset != required)
        return TURBOWASM_MALFORMED_MODULE;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_artifact_inspect(
    const uint8_t *artifact,
    size_t artifact_size,
    const uint8_t *source,
    size_t source_size,
    turbowasm_artifact_info *out) {
    tw_reader r;
    uint8_t magic[4];
    uint32_t header_size;
    uint32_t reserved;
    uint32_t section_index;
    uint8_t actual_digest[TURBOWASM_SHA256_DIGEST_SIZE];
    bool have_summary=false;

    if (artifact == NULL || out == NULL ||
        (source_size != 0u && source == NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out,0,sizeof(*out));
    r.data=artifact; r.size=artifact_size; r.offset=0u;
    if (!get_bytes(&r,magic,sizeof(magic)) ||
        memcmp(magic,"TWVA",4u)!=0 ||
        !get_u32(&r,&out->schema_version) ||
        !get_u32(&r,&header_size) ||
        !get_u32(&r,&out->flags) ||
        !get_u64(&r,&out->feature_fingerprint) ||
        !get_u64(&r,&out->source_size) ||
        !get_bytes(&r,out->source_sha256,sizeof(out->source_sha256)) ||
        !get_u32(&r,&out->section_count) ||
        !get_u32(&r,&reserved))
        return TURBOWASM_MALFORMED_MODULE;

    if (out->schema_version != TURBOWASM_ARTIFACT_SCHEMA_VERSION ||
        header_size != TW_ARTIFACT_HEADER_SIZE ||
        reserved != 0u ||
        (out->flags & ~TURBOWASM_ARTIFACT_FLAG_COMPLETE_METADATA) != 0u ||
        out->feature_fingerprint !=
            turbowasm_artifact_current_feature_fingerprint() ||
        out->source_size != (uint64_t)source_size)
        return TURBOWASM_UNSUPPORTED;

    turbowasm_sha256(source,source_size,actual_digest);
    if (memcmp(actual_digest,out->source_sha256,sizeof(actual_digest))!=0)
        return TURBOWASM_TYPE_MISMATCH;

    for(section_index=0u;section_index<out->section_count;++section_index){
        uint32_t type;
        uint32_t size;
        size_t end;
        if(!get_u32(&r,&type)||!get_u32(&r,&size) ||
           r.offset > r.size || (size_t)size > r.size-r.offset)
            return TURBOWASM_MALFORMED_MODULE;
        end=r.offset+(size_t)size;
        if(type==TURBOWASM_ARTIFACT_SECTION_SUMMARY){
            tw_reader sr;
            if(have_summary || size!=TW_ARTIFACT_SUMMARY_SIZE)
                return TURBOWASM_MALFORMED_MODULE;
            sr.data=r.data+r.offset; sr.size=size; sr.offset=0u;
            if(!read_summary(&sr,&out->summary)||sr.offset!=sr.size)
                return TURBOWASM_MALFORMED_MODULE;
            have_summary=true;
        }
        r.offset=end;
    }

    if(!have_summary || r.offset!=r.size)
        return TURBOWASM_MALFORMED_MODULE;
    return TURBOWASM_OK;
}
