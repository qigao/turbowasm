#include <turbowasm/turbowasm.h>

#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

int main(void) {
    static const uint8_t provider_bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x07,
        0x01, 0x03, 0x69, 0x6e, 0x63, 0x00, 0x00,
        0x0a, 0x09,
        0x01, 0x07,
        0x00, 0x20, 0x00, 0x41, 0x01, 0x6a, 0x0b
    };
    static const uint8_t consumer_bytes[] = {
        WASM_HEADER,
        0x01, 0x06,
        0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        0x02, 0x0c,
        0x01,
        0x04, 0x6d, 0x61, 0x74, 0x68,
        0x03, 0x69, 0x6e, 0x63,
        0x00, 0x00,
        0x03, 0x02, 0x01, 0x00,
        0x0a, 0x08,
        0x01, 0x06,
        0x00, 0x20, 0x00, 0x10, 0x00, 0x0b
    };
    static const uint8_t math_name_bytes[] = {
        (uint8_t)'m', (uint8_t)'a',
        (uint8_t)'t', (uint8_t)'h'
    };
    const turbowasm_name math_name = {
        math_name_bytes, 4u
    };
    turbowasm_module provider_module = {0};
    turbowasm_module consumer_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_instance consumer = {0};
    turbowasm_linker linker = {0};
    turbowasm_value argument = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    int exit_code = 0;

    if (turbowasm_module_load_borrowed(
            &provider_module,
            provider_bytes,
            sizeof(provider_bytes)) != TURBOWASM_OK) {
        exit_code = 1;
        goto done;
    }
    if (turbowasm_instance_create(
            &provider,
            &provider_module) != TURBOWASM_OK) {
        exit_code = 2;
        goto done;
    }
    if (turbowasm_linker_init(&linker) != TURBOWASM_OK) {
        exit_code = 3;
        goto done;
    }
    if (turbowasm_linker_define_instance(
            &linker, math_name,
            &provider) != TURBOWASM_OK) {
        exit_code = 4;
        goto done;
    }
    if (turbowasm_module_load_borrowed(
            &consumer_module,
            consumer_bytes,
            sizeof(consumer_bytes)) != TURBOWASM_OK) {
        exit_code = 5;
        goto done;
    }
    if (turbowasm_instance_create_linked(
            &consumer,
            &consumer_module,
            &linker) != TURBOWASM_OK) {
        exit_code = 6;
        goto done;
    }

    turbowasm_linker_destroy(&linker);

    argument.kind = TURBOWASM_VALUE_I32;
    argument.as.i32 = 40;
    if (turbowasm_instance_invoke(
            &consumer, 1u,
            &argument, 1u,
            &result, 1u,
            &result_count,
            &trap) != TURBOWASM_OK) {
        exit_code = 7;
        goto done;
    }
    if (trap != TURBOWASM_TRAP_NONE ||
        result_count != 1u ||
        result.kind != TURBOWASM_VALUE_I32 ||
        result.as.i32 != 41) {
        exit_code = 8;
        goto done;
    }

done:
    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
    return exit_code;
}
