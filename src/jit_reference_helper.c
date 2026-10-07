#include "jit_reference_helper.h"
#include "gc_exec.h"

int64_t turbowasm_jit_reference(turbowasm_jit_invocation_context *context,
    int64_t opcode, int64_t immediate, turbowasm_value *out,
    const turbowasm_value *a, const turbowasm_value *b) {
    turbowasm_status status = TURBOWASM_INVALID_ARGUMENT;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    int64_t result = 0;
    bool is_null;
    if (context == NULL)
        return 0;
    if (context->instance == NULL)
        goto done;
    if (opcode == TURBOWASM_JIT_REF_FUNC) {
        const turbowasm_module_impl *module = turbowasm_module_impl_get(context->instance->module);
        if (out == NULL || immediate < 0 || immediate > UINT32_MAX || module == NULL ||
            (uint64_t)immediate >= module->validation.function_count)
            goto done;
        *out = (turbowasm_value){0};
        out->kind = TURBOWASM_VALUE_FUNCREF;
        out->as.funcref.function_index = (uint32_t)immediate;
        out->as.funcref.owner = context->instance;
        status = TURBOWASM_OK;
    } else if (opcode == TURBOWASM_JIT_REF_EQ) {
        if (a == NULL || b == NULL || a->kind != TURBOWASM_VALUE_GCREF ||
            b->kind != TURBOWASM_VALUE_GCREF)
            goto done;
        result = turbowasm_gc_equal(context->instance->store, a->as.gcref, b->as.gcref);
        status = TURBOWASM_OK;
    } else if (opcode == TURBOWASM_JIT_REF_IS_NULL || opcode == TURBOWASM_JIT_REF_AS_NON_NULL) {
        if (a == NULL || (opcode == TURBOWASM_JIT_REF_AS_NON_NULL && out == NULL))
            goto done;
        status = turbowasm_reference_is_null(a, &is_null);
        if (status != TURBOWASM_OK)
            goto done;
        if (opcode == TURBOWASM_JIT_REF_IS_NULL)
            result = is_null;
        else if (is_null) {
            status = TURBOWASM_TRAPPED;
            trap = TURBOWASM_TRAP_NULL_REFERENCE;
        } else
            *out = *a;
    } else
        status = TURBOWASM_UNSUPPORTED;
done:
    context->call_status = status;
    context->call_trap = trap;
    return result;
}
