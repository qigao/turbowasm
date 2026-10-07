#include "gc_exec.h"
#include "validate_type.h"
#include <string.h>

static turbowasm_status gc_trap(turbowasm_trap *trap, turbowasm_trap reason) {
    *trap = reason;
    return TURBOWASM_TRAPPED;
}

static turbowasm_value default_value(const turbowasm_validation_field *field) {
    turbowasm_value value = {0};
    value.kind = (turbowasm_value_kind)field->type.carrier;
    if (value.kind == TURBOWASM_VALUE_FUNCREF)
        value.as.funcref.is_null = true;
    if (value.kind == TURBOWASM_VALUE_EXTERNREF)
        value.as.externref.is_null = true;
    if (value.kind == TURBOWASM_VALUE_EXNREF)
        value.as.exnref.is_null = true;
    return value;
}

static turbowasm_value pack_value(turbowasm_value value, uint8_t bits) {
    if (bits != 0u)
        value.as.i32 = (int32_t)((uint32_t)value.as.i32 & ((UINT32_C(1) << bits) - 1u));
    return value;
}

static turbowasm_value unpack_value(turbowasm_value value, uint8_t bits, bool sign) {
    if (bits != 0u && sign) {
        uint32_t signbit = UINT32_C(1) << (bits - 1u);
        value.as.i32 = (int32_t)(((uint32_t)value.as.i32 ^ signbit) - signbit);
    }
    return value;
}

bool turbowasm_gc_equal(turbowasm_store_impl *store, turbowasm_gcref left, turbowasm_gcref right) {
    turbowasm_gc_object *a, *b;
    if (left.handle == right.handle && left.store == right.store)
        return true;
    if (left.handle == 0u && right.handle == 0u)
        return true;
    a = turbowasm_gc_resolve(store, left);
    b = turbowasm_gc_resolve(store, right);
    return a != NULL && b != NULL && a->type == NULL && b->type == NULL && a->count == 1u &&
           b->count == 1u && a->values[0].kind == TURBOWASM_VALUE_EXTERNREF &&
           b->values[0].kind == TURBOWASM_VALUE_EXTERNREF &&
           a->values[0].as.externref.token == b->values[0].as.externref.token;
}

static uint32_t field_bytes(const turbowasm_validation_field *field) {
    if (field->packed_bits != 0u)
        return field->packed_bits / 8u;
    switch (field->type.carrier) {
    case 0x7fu:
    case 0x7du:
        return 4u;
    case 0x7eu:
    case 0x7cu:
        return 8u;
    case 0x7bu:
        return 16u;
    default:
        return 0u;
    }
}

static turbowasm_status array_segment(turbowasm_instance_impl *instance, uint32_t op,
                                      uint32_t segment_index,
                                      const turbowasm_validation_field *field, uint32_t source,
                                      uint32_t length, turbowasm_gc_object *object,
                                      uint32_t destination, turbowasm_trap *trap) {
    const turbowasm_validation_context *context =
        &turbowasm_module_impl_get(instance->module)->validation;
    uint32_t i;
    if (op == 9u || op == 18u) {
        const turbowasm_validation_data_segment *segment = &context->data_segments[segment_index];
        uint32_t bytes = field_bytes(field);
        uint32_t available =
            instance->data_segment_dropped[segment_index] ? 0u : segment->data_size;
        if (bytes == 0u || source > available || (uint64_t)length * bytes > available - source)
            return gc_trap(trap, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        if (object == NULL)
            return TURBOWASM_OK;
        for (i = 0u; i < length; ++i) {
            const uint8_t *data = segment->data + source + (size_t)i * bytes;
            turbowasm_value value = default_value(field);
            uint64_t bits = 0u;
            uint32_t j;
            if (value.kind == TURBOWASM_VALUE_V128)
                cmeta_simd_v128_load(&value.as.v128.bits, data);
            else {
                for (j = 0u; j < bytes; ++j)
                    bits |= (uint64_t)data[j] << (j * 8u);
                switch (value.kind) {
                case TURBOWASM_VALUE_I32:
                    value.as.i32 = (int32_t)(uint32_t)bits;
                    break;
                case TURBOWASM_VALUE_I64:
                    memcpy(&value.as.i64, &bits, sizeof(bits));
                    break;
                case TURBOWASM_VALUE_F32: {
                    uint32_t low = (uint32_t)bits;
                    memcpy(&value.as.f32, &low, sizeof(low));
                    break;
                }
                case TURBOWASM_VALUE_F64:
                    memcpy(&value.as.f64, &bits, sizeof(bits));
                    break;
                default:
                    return TURBOWASM_TYPE_MISMATCH;
                }
            }
            object->values[destination + i] = value;
        }
    } else {
        const turbowasm_validation_element_segment *segment =
            &context->element_segments[segment_index];
        uint32_t available =
            instance->element_segment_dropped[segment_index] ? 0u : segment->item_count;
        if (source > available || length > available - source)
            return gc_trap(trap, TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS);
        if (object != NULL && length != 0u)
            memcpy(&object->values[destination], &instance->element_values[segment_index][source],
                   (size_t)length * sizeof(turbowasm_value));
    }
    return TURBOWASM_OK;
}

turbowasm_status turbowasm_gc_execute(turbowasm_instance_impl *instance, turbowasm_reader *reader,
                                      const turbowasm_value *values, uint32_t count,
                                      turbowasm_gc_effect *effect, turbowasm_trap *trap) {
    const turbowasm_validation_context *context =
        &turbowasm_module_impl_get(instance->module)->validation;
    const turbowasm_validation_func_type *type = NULL;
    const turbowasm_validation_field *field = NULL;
    turbowasm_gc_object *object = NULL;
    turbowasm_validation_value_type target, from;
    uint32_t op, index = 0u, extra = 0u, n = 0u, i, destination = 0u, source = 0u;
    turbowasm_status status;
    memset(effect, 0, sizeof(*effect));
    if (!turbowasm_reader_uleb32(reader, &op) || op > 30u)
        return TURBOWASM_MALFORMED_MODULE;
    if (op <= 19u && op != 15u) {
        if (!turbowasm_reader_uleb32(reader, &index) || index >= context->type_count)
            return TURBOWASM_MALFORMED_MODULE;
        type = &context->types[index];
        if (op >= 2u && op <= 5u) {
            if (!turbowasm_reader_uleb32(reader, &extra) || extra >= type->field_count)
                return TURBOWASM_MALFORMED_MODULE;
            field = &type->fields[extra];
        } else if (op >= 6u)
            field = &type->fields[0];
        if (op == 8u || op == 9u || op == 10u || op == 17u || op == 18u || op == 19u)
            if (!turbowasm_reader_uleb32(reader, &extra))
                return TURBOWASM_MALFORMED_MODULE;
    }
    switch (op) {
    case 0u:
        n = type->field_count;
        break;
    case 1u:
        n = 0u;
        break;
    case 5u:
    case 6u:
    case 9u:
    case 10u:
    case 11u:
    case 12u:
    case 13u:
        n = 2u;
        break;
    case 8u:
        n = extra;
        break;
    case 14u:
        n = 3u;
        break;
    case 16u:
    case 18u:
    case 19u:
        n = 4u;
        break;
    case 17u:
        n = 5u;
        break;
    default:
        n = 1u;
        break;
    }
    if (n > count)
        return TURBOWASM_MALFORMED_MODULE;
    effect->consumed = n;
    effect->produces = op != 5u && op != 14u && op != 16u && op != 17u && op != 18u && op != 19u;
#define ARG(k) values[count - effect->consumed + (k)]
    if (op <= 1u || (op >= 6u && op <= 10u)) {
        uint32_t length = op <= 1u   ? type->field_count
                          : op == 8u ? extra
                                     : (uint32_t)ARG(n - 1u).as.i32;
        if (instance->store == NULL)
            return TURBOWASM_INVALID_ARGUMENT;
        if (op == 9u || op == 10u) {
            status = array_segment(instance, op, extra, field, (uint32_t)ARG(0u).as.i32, length,
                                   NULL, 0u, trap);
            if (status != TURBOWASM_OK)
                return status;
        }
        status = turbowasm_gc_allocate(instance->store, &instance->store_types->types[index],
                                       length, &effect->value);
        if (status != TURBOWASM_OK)
            return status;
        object = turbowasm_gc_resolve(instance->store, effect->value.as.gcref);
        if (op == 9u || op == 10u)
            return array_segment(instance, op, extra, field, (uint32_t)ARG(0u).as.i32, length,
                                 object, 0u, trap);
        for (i = 0u; i < length; ++i) {
            const turbowasm_validation_field *element = op <= 1u ? &type->fields[i] : field;
            turbowasm_value value = op == 1u || op == 7u ? default_value(element)
                                    : op == 6u           ? ARG(0u)
                                                         : ARG(i);
            object->values[i] = pack_value(value, element->packed_bits);
        }
        return TURBOWASM_OK;
    }
    if (op <= 19u) {
        if (ARG(0u).as.gcref.handle == 0u)
            return gc_trap(trap, TURBOWASM_TRAP_NULL_REFERENCE);
        object = turbowasm_gc_resolve(instance->store, ARG(0u).as.gcref);
        if (object == NULL)
            return TURBOWASM_TYPE_MISMATCH;
        if (op <= 5u) {
            if (op == 5u)
                object->values[extra] = pack_value(ARG(1u), field->packed_bits);
            else
                effect->value = unpack_value(object->values[extra], field->packed_bits, op == 3u);
            return TURBOWASM_OK;
        }
        if (op == 15u) {
            effect->value.kind = TURBOWASM_VALUE_I32;
            effect->value.as.i32 = (int32_t)object->count;
            return TURBOWASM_OK;
        }
        if (op == 17u && ARG(2u).as.gcref.handle == 0u)
            return gc_trap(trap, TURBOWASM_TRAP_NULL_REFERENCE);
        destination = (uint32_t)ARG(1u).as.i32;
        n = op <= 14u ? 1u : (uint32_t)ARG(effect->consumed - 1u).as.i32;
        if (destination > object->count || n > object->count - destination)
            return gc_trap(trap, TURBOWASM_TRAP_ARRAY_OUT_OF_BOUNDS);
        if (op <= 13u)
            effect->value =
                unpack_value(object->values[destination], field->packed_bits, op == 12u);
        else if (op == 14u || op == 16u) {
            turbowasm_value value = pack_value(ARG(2u), field->packed_bits);
            for (i = 0u; i < n; ++i)
                object->values[destination + i] = value;
        } else if (op == 17u) {
            turbowasm_gc_object *other;
            if (ARG(2u).as.gcref.handle == 0u)
                return gc_trap(trap, TURBOWASM_TRAP_NULL_REFERENCE);
            other = turbowasm_gc_resolve(instance->store, ARG(2u).as.gcref);
            if (other == NULL)
                return TURBOWASM_TYPE_MISMATCH;
            source = (uint32_t)ARG(3u).as.i32;
            if (source > other->count || n > other->count - source)
                return gc_trap(trap, TURBOWASM_TRAP_ARRAY_OUT_OF_BOUNDS);
            memmove(&object->values[destination], &other->values[source],
                    (size_t)n * sizeof(turbowasm_value));
        } else
            return array_segment(instance, op, extra, field, (uint32_t)ARG(2u).as.i32, n, object,
                                 destination, trap);
        return TURBOWASM_OK;
    }
    if (op <= 25u) {
        uint8_t flags = 0u;
        bool matches;
        if (op >= 24u) {
            if (!turbowasm_reader_u8(reader, &flags) || flags > 3u ||
                !turbowasm_reader_uleb32(reader, &effect->label))
                return TURBOWASM_MALFORMED_MODULE;
            status = turbowasm_validation_read_heaptype(reader, context, &from);
            if (status != TURBOWASM_OK)
                return status;
        }
        status = turbowasm_validation_read_heaptype(reader, context, &target);
        if (status != TURBOWASM_OK)
            return status;
        target.nullable = op >= 24u ? (flags & 2u) != 0u : (op & 1u) != 0u;
        matches = turbowasm_value_matches_semantic(instance, &ARG(0u), &target);
        if (op <= 21u) {
            effect->value.kind = TURBOWASM_VALUE_I32;
            effect->value.as.i32 = matches ? 1 : 0;
        } else {
            if (op <= 23u && !matches)
                return gc_trap(trap, TURBOWASM_TRAP_CAST_FAILURE);
            effect->value = ARG(0u);
            effect->branches = (op == 24u && matches) || (op == 25u && !matches);
        }
        return TURBOWASM_OK;
    }
    if (op == 26u) {
        effect->value = ARG(0u);
        if (ARG(0u).kind == TURBOWASM_VALUE_MANAGED_EXTERNREF)
            effect->value.kind = TURBOWASM_VALUE_GCREF;
        else if (ARG(0u).as.externref.is_null) {
            memset(&effect->value, 0, sizeof(effect->value));
            effect->value.kind = TURBOWASM_VALUE_GCREF;
        } else {
            status = turbowasm_gc_allocate(instance->store, NULL, 1u, &effect->value);
            if (status != TURBOWASM_OK)
                return status;
            turbowasm_gc_resolve(instance->store, effect->value.as.gcref)->values[0] = ARG(0u);
        }
    } else if (op == 27u) {
        effect->value = ARG(0u);
        object = turbowasm_gc_resolve(instance->store, ARG(0u).as.gcref);
        if (ARG(0u).as.gcref.handle == 0u) {
            effect->value.kind = TURBOWASM_VALUE_EXTERNREF;
            effect->value.as.externref.is_null = true;
        } else if (object != NULL && object->type == NULL)
            effect->value = object->values[0];
        else
            effect->value.kind = TURBOWASM_VALUE_MANAGED_EXTERNREF;
    } else if (op == 28u)
        effect->value = turbowasm_gc_i31((uint32_t)ARG(0u).as.i32);
    else {
        uint32_t bits;
        if (ARG(0u).as.gcref.handle == 0u)
            return gc_trap(trap, TURBOWASM_TRAP_NULL_REFERENCE);
        bits = turbowasm_gc_i31_bits(ARG(0u).as.gcref);
        effect->value.kind = TURBOWASM_VALUE_I32;
        effect->value.as.i32 = op == 29u
                                   ? (int32_t)((bits ^ UINT32_C(0x40000000)) - UINT32_C(0x40000000))
                                   : (int32_t)bits;
    }
#undef ARG
    return TURBOWASM_OK;
}
