#include "validate_type.h"
#include "runtime_alloc.h"

#include <stddef.h>
#include <string.h>

/* Previous-group edges form a DAG; recursive edges stay inside a group and
 * are compared by projection. This bound protects native comparison stacks. */
#ifndef TURBOWASM_TYPE_DEPENDENCY_LIMIT
#define TURBOWASM_TYPE_DEPENDENCY_LIMIT 256u
#endif

static bool group_reference_equal(const turbowasm_validation_func_type *left,
                                  const turbowasm_validation_func_type *right,
                                  const turbowasm_validation_func_type *left_group,
                                  const turbowasm_validation_func_type *right_group) {
    bool left_local, right_local;
    if (left == NULL || right == NULL)
        return left == right;
    left_local = left->group == left_group;
    right_local = right->group == right_group;
    if (left_local || right_local)
        return left_local && right_local && left->group_offset == right->group_offset;
    return turbowasm_validation_defined_type_equal(left, right);
}

static bool group_value_equal(const turbowasm_validation_value_type *left,
                              const turbowasm_validation_value_type *right,
                              const turbowasm_validation_func_type *left_group,
                              const turbowasm_validation_func_type *right_group) {
    if (left->carrier != right->carrier || left->is_reference != right->is_reference ||
        left->nullable != right->nullable || left->heap_kind != right->heap_kind)
        return false;
    if (left->heap_kind != TURBOWASM_VALIDATION_HEAP_TYPE_INDEX)
        return true;
    return group_reference_equal(left->definition, right->definition, left_group, right_group);
}

bool turbowasm_validation_defined_type_equal(const turbowasm_validation_func_type *left,
                                             const turbowasm_validation_func_type *right) {
    const turbowasm_validation_func_type *lg, *rg;
    uint32_t i, j;
    if (left == NULL || right == NULL || !left->defined || !right->defined)
        return false;
    if (left == right || (left->canonical != NULL && left->canonical == right->canonical))
        return true;
    if (left->group_offset != right->group_offset || left->group_count != right->group_count)
        return false;
    lg = left->group;
    rg = right->group;
    if (lg == NULL || rg == NULL)
        return false;
    for (i = 0u; i < left->group_count; ++i) {
        const turbowasm_validation_func_type *a = &lg[i], *b = &rg[i];
        if (a->kind != b->kind || a->final_type != b->final_type ||
            a->param_count != b->param_count || a->result_count != b->result_count ||
            a->field_count != b->field_count || !group_reference_equal(a->super, b->super, lg, rg))
            return false;
        for (j = 0u; j < a->param_count; ++j)
            if (!group_value_equal(&a->param_semantics[j], &b->param_semantics[j], lg, rg))
                return false;
        for (j = 0u; j < a->result_count; ++j)
            if (!group_value_equal(&a->result_semantics[j], &b->result_semantics[j], lg, rg))
                return false;
        for (j = 0u; j < a->field_count; ++j)
            if (a->fields[j].packed_bits != b->fields[j].packed_bits ||
                a->fields[j].mutable_value != b->fields[j].mutable_value ||
                !group_value_equal(&a->fields[j].type, &b->fields[j].type, lg, rg))
                return false;
    }
    return true;
}

bool turbowasm_validation_defined_type_matches(const turbowasm_validation_func_type *actual,
                                               const turbowasm_validation_func_type *expected) {
    for (; actual != NULL; actual = actual->super)
        if (turbowasm_validation_defined_type_equal(actual, expected))
            return true;
    return false;
}

static turbowasm_status read_value(turbowasm_reader *reader,
                                   const turbowasm_validation_context *context, uint32_t limit,
                                   turbowasm_validation_value_type *type) {
    turbowasm_status status = turbowasm_validation_read_valtype(reader, context, type, NULL);
    if (status != TURBOWASM_OK)
        return status;
    if (type->heap_kind == TURBOWASM_VALIDATION_HEAP_TYPE_INDEX && type->type_index >= limit)
        return TURBOWASM_MALFORMED_MODULE;
    return TURBOWASM_OK;
}

static turbowasm_status read_vector(turbowasm_reader *reader,
                                    const turbowasm_validation_context *context, uint32_t limit,
                                    uint8_t **carriers, turbowasm_validation_value_type **values,
                                    uint32_t *count) {
    uint32_t i, size;
    if (!turbowasm_reader_uleb32(reader, &size) || size > turbowasm_reader_remaining(reader))
        return TURBOWASM_MALFORMED_MODULE;
    if (context != NULL && size != 0u) {
        *carriers = (uint8_t *)turbowasm_rt_calloc(size, sizeof(**carriers));
        *values = (turbowasm_validation_value_type *)turbowasm_rt_calloc(size, sizeof(**values));
        if (*carriers == NULL || *values == NULL)
            return TURBOWASM_OUT_OF_MEMORY;
    }
    *count = size;
    for (i = 0u; i < size; ++i) {
        turbowasm_validation_value_type type;
        turbowasm_status status = read_value(reader, context, limit, &type);
        if (status != TURBOWASM_OK)
            return status;
        if (context != NULL) {
            (*carriers)[i] = type.carrier;
            (*values)[i] = type;
        }
    }
    return TURBOWASM_OK;
}

static turbowasm_status read_definition(turbowasm_reader *reader,
                                        turbowasm_validation_context *context, uint32_t index,
                                        uint32_t group_start, uint32_t group_count) {
    turbowasm_validation_func_type scratch = {0};
    turbowasm_validation_func_type *type = context == NULL ? &scratch : &context->types[index];
    uint32_t i, super_count;
    uint8_t form;
    turbowasm_status status;
    type->super_index = UINT32_MAX;
    type->final_type = true;
    if (!turbowasm_reader_u8(reader, &form))
        return TURBOWASM_MALFORMED_MODULE;
    if (form == 0x4fu || form == 0x50u) {
        type->final_type = form == 0x4fu;
        if (!turbowasm_reader_uleb32(reader, &super_count) || super_count > 1u)
            return TURBOWASM_MALFORMED_MODULE;
        if (super_count != 0u &&
            (!turbowasm_reader_uleb32(reader, &type->super_index) || type->super_index >= index))
            return TURBOWASM_MALFORMED_MODULE;
        if (!turbowasm_reader_u8(reader, &form))
            return TURBOWASM_MALFORMED_MODULE;
    }
    if (form == 0x60u) {
        type->kind = TURBOWASM_TYPE_FUNCTION;
        status = read_vector(reader, context, group_start + group_count, &type->params,
                             &type->param_semantics, &type->param_count);
        if (status != TURBOWASM_OK)
            return status;
        status = read_vector(reader, context, group_start + group_count, &type->results,
                             &type->result_semantics, &type->result_count);
        if (status != TURBOWASM_OK)
            return status;
    } else if (form == 0x5fu || form == 0x5eu) {
        type->kind = form == 0x5fu ? TURBOWASM_TYPE_STRUCT : TURBOWASM_TYPE_ARRAY;
        type->field_count = 1u;
        if (form == 0x5fu && !turbowasm_reader_uleb32(reader, &type->field_count))
            return TURBOWASM_MALFORMED_MODULE;
        if (type->field_count > turbowasm_reader_remaining(reader) / 2u)
            return TURBOWASM_MALFORMED_MODULE;
        if (context != NULL && type->field_count != 0u) {
            type->fields = (turbowasm_validation_field *)turbowasm_rt_calloc(type->field_count,
                                                                             sizeof(*type->fields));
            if (type->fields == NULL)
                return TURBOWASM_OUT_OF_MEMORY;
        }
        for (i = 0u; i < type->field_count; ++i) {
            turbowasm_validation_field field = {0};
            uint8_t mutability;
            if (turbowasm_reader_remaining(reader) == 0u)
                return TURBOWASM_MALFORMED_MODULE;
            if (*reader->cursor == 0x78u || *reader->cursor == 0x77u) {
                field.packed_bits = *reader->cursor++ == 0x78u ? 8u : 16u;
                field.type = turbowasm_validation_value_type_legacy(0x7fu);
            } else {
                status = read_value(reader, context, group_start + group_count, &field.type);
                if (status != TURBOWASM_OK)
                    return status;
            }
            if (!turbowasm_reader_u8(reader, &mutability) || mutability > 1u)
                return TURBOWASM_MALFORMED_MODULE;
            field.mutable_value = mutability != 0u;
            if (context != NULL)
                type->fields[i] = field;
        }
    } else {
        return TURBOWASM_MALFORMED_MODULE;
    }
    type->defined = true;
    if (context != NULL) {
        type->group = &context->types[group_start];
        type->group_count = group_count;
        type->group_offset = index - group_start;
        type->canonical = type;
        if (type->super_index != UINT32_MAX)
            type->super = &context->types[type->super_index];
    }
    return TURBOWASM_OK;
}

static turbowasm_status read_groups(turbowasm_reader *reader, turbowasm_validation_context *context,
                                    uint32_t *out_count) {
    uint32_t groups, group, total = 0u;
    if (!turbowasm_reader_uleb32(reader, &groups))
        return TURBOWASM_MALFORMED_MODULE;
    for (group = 0u; group < groups; ++group) {
        uint32_t count = 1u, i;
        if (turbowasm_reader_remaining(reader) == 0u)
            return TURBOWASM_MALFORMED_MODULE;
        if (*reader->cursor == 0x4eu) {
            ++reader->cursor;
            if (!turbowasm_reader_uleb32(reader, &count))
                return TURBOWASM_MALFORMED_MODULE;
        }
        if (count > UINT32_MAX - total || count > turbowasm_reader_remaining(reader))
            return TURBOWASM_MALFORMED_MODULE;
        for (i = 0u; i < count; ++i) {
            turbowasm_status status = read_definition(reader, context, total + i, total, count);
            if (status != TURBOWASM_OK)
                return status;
        }
        total += count;
    }
    if (turbowasm_reader_remaining(reader) != 0u)
        return TURBOWASM_MALFORMED_MODULE;
    *out_count = total;
    return TURBOWASM_OK;
}

bool turbowasm_validation_bind_value(const turbowasm_validation_context *context,
                                     turbowasm_validation_value_type *value) {
    value->definition = NULL;
    if (value->heap_kind == TURBOWASM_VALIDATION_HEAP_TYPE_INDEX) {
        if (!value->is_reference || value->type_index >= context->type_count)
            return false;
        value->definition = &context->types[value->type_index];
        value->carrier = value->definition->kind == TURBOWASM_TYPE_FUNCTION ? 0x70u : 0x6eu;
    }
    return true;
}

static bool subtype_body(const turbowasm_validation_func_type *type) {
    const turbowasm_validation_func_type *parent = type->super;
    uint32_t i;
    if (parent == NULL)
        return true;
    if (parent->final_type || type->kind != parent->kind ||
        type->param_count != parent->param_count || type->result_count != parent->result_count ||
        type->field_count < parent->field_count)
        return false;
    for (i = 0u; i < type->param_count; ++i)
        if (!turbowasm_validation_value_type_matches(&parent->param_semantics[i],
                                                     &type->param_semantics[i]))
            return false;
    for (i = 0u; i < type->result_count; ++i)
        if (!turbowasm_validation_value_type_matches(&type->result_semantics[i],
                                                     &parent->result_semantics[i]))
            return false;
    for (i = 0u; i < parent->field_count; ++i) {
        const turbowasm_validation_field *a = &type->fields[i], *b = &parent->fields[i];
        if (a->mutable_value != b->mutable_value || a->packed_bits != b->packed_bits ||
            (a->mutable_value ? !turbowasm_validation_value_type_equal(&a->type, &b->type)
                              : !turbowasm_validation_value_type_matches(&a->type, &b->type)))
            return false;
    }
    return true;
}

turbowasm_status turbowasm_validation_types_finalize(turbowasm_validation_context *context) {
    uint32_t i, j;
    for (i = 0u; i < context->type_count; ++i) {
        turbowasm_validation_func_type *type = &context->types[i];
        if (type->kind != TURBOWASM_TYPE_FUNCTION)
            context->requires_store = true;
        if (type->group_offset > i || type->group_count <= type->group_offset ||
            type->group_count > context->type_count - (i - type->group_offset))
            return TURBOWASM_MALFORMED_MODULE;
        for (j = 0u; j < type->param_count; ++j) {
            if (!turbowasm_validation_bind_value(context, &type->param_semantics[j]))
                return TURBOWASM_MALFORMED_MODULE;
            type->params[j] = type->param_semantics[j].carrier;
        }
        for (j = 0u; j < type->result_count; ++j) {
            if (!turbowasm_validation_bind_value(context, &type->result_semantics[j]))
                return TURBOWASM_MALFORMED_MODULE;
            type->results[j] = type->result_semantics[j].carrier;
        }
        for (j = 0u; j < type->field_count; ++j)
            if (!turbowasm_validation_bind_value(context, &type->fields[j].type))
                return TURBOWASM_MALFORMED_MODULE;
    }
    for (i = 0u; i < context->type_count;) {
        uint32_t count = context->types[i].group_count, depth = 1u;
        uint64_t k;
        if (count == 0u || count > context->type_count - i)
            return TURBOWASM_MALFORMED_MODULE;
        for (j = 0u; j < count; ++j) {
            turbowasm_validation_func_type *type = &context->types[i + j];
            if (type->group != &context->types[i] || type->group_offset != j ||
                type->group_count != count)
                return TURBOWASM_MALFORMED_MODULE;
            if (type->super != NULL && type->super_index < i &&
                depth <= type->super->dependency_depth)
                depth = type->super->dependency_depth + 1u;
            for (k = 0u; k < (uint64_t)type->param_count + type->result_count + type->field_count;
                 ++k) {
                const turbowasm_validation_value_type *value =
                    k < type->param_count ? &type->param_semantics[k]
                    : k < (uint64_t)type->param_count + type->result_count
                        ? &type->result_semantics[k - type->param_count]
                        : &type->fields[k - type->param_count - type->result_count].type;
                if (value->definition != NULL && value->type_index >= i + count)
                    return TURBOWASM_MALFORMED_MODULE;
                if (value->definition != NULL && value->type_index < i &&
                    depth <= value->definition->dependency_depth)
                    depth = value->definition->dependency_depth + 1u;
            }
        }
        if (depth > TURBOWASM_TYPE_DEPENDENCY_LIMIT)
            return TURBOWASM_OUT_OF_MEMORY;
        for (j = 0u; j < count; ++j) {
            context->types[i + j].dependency_depth = depth;
            if (!subtype_body(&context->types[i + j]))
                return TURBOWASM_MALFORMED_MODULE;
        }
        i += count;
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_validate_composite_types(turbowasm_reader *section,
                                                    turbowasm_module_summary *summary,
                                                    turbowasm_validation_context *context) {
    turbowasm_reader scan = *section;
    uint32_t count;
    turbowasm_status status = read_groups(&scan, NULL, &count);
    if (status != TURBOWASM_OK)
        return status;
    if (!turbowasm_validation_context_allocate_types(context, count))
        return TURBOWASM_OUT_OF_MEMORY;
    status = read_groups(section, context, &count);
    if (status != TURBOWASM_OK)
        return status;
    summary->type_count = count;
    return turbowasm_validation_types_finalize(context);
}

bool turbowasm_validation_types_size(const turbowasm_validation_context *source, size_t *out) {
    uint64_t total = (uint64_t)source->type_count * sizeof(*source->types);
    uint32_t i;
    for (i = 0u; i < source->type_count; ++i) {
        const turbowasm_validation_func_type *type = &source->types[i];
        uint64_t bytes = ((uint64_t)type->param_count + type->result_count) *
                             (sizeof(turbowasm_validation_value_type) + sizeof(uint8_t)) +
                         (uint64_t)type->field_count * sizeof(*type->fields);
        if (bytes > SIZE_MAX || total > SIZE_MAX - bytes)
            return false;
        total += bytes;
    }
    if (total > SIZE_MAX)
        return false;
    *out = (size_t)total;
    return true;
}

turbowasm_status turbowasm_validation_clone_types(const turbowasm_validation_context *source,
                                                  turbowasm_validation_context *target) {
    uint32_t i;
    turbowasm_status status = TURBOWASM_OUT_OF_MEMORY;
    if (!turbowasm_validation_context_allocate_types(target, source->type_count))
        return status;
    for (i = 0u; i < source->type_count; ++i) {
        const turbowasm_validation_func_type *from = &source->types[i];
        turbowasm_validation_func_type *to = &target->types[i];
        if (!turbowasm_validation_context_define_type(target, i, from->param_count,
                                                      from->result_count))
            goto fail;
        to->kind = from->kind;
        to->final_type = from->final_type;
        to->super_index = from->super_index;
        to->super = from->super_index == UINT32_MAX ? NULL : &target->types[from->super_index];
        to->group_count = from->group_count;
        to->group_offset = from->group_offset;
        to->group = &target->types[i - from->group_offset];
        to->field_count = from->field_count;
        if (from->param_count != 0u)
            memcpy(to->param_semantics, from->param_semantics,
                   from->param_count * sizeof(*from->param_semantics));
        if (from->result_count != 0u)
            memcpy(to->result_semantics, from->result_semantics,
                   from->result_count * sizeof(*from->result_semantics));
        if (from->field_count != 0u) {
            to->fields = turbowasm_rt_calloc(from->field_count, sizeof(*to->fields));
            if (to->fields == NULL)
                goto fail;
            memcpy(to->fields, from->fields, from->field_count * sizeof(*to->fields));
        }
    }
    status = turbowasm_validation_types_finalize(target);
    if (status == TURBOWASM_OK)
        return status;
fail:
    turbowasm_validation_context_destroy(target);
    return status;
}
