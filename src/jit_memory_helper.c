#include "jit_memory_helper.h"

#include <string.h>

static uint64_t guest_address(
    turbowasm_instance_impl *instance, uint32_t memory, int64_t bits) {
    const turbowasm_validation_context *validation =
        &turbowasm_module_impl_get(instance->module)->validation;
    return validation->memories[memory].memory64
        ? (uint64_t)bits : (uint64_t)(uint32_t)bits;
}

int64_t turbowasm_jit_memory(
    turbowasm_jit_invocation_context *context, int64_t opcode,
    int64_t memory, int64_t secondary, int64_t offset,
    int64_t a, int64_t b, int64_t c) {
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_instance_impl *instance;
    turbowasm_value value = {0};
    uint64_t result = 0u;
    uint64_t address;

    if (context == NULL)
        return 0;
    instance = context->instance;
    if (instance == NULL || memory < 0 || memory > UINT32_MAX ||
        secondary < 0 || secondary > UINT32_MAX)
        goto done;
    if (opcode == TURBOWASM_JIT_MEMORY_BULK + 9) {
        status = turbowasm_instance_data_drop(instance, (uint32_t)secondary);
        goto done;
    }
    if ((uint64_t)memory >= instance->memory_count)
        goto done;
    address = guest_address(instance, (uint32_t)memory, a);
    if (opcode >= 0x28 && opcode <= 0x35) {
        status = turbowasm_instance_memory_load_value(instance,
            (uint32_t)memory, address, (uint64_t)offset,
            (uint8_t)opcode, &value, &trap);
        if (status == TURBOWASM_OK) {
            if (value.kind == TURBOWASM_VALUE_I32)
                result = (uint64_t)(int64_t)value.as.i32;
            else if (value.kind == TURBOWASM_VALUE_I64)
                result = (uint64_t)value.as.i64;
            else if (value.kind == TURBOWASM_VALUE_F32) {
                uint32_t bits;
                memcpy(&bits, &value.as.f32, sizeof(bits));
                result = bits;
            } else
                memcpy(&result, &value.as.f64, sizeof(result));
        }
    } else if (opcode >= 0x36 && opcode <= 0x3e) {
        if (opcode == 0x38) {
            uint32_t bits = (uint32_t)b;
            value.kind = TURBOWASM_VALUE_F32;
            memcpy(&value.as.f32, &bits, sizeof(bits));
        } else if (opcode == 0x39) {
            value.kind = TURBOWASM_VALUE_F64;
            memcpy(&value.as.f64, &b, sizeof(b));
        } else if (opcode == 0x36 || opcode == 0x3a || opcode == 0x3b) {
            value.kind = TURBOWASM_VALUE_I32;
            value.as.i32 = (int32_t)(uint32_t)b;
        } else {
            value.kind = TURBOWASM_VALUE_I64;
            value.as.i64 = b;
        }
        status = turbowasm_instance_memory_store_value(instance,
            (uint32_t)memory, address, (uint64_t)offset,
            (uint8_t)opcode, value, &trap);
    } else if (opcode == 0x3f) {
        status = turbowasm_instance_memory_size64(instance,
            (uint32_t)memory, &result);
    } else if (opcode == 0x40) {
        status = turbowasm_instance_memory_grow64(instance,
            (uint32_t)memory, address, &result);
    } else if (opcode == TURBOWASM_JIT_MEMORY_BULK + 8) {
        status = turbowasm_instance_memory_init(instance, (uint32_t)secondary,
            (uint32_t)memory, address, (uint32_t)b, (uint32_t)c);
    } else if (opcode == TURBOWASM_JIT_MEMORY_BULK + 10) {
        const turbowasm_validation_context *validation =
            &turbowasm_module_impl_get(instance->module)->validation;
        uint64_t length;
        if ((uint64_t)secondary >= instance->memory_count)
            goto done;
        length = validation->memories[memory].memory64 &&
                 validation->memories[secondary].memory64
            ? (uint64_t)c : (uint64_t)(uint32_t)c;
        status = turbowasm_instance_memory_copy(instance,
            (uint32_t)memory, (uint32_t)secondary, address,
            guest_address(instance, (uint32_t)secondary, b), length);
    } else if (opcode == TURBOWASM_JIT_MEMORY_BULK + 11) {
        status = turbowasm_instance_memory_fill(instance, (uint32_t)memory,
            address, (uint8_t)b, guest_address(instance, (uint32_t)memory, c));
    } else {
        status = TURBOWASM_UNSUPPORTED;
    }
done:
    if (status == TURBOWASM_TRAPPED && trap == TURBOWASM_TRAP_NONE)
        trap = TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
    context->call_status = status;
    context->call_trap = trap;
    return (int64_t)result;
}

float turbowasm_jit_memory_load_f32(
    turbowasm_jit_invocation_context *context, int64_t memory,
    int64_t offset, int64_t address) {
    uint32_t bits = (uint32_t)turbowasm_jit_memory(
        context, 0x2a, memory, 0, offset, address, 0, 0);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

double turbowasm_jit_memory_load_f64(
    turbowasm_jit_invocation_context *context, int64_t memory,
    int64_t offset, int64_t address) {
    int64_t bits = turbowasm_jit_memory(
        context, 0x2b, memory, 0, offset, address, 0, 0);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

int64_t turbowasm_jit_memory_store_f32(
    turbowasm_jit_invocation_context *context, int64_t memory,
    int64_t offset, int64_t address, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return turbowasm_jit_memory(
        context, 0x38, memory, 0, offset, address, bits, 0);
}

int64_t turbowasm_jit_memory_store_f64(
    turbowasm_jit_invocation_context *context, int64_t memory,
    int64_t offset, int64_t address, double value) {
    int64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return turbowasm_jit_memory(
        context, 0x39, memory, 0, offset, address, bits, 0);
}
