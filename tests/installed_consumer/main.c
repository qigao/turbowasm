#include <turbowasm/turbowasm.h>

#include <stdint.h>
#include <string.h>

int main(void) {
    const int32_t left_data[4] = {1, 2, 3, 4};
    const int32_t right_data[4] = {4, 3, 2, 1};
    const int32_t expected[4] = {5, 5, 5, 5};
    int32_t actual[4] = {0};
    turbowasm_v128 left = {0};
    turbowasm_v128 right = {0};
    turbowasm_v128 result = {0};
    const cmeta_type_desc *i32_type =
        turbowasm_value_type_descriptor(TURBOWASM_VALUE_I32);
    const cmeta_type_desc *externref_type =
        turbowasm_value_type_descriptor(TURBOWASM_VALUE_EXTERNREF);
    turbowasm_value external = {0};

    external.kind = TURBOWASM_VALUE_EXTERNREF;
    external.as.externref.is_null = false;
    external.as.externref.token = (uintptr_t)7u;

    if (i32_type == NULL ||
        !cmeta_type_equal(i32_type, &cmeta_type_int32))
        return 7;
    if (externref_type == NULL ||
        externref_type->size != sizeof(turbowasm_externref) ||
        external.as.externref.token != (uintptr_t)7u)
        return 14;

    if (turbowasm_v128_load(
            &left, TURBOWASM_V128_I32X4, left_data) != TURBOWASM_OK)
        return 1;
    if (turbowasm_v128_load(
            &right, TURBOWASM_V128_I32X4, right_data) != TURBOWASM_OK)
        return 2;
    if (turbowasm_simd_i32x4_add(
            &result, &left, &right) != TURBOWASM_OK)
        return 3;
    if (result.shape != TURBOWASM_V128_I32X4)
        return 4;
    if (turbowasm_v128_store(actual, &result) != TURBOWASM_OK)
        return 5;
    if (memcmp(actual, expected, sizeof(actual)) != 0)
        return 6;

    {
        static const uint8_t module_bytes[] = {
            0x00, 0x61, 0x73, 0x6d,
            0x01, 0x00, 0x00, 0x00,
            0x01, 0x04,
            0x01, 0x60, 0x00, 0x00,
            0x02, 0x07,
            0x01,
            0x01, 0x6d,
            0x01, 0x66,
            0x00, 0x00,
            0x07, 0x05,
            0x01,
            0x01, 0x66,
            0x00, 0x00
        };
        turbowasm_module module = {0};
        const turbowasm_import_desc *import_desc;
        const turbowasm_export_desc *export_desc;

        if (turbowasm_module_load_borrowed(
                &module, module_bytes,
                sizeof(module_bytes)) != TURBOWASM_OK)
            return 8;
        if (turbowasm_module_import_count(&module) != 1u ||
            turbowasm_module_export_count(&module) != 1u)
            return 9;

        import_desc = turbowasm_module_import_at(&module, 0u);
        export_desc = turbowasm_module_export_at(&module, 0u);
        if (import_desc == NULL || export_desc == NULL)
            return 10;
        if (import_desc->kind != TURBOWASM_EXTERN_FUNCTION ||
            import_desc->item_index != 0u ||
            import_desc->type_index != 0u)
            return 11;
        if (import_desc->module_name.size != 1u ||
            import_desc->module_name.bytes[0] != (uint8_t)'m' ||
            import_desc->name.size != 1u ||
            import_desc->name.bytes[0] != (uint8_t)'f')
            return 12;
        if (export_desc->kind != TURBOWASM_EXTERN_FUNCTION ||
            export_desc->item_index != 0u ||
            export_desc->name.size != 1u ||
            export_desc->name.bytes[0] != (uint8_t)'f')
            return 13;

        turbowasm_module_destroy(&module);
    }

    return 0;
}
