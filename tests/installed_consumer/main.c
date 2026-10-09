#include <turbowasm/turbowasm.h>

#include <stdint.h>
#include <string.h>

static bool bounded_start(void) {
    static const uint8_t bytes[] = {
        0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
        1, 4, 1, 0x60, 0, 0, 3, 2, 1, 0, 8, 1, 0,
        10, 9, 1, 7, 0, 3, 0x40, 0x0c, 0, 0x0b, 0x0b
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    turbowasm_execution_options options = {.fuel = 32, .has_fuel_limit = true};
    bool passed = turbowasm_module_load_borrowed(&module, bytes, sizeof(bytes)) == TURBOWASM_OK &&
        turbowasm_linker_init(&linker) == TURBOWASM_OK &&
        turbowasm_instance_create_linked_with_options(&instance, &module, &linker,
            &options, &trap) == TURBOWASM_FUEL_EXHAUSTED &&
        instance.impl == NULL && trap == TURBOWASM_TRAP_NONE;
    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    return passed;
}

int main(void) {
    if (!bounded_start()) return 40;
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


    {
        static const uint8_t module_bytes[] = {
            0x00, 0x61, 0x73, 0x6d,
            0x01, 0x00, 0x00, 0x00,
            0x01, 0x05,
            0x01, 0x60, 0x00, 0x01, 0x7f,
            0x03, 0x02,
            0x01, 0x00,
            0x0a, 0x06,
            0x01, 0x04,
            0x00, 0x41, 0x07, 0x0b
        };
        turbowasm_module module = {0};
        turbowasm_instance instance = {0};
        turbowasm_execution execution = {0};
        turbowasm_execution_options options = {0};
        const turbowasm_value *value;

        if (turbowasm_module_load_borrowed(
                &module, module_bytes,
                sizeof(module_bytes)) != TURBOWASM_OK)
            return 15;
        if (turbowasm_instance_create(
                &instance, &module) != TURBOWASM_OK)
            return 16;
        if (turbowasm_execution_create(
                &execution, &instance, 0u, NULL, 0u) != TURBOWASM_OK)
            return 17;

        options.has_fuel_limit = true;
        options.fuel = 1u;
        if (turbowasm_execution_resume(
                &execution, &options) != TURBOWASM_YIELDED)
            return 18;
        if (turbowasm_execution_yield_reason_get(&execution) !=
                TURBOWASM_YIELD_FUEL)
            return 19;

        options.fuel = 4u;
        if (turbowasm_execution_resume(
                &execution, &options) != TURBOWASM_OK)
            return 20;
        if (turbowasm_execution_state_get(&execution) !=
                TURBOWASM_EXECUTION_COMPLETED)
            return 21;
        value = turbowasm_execution_result_at(&execution, 0u);
        if (value == NULL ||
            value->kind != TURBOWASM_VALUE_I32 ||
            value->as.i32 != 7)
            return 22;

        turbowasm_execution_destroy(&execution);
        turbowasm_instance_destroy(&instance);
        turbowasm_module_destroy(&module);
    }

    {
        static const uint8_t artifact_module_bytes[] = {
            0x00, 0x61, 0x73, 0x6d,
            0x01, 0x00, 0x00, 0x00,
            0x01, 0x05,
            0x01, 0x60, 0x00, 0x01, 0x7f,
            0x03, 0x02,
            0x01, 0x00,
            0x0a, 0x06,
            0x01, 0x04,
            0x00, 0x41, 0x2a, 0x0b
        };
        turbowasm_module fresh = {0};
        turbowasm_module restored = {0};
        turbowasm_instance restored_instance = {0};
        turbowasm_value restored_result = {0};
        turbowasm_trap restored_trap = TURBOWASM_TRAP_NONE;
        uint8_t artifact[4096] = {0};
        size_t artifact_required = 0u;
        size_t artifact_written = 0u;
        size_t restored_result_count = 0u;

        if (turbowasm_module_load_borrowed(
                &fresh,
                artifact_module_bytes,
                sizeof(artifact_module_bytes)) != TURBOWASM_OK)
            return 23;
        if (turbowasm_module_artifact_measure(
                &fresh, &artifact_required) != TURBOWASM_OK ||
            artifact_required == 0u ||
            artifact_required > sizeof(artifact)) {
            turbowasm_module_destroy(&fresh);
            return 24;
        }
        if (turbowasm_module_artifact_write(
                &fresh,
                artifact,
                sizeof(artifact),
                &artifact_written) != TURBOWASM_OK ||
            artifact_written != artifact_required) {
            turbowasm_module_destroy(&fresh);
            return 25;
        }
        if (turbowasm_module_load_borrowed_from_artifact(
                &restored,
                artifact_module_bytes,
                sizeof(artifact_module_bytes),
                artifact,
                artifact_written) != TURBOWASM_OK) {
            turbowasm_module_destroy(&fresh);
            return 26;
        }
        if (turbowasm_instance_create(
                &restored_instance, &restored) != TURBOWASM_OK) {
            turbowasm_module_destroy(&restored);
            turbowasm_module_destroy(&fresh);
            return 27;
        }
        if (turbowasm_instance_invoke(
                &restored_instance, 0u,
                NULL, 0u,
                &restored_result, 1u,
                &restored_result_count,
                &restored_trap) != TURBOWASM_OK ||
            restored_trap != TURBOWASM_TRAP_NONE ||
            restored_result_count != 1u ||
            restored_result.kind != TURBOWASM_VALUE_I32 ||
            restored_result.as.i32 != 42) {
            turbowasm_instance_destroy(&restored_instance);
            turbowasm_module_destroy(&restored);
            turbowasm_module_destroy(&fresh);
            return 28;
        }

        turbowasm_instance_destroy(&restored_instance);
        turbowasm_module_destroy(&restored);
        turbowasm_module_destroy(&fresh);
    }

    return 0;
}
