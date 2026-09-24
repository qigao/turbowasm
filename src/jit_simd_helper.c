#include "jit_simd_helper.h"

#include "simd_exec_table.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

static void turbowasm_jit_simd_record(
    turbowasm_jit_invocation_context *context,
    turbowasm_status status,
    turbowasm_trap trap) {
    if (context == NULL)
        return;
    context->call_status = status;
    context->call_trap = trap;
}

static salts_v128 *turbowasm_jit_simd_slot(
    turbowasm_jit_invocation_context *context,
    int64_t index) {
    if (context == NULL || context->simd_slots == NULL ||
        index < 0 || (uint64_t)index >= context->simd_slot_count)
        return NULL;
    return &context->simd_slots[(uint32_t)index];
}

static const salts_v128 *turbowasm_jit_simd_slot_const(
    const turbowasm_jit_invocation_context *context,
    int64_t index) {
    if (context == NULL || context->simd_slots == NULL ||
        index < 0 || (uint64_t)index >= context->simd_slot_count)
        return NULL;
    return &context->simd_slots[(uint32_t)index];
}

static int64_t turbowasm_jit_simd_status(
    turbowasm_jit_invocation_context *context,
    turbowasm_status status,
    turbowasm_trap trap) {
    if (status != TURBOWASM_OK)
        turbowasm_jit_simd_record(context, status, trap);
    return (int64_t)status;
}

static bool turbowasm_jit_simd_scalar_i64(
    const cmeta_vector_desc *desc,
    int64_t raw,
    salts_simd_scalar *out) {
    if (desc == NULL || out == NULL)
        return false;

    switch (desc->lane_kind) {
        case CMETA_VECTOR_I8:
            out->i8 = (int8_t)raw;
            return true;
        case CMETA_VECTOR_U8:
            out->u8 = (uint8_t)raw;
            return true;
        case CMETA_VECTOR_I16:
            out->i16 = (int16_t)raw;
            return true;
        case CMETA_VECTOR_U16:
            out->u16 = (uint16_t)raw;
            return true;
        case CMETA_VECTOR_I32:
            out->i32 = (int32_t)(uint32_t)raw;
            return true;
        case CMETA_VECTOR_U32:
            out->u32 = (uint32_t)raw;
            return true;
        case CMETA_VECTOR_I64:
            out->i64 = raw;
            return true;
        case CMETA_VECTOR_U64:
            out->u64 = (uint64_t)raw;
            return true;
        default:
            return false;
    }
}

int64_t turbowasm_jit_simd_const(
    turbowasm_jit_invocation_context *context,
    int64_t out_slot,
    int64_t low_bits,
    int64_t high_bits) {
    salts_v128 *out;
    uint8_t bytes[16];
    uint64_t low = (uint64_t)low_bits;
    uint64_t high = (uint64_t)high_bits;
    uint32_t index;

    out = turbowasm_jit_simd_slot(context, out_slot);
    if (out == NULL)
        return turbowasm_jit_simd_status(
            context, TURBOWASM_INVALID_ARGUMENT,
            TURBOWASM_TRAP_NONE);

    for (index = 0u; index < 8u; ++index) {
        bytes[index] = (uint8_t)(low >> (8u * index));
        bytes[8u + index] = (uint8_t)(high >> (8u * index));
    }

    salts_simd_v128_load(out, bytes);
    return (int64_t)TURBOWASM_OK;
}

int64_t turbowasm_jit_simd_splat_i64(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t out_slot,
    int64_t scalar) {
    const turbowasm_simd_exec_descriptor *descriptor;
    salts_simd_scalar value = {0};
    salts_v128 *out;

    descriptor = turbowasm_simd_exec_descriptor_find((uint32_t)opcode);
    out = turbowasm_jit_simd_slot(context, out_slot);
    if (descriptor == NULL ||
        descriptor->kind != TURBOWASM_SIMD_EXEC_SPLAT ||
        out == NULL ||
        !turbowasm_jit_simd_scalar_i64(
            descriptor->vector_desc, scalar, &value) ||
        !salts_simd_splat(descriptor->vector_desc, out, value)) {
        return turbowasm_jit_simd_status(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);
    }

    return (int64_t)TURBOWASM_OK;
}

int64_t turbowasm_jit_simd_splat_f32(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t out_slot,
    float scalar) {
    const turbowasm_simd_exec_descriptor *descriptor;
    salts_simd_scalar value = {0};
    salts_v128 *out;

    descriptor = turbowasm_simd_exec_descriptor_find((uint32_t)opcode);
    out = turbowasm_jit_simd_slot(context, out_slot);
    if (descriptor == NULL ||
        descriptor->kind != TURBOWASM_SIMD_EXEC_SPLAT ||
        descriptor->vector_desc != &cmeta_vector_f32x4 ||
        out == NULL) {
        return turbowasm_jit_simd_status(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);
    }

    value.f32 = scalar;
    if (!salts_simd_splat(descriptor->vector_desc, out, value))
        return turbowasm_jit_simd_status(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);

    return (int64_t)TURBOWASM_OK;
}

int64_t turbowasm_jit_simd_splat_f64(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t out_slot,
    double scalar) {
    const turbowasm_simd_exec_descriptor *descriptor;
    salts_simd_scalar value = {0};
    salts_v128 *out;

    descriptor = turbowasm_simd_exec_descriptor_find((uint32_t)opcode);
    out = turbowasm_jit_simd_slot(context, out_slot);
    if (descriptor == NULL ||
        descriptor->kind != TURBOWASM_SIMD_EXEC_SPLAT ||
        descriptor->vector_desc != &cmeta_vector_f64x2 ||
        out == NULL) {
        return turbowasm_jit_simd_status(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);
    }

    value.f64 = scalar;
    if (!salts_simd_splat(descriptor->vector_desc, out, value))
        return turbowasm_jit_simd_status(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);

    return (int64_t)TURBOWASM_OK;
}

int64_t turbowasm_jit_simd_op(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t out_slot,
    int64_t a_slot,
    int64_t b_slot,
    int64_t c_slot,
    int64_t scalar_count) {
    const turbowasm_simd_exec_descriptor *descriptor;
    salts_v128 *out;
    const salts_v128 *a;
    const salts_v128 *b;
    const salts_v128 *c;
    bool supported = false;

    descriptor = turbowasm_simd_exec_descriptor_find((uint32_t)opcode);
    out = turbowasm_jit_simd_slot(context, out_slot);
    a = turbowasm_jit_simd_slot_const(context, a_slot);
    b = turbowasm_jit_simd_slot_const(context, b_slot);
    c = turbowasm_jit_simd_slot_const(context, c_slot);

    if (descriptor == NULL || out == NULL)
        return turbowasm_jit_simd_status(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);

    switch (descriptor->kind) {
        case TURBOWASM_SIMD_EXEC_UNARY:
            supported = a != NULL &&
                salts_simd_unary(
                    descriptor->vector_desc,
                    (salts_simd_unary_op)descriptor->op,
                    out, a);
            break;

        case TURBOWASM_SIMD_EXEC_BINARY:
            supported = a != NULL && b != NULL &&
                salts_simd_binary(
                    descriptor->vector_desc,
                    (salts_simd_binary_op)descriptor->op,
                    out, a, b);
            break;

        case TURBOWASM_SIMD_EXEC_COMPARE:
            supported = a != NULL && b != NULL &&
                salts_simd_compare(
                    descriptor->vector_desc,
                    (salts_simd_compare_op)descriptor->op,
                    out, a, b);
            break;

        case TURBOWASM_SIMD_EXEC_SHIFT:
            supported = a != NULL &&
                salts_simd_shift(
                    descriptor->vector_desc,
                    (salts_simd_shift_op)descriptor->op,
                    out, a, (uint32_t)scalar_count);
            break;

        case TURBOWASM_SIMD_EXEC_SELECT:
            supported = a != NULL && b != NULL && c != NULL &&
                salts_simd_select(
                    descriptor->vector_desc,
                    out, a, b, c);
            break;

        case TURBOWASM_SIMD_EXEC_SATURATING:
            supported = a != NULL && b != NULL &&
                salts_simd_saturating_binary(
                    descriptor->vector_desc,
                    (salts_simd_saturating_op)descriptor->op,
                    out, a, b);
            break;

        case TURBOWASM_SIMD_EXEC_NARROW:
            supported = a != NULL && b != NULL &&
                salts_simd_narrow(
                    descriptor->vector_desc, out, a, b);
            break;

        case TURBOWASM_SIMD_EXEC_EXTEND_HALF:
            supported = a != NULL &&
                salts_simd_extend_half(
                    descriptor->vector_desc,
                    (salts_simd_half)descriptor->op,
                    out, a);
            break;

        case TURBOWASM_SIMD_EXEC_EXTMUL_HALF:
            supported = a != NULL && b != NULL &&
                salts_simd_extmul_half(
                    descriptor->vector_desc,
                    (salts_simd_half)descriptor->op,
                    out, a, b);
            break;

        case TURBOWASM_SIMD_EXEC_EXTADD_PAIRWISE:
            supported = a != NULL &&
                salts_simd_extadd_pairwise(
                    descriptor->vector_desc, out, a);
            break;

        case TURBOWASM_SIMD_EXEC_Q15MULR:
            supported = a != NULL && b != NULL &&
                salts_simd_q15mulr_sat(
                    descriptor->vector_desc, out, a, b);
            break;

        case TURBOWASM_SIMD_EXEC_DOT_PAIRWISE:
            supported = a != NULL && b != NULL &&
                salts_simd_dot_pairwise(
                    descriptor->vector_desc, out, a, b);
            break;

        case TURBOWASM_SIMD_EXEC_CONVERT:
            supported = a != NULL && descriptor->source_desc != NULL &&
                salts_simd_convert(
                    descriptor->vector_desc,
                    descriptor->source_desc,
                    (salts_simd_convert_op)descriptor->op,
                    (salts_simd_lane_policy)descriptor->lane_policy,
                    out, a);
            break;

        case TURBOWASM_SIMD_EXEC_SWIZZLE:
            supported = a != NULL && b != NULL &&
                salts_simd_swizzle_bytes(out, a, b);
            break;

        default:
            supported = false;
            break;
    }

    if (!supported)
        return turbowasm_jit_simd_status(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);

    return (int64_t)TURBOWASM_OK;
}

int64_t turbowasm_jit_simd_reduce(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t in_slot) {
    const turbowasm_simd_exec_descriptor *descriptor;
    const salts_v128 *in;
    uint32_t value = 0u;

    descriptor = turbowasm_simd_exec_descriptor_find((uint32_t)opcode);
    in = turbowasm_jit_simd_slot_const(context, in_slot);
    if (descriptor == NULL ||
        descriptor->kind != TURBOWASM_SIMD_EXEC_REDUCE ||
        in == NULL ||
        !salts_simd_reduce(
            descriptor->vector_desc,
            (salts_simd_reduce_op)descriptor->op,
            in, &value)) {
        turbowasm_jit_simd_record(
            context, TURBOWASM_UNSUPPORTED,
            TURBOWASM_TRAP_NONE);
        return 0;
    }

    return (int64_t)(uint32_t)value;
}

int64_t turbowasm_jit_simd_memory(
    turbowasm_jit_invocation_context *context,
    int64_t opcode,
    int64_t slot,
    int64_t address,
    int64_t offset) {
    salts_v128 *value;
    uint8_t *memory;
    turbowasm_status status;

    if (context == NULL || context->instance == NULL ||
        address < INT32_MIN || address > UINT32_MAX ||
        offset < 0 || offset > UINT32_MAX) {
        return turbowasm_jit_simd_status(
            context, TURBOWASM_INVALID_ARGUMENT,
            TURBOWASM_TRAP_NONE);
    }

    value = turbowasm_jit_simd_slot(context, slot);
    if (value == NULL)
        return turbowasm_jit_simd_status(
            context, TURBOWASM_INVALID_ARGUMENT,
            TURBOWASM_TRAP_NONE);

    if ((uint32_t)opcode == 0x00u) {
        status = turbowasm_instance_memory_bounds(
            context->instance, 0u,
            (uint32_t)address, (uint32_t)offset,
            16u, &memory);
        if (status != TURBOWASM_OK) {
            return turbowasm_jit_simd_status(
                context,
                status == TURBOWASM_TRAPPED
                    ? TURBOWASM_TRAPPED
                    : status,
                status == TURBOWASM_TRAPPED
                    ? TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS
                    : TURBOWASM_TRAP_NONE);
        }
        salts_simd_v128_load(value, memory);
        return (int64_t)TURBOWASM_OK;
    }

    if ((uint32_t)opcode == 0x0bu) {
        status = turbowasm_instance_memory_bounds(
            context->instance, 0u,
            (uint32_t)address, (uint32_t)offset,
            16u, &memory);
        if (status != TURBOWASM_OK) {
            return turbowasm_jit_simd_status(
                context,
                status == TURBOWASM_TRAPPED
                    ? TURBOWASM_TRAPPED
                    : status,
                status == TURBOWASM_TRAPPED
                    ? TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS
                    : TURBOWASM_TRAP_NONE);
        }
        salts_simd_v128_store(memory, value);
        return (int64_t)TURBOWASM_OK;
    }

    return turbowasm_jit_simd_status(
        context, TURBOWASM_UNSUPPORTED,
        TURBOWASM_TRAP_NONE);
}
