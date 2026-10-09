#include <turbowasm/wasi.h>

#include "wasi_preview1_adapter_plan.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static turbowasm_wasi_preview1_adapter_carrier carrier_for_type(
    const cmeta_type_desc *type,
    int allow_void) {
    if (allow_void && cmeta_type_equal(type, &cmeta_type_void))
        return turbowasm_wasi_preview1_adapter_carrier_void;
    if (cmeta_type_equal(type, &cmeta_type_uint32))
        return turbowasm_wasi_preview1_adapter_carrier_u32;
    if (cmeta_type_equal(type, &cmeta_type_uint64))
        return turbowasm_wasi_preview1_adapter_carrier_u64;
    assert(!"unsupported Preview1 CMeta type");
    return turbowasm_wasi_preview1_adapter_carrier_void;
}

static turbowasm_value_kind public_kind_for_carrier(
    turbowasm_wasi_preview1_adapter_carrier carrier) {
    switch (carrier) {
    case turbowasm_wasi_preview1_adapter_carrier_u32:
        return TURBOWASM_VALUE_I32;
    case turbowasm_wasi_preview1_adapter_carrier_u64:
        return TURBOWASM_VALUE_I64;
    case turbowasm_wasi_preview1_adapter_carrier_void:
        break;
    }
    assert(!"void/unknown carrier is not a parameter kind");
    return TURBOWASM_VALUE_I32;
}

int main(void) {
    size_t count = turbowasm_wasi_preview1_function_count();
    size_t i;

    assert(count == turbowasm_wasi_preview1_adapter_function_count);
    assert(count == 30u);

    for (i = 0u; i < count; ++i) {
        const cmeta_function_desc *function =
            turbowasm_wasi_preview1_function_at(i);
        const turbowasm_wasi_preview1_adapter_function_plan *plan =
            &turbowasm_wasi_preview1_adapter_functions[i];
        size_t j;

        assert(function != NULL);
        assert(cmeta_function_desc_valid(function));
        assert(plan->source_ordinal == i);
        assert(strcmp(plan->function_name, function->name) == 0);
        assert(plan->param_count == function->param_count);
        assert(plan->effects == (uint32_t)function->effects);
        assert(plan->properties == (uint32_t)function->properties);
        assert(plan->return_carrier ==
               carrier_for_type(function->return_type, 1));

        if (plan->return_carrier !=
            turbowasm_wasi_preview1_adapter_carrier_void) {
            turbowasm_value_kind result_kind =
                public_kind_for_carrier(plan->return_carrier);
            assert(result_kind == TURBOWASM_VALUE_I32 ||
                   result_kind == TURBOWASM_VALUE_I64);
        }

        for (j = 0u; j < function->param_count; ++j) {
            const cmeta_param_desc *param =
                cmeta_function_param(function, j);
            assert(param != NULL);
            assert(plan->params != NULL);
            assert(strcmp(plan->params[j].name, param->name) == 0);
            assert(plan->params[j].flags == (uint32_t)param->flags);
            assert(plan->params[j].carrier ==
                   carrier_for_type(param->type, 0));
            assert(public_kind_for_carrier(plan->params[j].carrier) ==
                   (cmeta_type_equal(param->type, &cmeta_type_uint64)
                        ? TURBOWASM_VALUE_I64
                        : TURBOWASM_VALUE_I32));
        }
    }

    return 0;
}
