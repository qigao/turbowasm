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
    if (w->data != NULL) {
        w->data[w->offset + 0u]=(uint8_t)v;
        w->data[w->offset + 1u]=(uint8_t)(v>>8u);
        w->data[w->offset + 2u]=(uint8_t)(v>>16u);
        w->data[w->offset + 3u]=(uint8_t)(v>>24u);
    }
    w->offset += 4u;
    return true;
}

static bool put_u64(tw_writer *w, uint64_t v) {
    uint32_t i;
    if (w == NULL || w->offset > w->size ||
        w->size - w->offset < 8u)
        return false;
    if (w->data != NULL) {
        for (i=0u;i<8u;++i)
            w->data[w->offset+i]=(uint8_t)(v>>(8u*i));
    }
    w->offset += 8u;
    return true;
}

static bool put_bytes(tw_writer *w, const uint8_t *p, size_t n) {
    if (w == NULL || (n != 0u && p == NULL) ||
        w->offset > w->size || n > w->size - w->offset)
        return false;
    if (n != 0u && w->data != NULL)
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

typedef struct tw_core_metadata_counts {
    uint32_t type_count;
    uint32_t function_count;
    uint32_t import_count;
    uint32_t export_count;
    uint32_t memory_count;
} tw_core_metadata_counts;

static bool source_span_offset(
    const turbowasm_module_impl *impl,
    const uint8_t *bytes,
    uint32_t size,
    uint64_t *out_offset) {
    uintptr_t base;
    uintptr_t address;
    uint64_t offset;

    if (impl == NULL || out_offset == NULL)
        return false;
    if (bytes == NULL) {
        if (size != 0u)
            return false;
        *out_offset = UINT64_MAX;
        return true;
    }

    base=(uintptr_t)impl->bytes;
    address=(uintptr_t)bytes;
    if(address<base)
        return false;
    offset=(uint64_t)(address-base);
    if(offset>(uint64_t)impl->size ||
       (uint64_t)size>(uint64_t)impl->size-offset)
        return false;
    *out_offset=offset;
    return true;
}

static bool read_source_span(
    tw_reader *r,
    uint64_t source_size) {
    uint64_t offset;
    uint32_t size;

    if(!get_u64(r,&offset)||!get_u32(r,&size))
        return false;
    if(offset==UINT64_MAX)
        return size==0u;
    return offset<=source_size &&
           (uint64_t)size<=source_size-offset;
}

static bool write_semantic_value(
    tw_writer *w,
    const turbowasm_validation_value_type *value) {
    return value != NULL &&
           put_u32(w,(uint32_t)value->carrier) &&
           put_u32(w,value->is_reference?1u:0u) &&
           put_u32(w,value->nullable?1u:0u) &&
           put_u32(w,(uint32_t)value->heap_kind) &&
           put_u32(w,value->type_index);
}

static bool read_semantic_value(tw_reader *r) {
    uint32_t carrier;
    uint32_t is_reference;
    uint32_t nullable;
    uint32_t heap_kind;
    uint32_t type_index;

    if(!get_u32(r,&carrier) ||
       !get_u32(r,&is_reference) ||
       !get_u32(r,&nullable) ||
       !get_u32(r,&heap_kind) ||
       !get_u32(r,&type_index))
        return false;
    (void)type_index;
    return carrier<=UINT8_MAX &&
           is_reference<=1u &&
           nullable<=1u &&
           heap_kind<=(uint32_t)TURBOWASM_VALIDATION_HEAP_TYPE_INDEX;
}

static bool write_name_span(
    tw_writer *w,
    const turbowasm_module_impl *impl,
    turbowasm_name name) {
    uint64_t offset;
    return source_span_offset(
               impl,name.bytes,name.size,&offset) &&
           put_u64(w,offset) &&
           put_u32(w,name.size);
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

static bool write_core_metadata(
    tw_writer *w,
    const turbowasm_module_impl *impl) {
    const turbowasm_validation_context *v;
    uint32_t i;
    uint32_t j;

    if(w==NULL||impl==NULL)
        return false;
    v=&impl->validation;

    if(!put_u32(w,v->type_count) ||
       !put_u32(w,v->function_count) ||
       !put_u32(w,v->import_count) ||
       !put_u32(w,v->export_count) ||
       !put_u32(w,v->memory_count))
        return false;

    for(i=0u;i<v->type_count;++i) {
        const turbowasm_validation_func_type *type=&v->types[i];
        if(!put_u32(w,type->defined?1u:0u) ||
           !put_u32(w,type->param_count) ||
           !put_u32(w,type->result_count))
            return false;
        if((type->param_count!=0u &&
            (type->params==NULL||type->param_semantics==NULL)) ||
           (type->result_count!=0u &&
            (type->results==NULL||type->result_semantics==NULL)))
            return false;
        for(j=0u;j<type->param_count;++j) {
            if(!put_u32(w,(uint32_t)type->params[j]) ||
               !write_semantic_value(w,&type->param_semantics[j]))
                return false;
        }
        for(j=0u;j<type->result_count;++j) {
            if(!put_u32(w,(uint32_t)type->results[j]) ||
               !write_semantic_value(w,&type->result_semantics[j]))
                return false;
        }
    }

    for(i=0u;i<v->function_count;++i) {
        const turbowasm_validation_function *function=&v->functions[i];
        uint64_t code_offset;
        if(!source_span_offset(
                impl,function->code,function->code_size,&code_offset) ||
           !put_u32(w,function->type_index) ||
           !put_u32(w,function->imported?1u:0u) ||
           !put_u32(w,function->local_count) ||
           !put_u64(w,code_offset) ||
           !put_u32(w,function->code_size) ||
           !put_u32(w,function->control_count))
            return false;
        if(function->local_count!=0u &&
           (function->local_types==NULL ||
            function->local_semantics==NULL))
            return false;
        if(function->control_count!=0u &&
           function->controls==NULL)
            return false;

        for(j=0u;j<function->local_count;++j) {
            if(!put_u32(w,(uint32_t)function->local_types[j]) ||
               !write_semantic_value(
                   w,&function->local_semantics[j]))
                return false;
        }

        for(j=0u;j<function->control_count;++j) {
            const turbowasm_validation_control *control=
                &function->controls[j];
            uint32_t k;
            if(!put_u32(w,(uint32_t)control->kind) ||
               !put_u32(w,control->opcode_offset) ||
               !put_u32(w,control->body_offset) ||
               !put_u32(w,control->else_offset) ||
               !put_u32(w,control->end_offset) ||
               !put_u32(w,control->type_index) ||
               !put_u32(w,(uint32_t)control->inline_result_type) ||
               !put_u32(w,control->catch_count))
                return false;
            if(control->catch_count!=0u &&
               control->catches==NULL)
                return false;
            for(k=0u;k<control->catch_count;++k) {
                const turbowasm_validation_catch *catch_=
                    &control->catches[k];
                if(!put_u32(w,(uint32_t)catch_->kind) ||
                   !put_u32(w,catch_->tag_index) ||
                   !put_u32(w,catch_->label_depth))
                    return false;
            }
        }
    }

    for(i=0u;i<v->import_count;++i) {
        const turbowasm_import_desc *import_desc=&v->imports[i];
        if(!write_name_span(w,impl,import_desc->module_name) ||
           !write_name_span(w,impl,import_desc->name) ||
           !put_u32(w,(uint32_t)import_desc->kind) ||
           !put_u32(w,import_desc->item_index) ||
           !put_u32(w,import_desc->type_index))
            return false;
    }

    for(i=0u;i<v->export_count;++i) {
        const turbowasm_export_desc *export_desc=&v->exports[i];
        if(!write_name_span(w,impl,export_desc->name) ||
           !put_u32(w,(uint32_t)export_desc->kind) ||
           !put_u32(w,export_desc->item_index))
            return false;
    }

    for(i=0u;i<v->memory_count;++i) {
        const turbowasm_validation_memory *memory=&v->memories[i];
        if(!put_u32(w,memory->imported?1u:0u) ||
           !put_u32(w,memory->shared?1u:0u) ||
           !put_u32(w,memory->memory64?1u:0u) ||
           !put_u32(w,memory->page_size) ||
           !put_u64(w,memory->limits.minimum) ||
           !put_u64(w,memory->limits.maximum) ||
           !put_u32(w,memory->limits.has_maximum?1u:0u))
            return false;
    }

    return true;
}

static bool measure_core_metadata(
    const turbowasm_module_impl *impl,
    size_t *out_size) {
    tw_writer w;
    if(impl==NULL||out_size==NULL)
        return false;
    w.data=NULL;
    w.size=SIZE_MAX;
    w.offset=0u;
    if(!write_core_metadata(&w,impl))
        return false;
    *out_size=w.offset;
    return true;
}

static bool read_core_metadata(
    tw_reader *r,
    uint64_t source_size,
    tw_core_metadata_counts *out) {
    uint32_t i;
    uint32_t j;
    tw_core_metadata_counts counts;

    if(r==NULL||out==NULL)
        return false;
    memset(&counts,0,sizeof(counts));
    if(!get_u32(r,&counts.type_count) ||
       !get_u32(r,&counts.function_count) ||
       !get_u32(r,&counts.import_count) ||
       !get_u32(r,&counts.export_count) ||
       !get_u32(r,&counts.memory_count))
        return false;

    for(i=0u;i<counts.type_count;++i) {
        uint32_t defined;
        uint32_t param_count;
        uint32_t result_count;
        if(!get_u32(r,&defined) ||
           !get_u32(r,&param_count) ||
           !get_u32(r,&result_count) ||
           defined>1u)
            return false;
        for(j=0u;j<param_count;++j) {
            uint32_t carrier;
            if(!get_u32(r,&carrier) ||
               carrier>UINT8_MAX ||
               !read_semantic_value(r))
                return false;
        }
        for(j=0u;j<result_count;++j) {
            uint32_t carrier;
            if(!get_u32(r,&carrier) ||
               carrier>UINT8_MAX ||
               !read_semantic_value(r))
                return false;
        }
    }

    for(i=0u;i<counts.function_count;++i) {
        uint32_t type_index;
        uint32_t imported;
        uint32_t local_count;
        uint32_t code_size;
        uint32_t control_count;
        uint64_t code_offset;
        if(!get_u32(r,&type_index) ||
           !get_u32(r,&imported) ||
           !get_u32(r,&local_count) ||
           !get_u64(r,&code_offset) ||
           !get_u32(r,&code_size) ||
           !get_u32(r,&control_count) ||
           imported>1u ||
           type_index>=counts.type_count)
            return false;
        if(code_offset==UINT64_MAX) {
            if(code_size!=0u)
                return false;
        } else if(code_offset>source_size ||
                  (uint64_t)code_size>source_size-code_offset) {
            return false;
        }

        for(j=0u;j<local_count;++j) {
            uint32_t carrier;
            if(!get_u32(r,&carrier) ||
               carrier>UINT8_MAX ||
               !read_semantic_value(r))
                return false;
        }

        for(j=0u;j<control_count;++j) {
            uint32_t kind;
            uint32_t opcode_offset;
            uint32_t body_offset;
            uint32_t else_offset;
            uint32_t end_offset;
            uint32_t type_index_or_none;
            uint32_t inline_result_type;
            uint32_t catch_count;
            uint32_t k;
            if(!get_u32(r,&kind) ||
               !get_u32(r,&opcode_offset) ||
               !get_u32(r,&body_offset) ||
               !get_u32(r,&else_offset) ||
               !get_u32(r,&end_offset) ||
               !get_u32(r,&type_index_or_none) ||
               !get_u32(r,&inline_result_type) ||
               !get_u32(r,&catch_count))
                return false;
            (void)opcode_offset;
            (void)body_offset;
            (void)else_offset;
            (void)end_offset;
            if(kind<(uint32_t)TURBOWASM_VALIDATION_CONTROL_BLOCK ||
               kind>(uint32_t)TURBOWASM_VALIDATION_CONTROL_TRY_TABLE ||
               inline_result_type>UINT8_MAX ||
               (type_index_or_none!=UINT32_MAX &&
                type_index_or_none>=counts.type_count))
                return false;
            for(k=0u;k<catch_count;++k) {
                uint32_t catch_kind;
                uint32_t tag_index;
                uint32_t label_depth;
                if(!get_u32(r,&catch_kind) ||
                   !get_u32(r,&tag_index) ||
                   !get_u32(r,&label_depth) ||
                   catch_kind>
                       (uint32_t)TURBOWASM_VALIDATION_CATCH_ALL_REF)
                    return false;
                (void)tag_index;
                (void)label_depth;
            }
        }
    }

    for(i=0u;i<counts.import_count;++i) {
        uint32_t kind;
        uint32_t item_index;
        uint32_t type_index;
        if(!read_source_span(r,source_size) ||
           !read_source_span(r,source_size) ||
           !get_u32(r,&kind) ||
           !get_u32(r,&item_index) ||
           !get_u32(r,&type_index) ||
           kind>(uint32_t)TURBOWASM_EXTERN_TAG)
            return false;
        (void)item_index;
        if(kind==TURBOWASM_EXTERN_FUNCTION ||
           kind==TURBOWASM_EXTERN_TAG) {
            if(type_index>=counts.type_count)
                return false;
        }
    }

    for(i=0u;i<counts.export_count;++i) {
        uint32_t kind;
        uint32_t item_index;
        if(!read_source_span(r,source_size) ||
           !get_u32(r,&kind) ||
           !get_u32(r,&item_index) ||
           kind>(uint32_t)TURBOWASM_EXTERN_TAG)
            return false;
        (void)item_index;
    }

    for(i=0u;i<counts.memory_count;++i) {
        uint32_t imported;
        uint32_t shared;
        uint32_t memory64;
        uint32_t page_size;
        uint64_t minimum;
        uint64_t maximum;
        uint32_t has_maximum;
        if(!get_u32(r,&imported) ||
           !get_u32(r,&shared) ||
           !get_u32(r,&memory64) ||
           !get_u32(r,&page_size) ||
           !get_u64(r,&minimum) ||
           !get_u64(r,&maximum) ||
           !get_u32(r,&has_maximum) ||
           imported>1u || shared>1u || memory64>1u ||
           has_maximum>1u || page_size==0u ||
           (shared!=0u && memory64!=0u) ||
           (has_maximum!=0u && maximum<minimum))
            return false;
    }

    *out=counts;
    return true;
}

turbowasm_status turbowasm_artifact_measure(
    const turbowasm_module *module,
    size_t *out_size) {
    const turbowasm_module_impl *impl=
        turbowasm_module_impl_get(module);
    size_t metadata_size;
    size_t fixed_size=
        TW_ARTIFACT_HEADER_SIZE +
        2u*TW_ARTIFACT_SECTION_HEADER_SIZE +
        TW_ARTIFACT_SUMMARY_SIZE;

    if(impl==NULL||out_size==NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if(!measure_core_metadata(impl,&metadata_size) ||
       metadata_size>SIZE_MAX-fixed_size)
        return TURBOWASM_OUT_OF_MEMORY;
    *out_size=fixed_size+metadata_size;
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_artifact_write(
    const turbowasm_module *module,
    uint8_t *output,
    size_t capacity,
    size_t *out_size) {
    const turbowasm_module_impl *impl=
        turbowasm_module_impl_get(module);
    size_t metadata_size;
    size_t required;
    uint8_t digest[TURBOWASM_SHA256_DIGEST_SIZE];
    tw_writer w;

    if(impl==NULL||out_size==NULL)
        return TURBOWASM_INVALID_ARGUMENT;
    if(!measure_core_metadata(impl,&metadata_size) ||
       metadata_size>UINT32_MAX)
        return TURBOWASM_OUT_OF_MEMORY;
    if(metadata_size>
       SIZE_MAX-(TW_ARTIFACT_HEADER_SIZE+
                 2u*TW_ARTIFACT_SECTION_HEADER_SIZE+
                 TW_ARTIFACT_SUMMARY_SIZE))
        return TURBOWASM_OUT_OF_MEMORY;
    required=
        TW_ARTIFACT_HEADER_SIZE+
        2u*TW_ARTIFACT_SECTION_HEADER_SIZE+
        TW_ARTIFACT_SUMMARY_SIZE+
        metadata_size;
    *out_size=required;
    if(output==NULL||capacity<required)
        return TURBOWASM_OUT_OF_MEMORY;

    turbowasm_sha256(impl->bytes,impl->size,digest);
    w.data=output; w.size=capacity; w.offset=0u;

    if(!put_bytes(&w,(const uint8_t *)"TWVA",4u) ||
       !put_u32(&w,TURBOWASM_ARTIFACT_SCHEMA_VERSION) ||
       !put_u32(&w,TW_ARTIFACT_HEADER_SIZE) ||
       !put_u32(&w,0u) ||
       !put_u64(&w,turbowasm_artifact_current_feature_fingerprint()) ||
       !put_u64(&w,(uint64_t)impl->size) ||
       !put_bytes(&w,digest,sizeof(digest)) ||
       !put_u32(&w,2u) ||
       !put_u32(&w,0u) ||
       !put_u32(&w,TURBOWASM_ARTIFACT_SECTION_SUMMARY) ||
       !put_u32(&w,TW_ARTIFACT_SUMMARY_SIZE) ||
       !write_summary(&w,&impl->summary) ||
       !put_u32(&w,TURBOWASM_ARTIFACT_SECTION_CORE_METADATA) ||
       !put_u32(&w,(uint32_t)metadata_size) ||
       !write_core_metadata(&w,impl) ||
       w.offset!=required)
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
    bool have_core_metadata=false;
    tw_core_metadata_counts metadata_counts={0};

    if(artifact==NULL||out==NULL||
       (source_size!=0u&&source==NULL))
        return TURBOWASM_INVALID_ARGUMENT;

    memset(out,0,sizeof(*out));
    r.data=artifact; r.size=artifact_size; r.offset=0u;
    if(!get_bytes(&r,magic,sizeof(magic)) ||
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

    if(out->schema_version!=TURBOWASM_ARTIFACT_SCHEMA_VERSION ||
       header_size!=TW_ARTIFACT_HEADER_SIZE ||
       reserved!=0u ||
       (out->flags&~TURBOWASM_ARTIFACT_FLAG_COMPLETE_METADATA)!=0u ||
       out->feature_fingerprint!=
           turbowasm_artifact_current_feature_fingerprint() ||
       out->source_size!=(uint64_t)source_size)
        return TURBOWASM_UNSUPPORTED;

    turbowasm_sha256(source,source_size,actual_digest);
    if(memcmp(actual_digest,out->source_sha256,sizeof(actual_digest))!=0)
        return TURBOWASM_TYPE_MISMATCH;

    for(section_index=0u;section_index<out->section_count;++section_index) {
        uint32_t type;
        uint32_t size;
        size_t end;
        if(!get_u32(&r,&type)||!get_u32(&r,&size) ||
           r.offset>r.size || (size_t)size>r.size-r.offset)
            return TURBOWASM_MALFORMED_MODULE;
        end=r.offset+(size_t)size;

        if(type==TURBOWASM_ARTIFACT_SECTION_SUMMARY) {
            tw_reader sr;
            if(have_summary||size!=TW_ARTIFACT_SUMMARY_SIZE)
                return TURBOWASM_MALFORMED_MODULE;
            sr.data=r.data+r.offset; sr.size=size; sr.offset=0u;
            if(!read_summary(&sr,&out->summary)||sr.offset!=sr.size)
                return TURBOWASM_MALFORMED_MODULE;
            have_summary=true;
        } else if(type==TURBOWASM_ARTIFACT_SECTION_CORE_METADATA) {
            tw_reader mr;
            if(have_core_metadata)
                return TURBOWASM_MALFORMED_MODULE;
            mr.data=r.data+r.offset; mr.size=size; mr.offset=0u;
            if(!read_core_metadata(
                    &mr,out->source_size,&metadata_counts) ||
               mr.offset!=mr.size)
                return TURBOWASM_MALFORMED_MODULE;
            have_core_metadata=true;
        } else {
            return TURBOWASM_MALFORMED_MODULE;
        }
        r.offset=end;
    }

    if(!have_summary||r.offset!=r.size)
        return TURBOWASM_MALFORMED_MODULE;

    if(have_core_metadata) {
        if(metadata_counts.type_count!=out->summary.type_count ||
           metadata_counts.function_count!=
               out->summary.imported_function_count+
               out->summary.function_count ||
           metadata_counts.import_count!=
               out->summary.imported_function_count+
               out->summary.imported_table_count+
               out->summary.imported_memory_count+
               out->summary.imported_global_count+
               out->summary.imported_tag_count ||
           metadata_counts.export_count!=out->summary.export_count ||
           metadata_counts.memory_count!=
               out->summary.imported_memory_count+
               out->summary.memory_count)
            return TURBOWASM_MALFORMED_MODULE;

        out->has_core_metadata=true;
        out->metadata_type_count=metadata_counts.type_count;
        out->metadata_function_count=metadata_counts.function_count;
        out->metadata_import_count=metadata_counts.import_count;
        out->metadata_export_count=metadata_counts.export_count;
        out->metadata_memory_count=metadata_counts.memory_count;
    }
    return TURBOWASM_OK;
}
