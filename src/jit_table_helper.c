#include "jit_table_helper.h"

#include <string.h>

static uint64_t table_address(const turbowasm_validation_context *validation,
    uint32_t table, int64_t bits) {
    return validation->tables[table].limits.table64
        ? (uint64_t)bits : (uint64_t)(uint32_t)bits;
}

int64_t turbowasm_jit_table(
    turbowasm_jit_invocation_context *context, int64_t opcode,
    int64_t table, int64_t secondary, int64_t a, int64_t b, int64_t c,
    turbowasm_value *reference) {
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    const turbowasm_module_impl *module;
    turbowasm_instance_impl *instance;
    uint32_t size = 0u;
    uint64_t result = 0u;
    int64_t result_bits;
    if (context == NULL)
        return 0;
    instance = context->instance;
    if (instance == NULL || table < 0 || table > UINT32_MAX ||
        secondary < 0 || secondary > UINT32_MAX)
        goto done;
    module = turbowasm_module_impl_get(instance->module);
    if (module == NULL)
        goto done;
    if (opcode == TURBOWASM_JIT_ELEMENT_DROP) {
        status = turbowasm_instance_element_drop(instance, (uint32_t)secondary);
        goto done;
    }
    if ((uint64_t)table >= module->validation.table_count)
        goto done;
    if ((opcode == TURBOWASM_JIT_TABLE_GET || opcode == TURBOWASM_JIT_TABLE_SET ||
         opcode == TURBOWASM_JIT_TABLE_GROW || opcode == TURBOWASM_JIT_TABLE_FILL) &&
        reference == NULL)
        goto done;
    switch (opcode) {
        case TURBOWASM_JIT_TABLE_GET:
            status = turbowasm_instance_table_get_value(instance, (uint32_t)table,
                table_address(&module->validation, (uint32_t)table, a), reference);
            break;
        case TURBOWASM_JIT_TABLE_SET:
            status = turbowasm_instance_table_set_value(instance, (uint32_t)table,
                table_address(&module->validation, (uint32_t)table, a), *reference);
            break;
        case TURBOWASM_JIT_TABLE_GROW:
            status = turbowasm_instance_table_grow_wide(instance, (uint32_t)table,
                *reference, table_address(&module->validation, (uint32_t)table, b), &result);
            break;
        case TURBOWASM_JIT_TABLE_FILL:
            status = turbowasm_instance_table_fill(instance, (uint32_t)table,
                table_address(&module->validation, (uint32_t)table, a), *reference,
                table_address(&module->validation, (uint32_t)table, c));
            break;
        case TURBOWASM_JIT_TABLE_INIT:
            status = turbowasm_instance_table_init(instance, (uint32_t)secondary,
                (uint32_t)table, table_address(&module->validation, (uint32_t)table, a),
                (uint32_t)b, (uint32_t)c);
            break;
        case TURBOWASM_JIT_TABLE_COPY: {
            uint64_t length;
            if ((uint64_t)secondary >= module->validation.table_count)
                goto done;
            length = module->validation.tables[table].limits.table64 &&
                module->validation.tables[secondary].limits.table64
                ? (uint64_t)c : (uint64_t)(uint32_t)c;
            status = turbowasm_instance_table_copy(instance, (uint32_t)table,
                (uint32_t)secondary,
                table_address(&module->validation, (uint32_t)table, a),
                table_address(&module->validation, (uint32_t)secondary, b), length);
            break;
        }
        case TURBOWASM_JIT_TABLE_SIZE:
            status = turbowasm_instance_table_size(instance, (uint32_t)table, &size);
            result = size;
            break;
        default:
            status = TURBOWASM_UNSUPPORTED;
            break;
    }
done:
    context->call_status = status;
    context->call_trap = status == TURBOWASM_TRAPPED
        ? TURBOWASM_TRAP_TABLE_OUT_OF_BOUNDS : TURBOWASM_TRAP_NONE;
    memcpy(&result_bits, &result, sizeof(result_bits));
    return result_bits;
}
