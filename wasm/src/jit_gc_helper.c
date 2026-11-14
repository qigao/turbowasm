#include "jit_gc_helper.h"
#include "gc_exec.h"
#include "validate_type.h"
#include <string.h>

uint8_t turbowasm_jit_gc_input_type(const turbowasm_jit_gc_instruction *op, uint32_t index) {
    if (op == NULL || index >= op->input_count) return 0;
    if (op->fields != NULL) return op->fields[index].type.carrier;
    if (op->repeated_type != 0) return op->repeated_type;
    return index < TURBOWASM_GC_FIXED_INPUT_LIMIT ? op->inputs[index] : 0;
}

bool turbowasm_jit_gc_decode(const turbowasm_validation_context *validation,
    turbowasm_reader *reader, turbowasm_jit_gc_instruction *out) {
    const turbowasm_validation_func_type *type = NULL;
    const turbowasm_validation_field *field = NULL;
    turbowasm_validation_value_type target, from;
    uint32_t index, extra = 0;
    uint8_t flags;
    if (validation == NULL || reader == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->code = reader->cursor;
    if (!turbowasm_reader_uleb32(reader, &out->opcode) || out->opcode > TURBOWASM_GC_I31_GET_U)
        return false;
    if (out->opcode <= TURBOWASM_GC_ARRAY_INIT_ELEM && out->opcode != TURBOWASM_GC_ARRAY_LEN) {
        if (!turbowasm_reader_uleb32(reader, &index) || index >= validation->type_count) return false;
        type = &validation->types[index];
        if (type->kind != (out->opcode <= TURBOWASM_GC_STRUCT_SET ? TURBOWASM_TYPE_STRUCT : TURBOWASM_TYPE_ARRAY))
            return false;
        if (out->opcode >= TURBOWASM_GC_STRUCT_GET && out->opcode <= TURBOWASM_GC_STRUCT_SET) {
            if (!turbowasm_reader_uleb32(reader, &extra) || extra >= type->field_count) return false;
            field = &type->fields[extra];
        } else if (out->opcode >= TURBOWASM_GC_ARRAY_NEW) {
            if (type->field_count != 1) return false;
            field = type->fields;
        }
        if (out->opcode == TURBOWASM_GC_ARRAY_NEW_FIXED || out->opcode == TURBOWASM_GC_ARRAY_NEW_DATA ||
            out->opcode == TURBOWASM_GC_ARRAY_NEW_ELEM || out->opcode == TURBOWASM_GC_ARRAY_COPY ||
            out->opcode == TURBOWASM_GC_ARRAY_INIT_DATA || out->opcode == TURBOWASM_GC_ARRAY_INIT_ELEM)
            if (!turbowasm_reader_uleb32(reader, &extra)) return false;
    }
    out->input_count = 1;
    out->inputs[0] = TURBOWASM_VALUE_GCREF;
    out->result_type = TURBOWASM_VALUE_GCREF;
    switch (out->opcode) {
    case TURBOWASM_GC_STRUCT_NEW:
        out->input_count = type->field_count; out->fields = type->fields; break;
    case TURBOWASM_GC_STRUCT_NEW_DEFAULT:
        out->input_count = 0; break;
    case TURBOWASM_GC_STRUCT_GET: case TURBOWASM_GC_STRUCT_GET_S: case TURBOWASM_GC_STRUCT_GET_U:
        out->result_type = field->type.carrier; break;
    case TURBOWASM_GC_STRUCT_SET:
        out->input_count = 2; out->inputs[1] = field->type.carrier; out->result_type = 0; break;
    case TURBOWASM_GC_ARRAY_NEW:
        out->input_count = 2; out->inputs[0] = field->type.carrier;
        out->inputs[1] = TURBOWASM_VALUE_I32; break;
    case TURBOWASM_GC_ARRAY_NEW_DEFAULT:
        out->inputs[0] = TURBOWASM_VALUE_I32; break;
    case TURBOWASM_GC_ARRAY_NEW_FIXED:
        out->input_count = extra; out->repeated_type = field->type.carrier; break;
    case TURBOWASM_GC_ARRAY_NEW_DATA: case TURBOWASM_GC_ARRAY_NEW_ELEM:
        out->input_count = 2; out->inputs[0] = out->inputs[1] = TURBOWASM_VALUE_I32; break;
    case TURBOWASM_GC_ARRAY_GET: case TURBOWASM_GC_ARRAY_GET_S: case TURBOWASM_GC_ARRAY_GET_U:
        out->input_count = 2; out->inputs[1] = TURBOWASM_VALUE_I32;
        out->result_type = field->type.carrier; break;
    case TURBOWASM_GC_ARRAY_SET: case TURBOWASM_GC_ARRAY_FILL:
        out->input_count = out->opcode == TURBOWASM_GC_ARRAY_SET ? 3 : 4;
        out->inputs[1] = out->inputs[3] = TURBOWASM_VALUE_I32;
        out->inputs[2] = field->type.carrier; out->result_type = 0; break;
    case TURBOWASM_GC_ARRAY_LEN: case TURBOWASM_GC_I31_GET_S: case TURBOWASM_GC_I31_GET_U:
        out->result_type = TURBOWASM_VALUE_I32; break;
    case TURBOWASM_GC_ARRAY_COPY:
        out->input_count = 5; out->result_type = 0;
        out->inputs[1] = out->inputs[3] = out->inputs[4] = TURBOWASM_VALUE_I32;
        out->inputs[2] = TURBOWASM_VALUE_GCREF; break;
    case TURBOWASM_GC_ARRAY_INIT_DATA: case TURBOWASM_GC_ARRAY_INIT_ELEM:
        out->input_count = 4; out->result_type = 0;
        out->inputs[1] = out->inputs[2] = out->inputs[3] = TURBOWASM_VALUE_I32; break;
    case TURBOWASM_GC_BR_ON_CAST: case TURBOWASM_GC_BR_ON_CAST_FAIL:
        if (!turbowasm_reader_u8(reader, &flags) || flags > 3 ||
            !turbowasm_reader_uleb32(reader, &out->label) ||
            turbowasm_validation_read_heaptype(reader, validation, &from) != TURBOWASM_OK) return false;
        out->branches = true;
        /* fall through */
    case TURBOWASM_GC_REF_TEST: case TURBOWASM_GC_REF_TEST_NULL:
    case TURBOWASM_GC_REF_CAST: case TURBOWASM_GC_REF_CAST_NULL:
        if (turbowasm_validation_read_heaptype(reader, validation, &target) != TURBOWASM_OK) return false;
        out->inputs[0] = target.carrier;
        out->result_type = out->opcode <= TURBOWASM_GC_REF_TEST_NULL ? TURBOWASM_VALUE_I32 : target.carrier;
        break;
    case TURBOWASM_GC_ANY_CONVERT_EXTERN:
        out->inputs[0] = TURBOWASM_VALUE_EXTERNREF; break;
    case TURBOWASM_GC_EXTERN_CONVERT_ANY:
        out->result_type = TURBOWASM_VALUE_EXTERNREF; break;
    case TURBOWASM_GC_REF_I31:
        out->inputs[0] = TURBOWASM_VALUE_I32; break;
    default: return false;
    }
    out->size = (uint32_t)(reader->cursor - out->code);
    return true;
}

int64_t turbowasm_jit_gc(turbowasm_jit_invocation_context *context,
    const uint8_t *code, int64_t size, const turbowasm_value *args, int64_t count,
    turbowasm_value *result) {
    turbowasm_reader reader;
    turbowasm_gc_effect effect = {0};
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    if (context == NULL) return 0;
    if (context->instance == NULL || code == NULL || size <= 0 || (uint64_t)size > SIZE_MAX ||
        count < 0 || count > UINT32_MAX || (count != 0 && args == NULL) || result == NULL) goto done;
    /* Only admitted module instructions reach this private boundary. Args are
     * already rooted, including constructor operands during allocation. */
    turbowasm_reader_init(&reader, code, (size_t)size);
    status = turbowasm_gc_execute(context->instance, &reader, args, (uint32_t)count, &effect, &trap);
    if (status == TURBOWASM_OK && (effect.consumed != (uint32_t)count || turbowasm_reader_remaining(&reader) != 0))
        status = TURBOWASM_MALFORMED_MODULE;
    if (status == TURBOWASM_OK) *result = effect.value;
done:
    context->call_status = status; context->call_trap = trap;
    return status == TURBOWASM_OK && effect.branches;
}
