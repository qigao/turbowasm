#include <turbowasm/turbowasm.h>

#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static turbowasm_status host_inc(
    void *context,
    turbowasm_instance *caller,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    (void)context;

    if (caller == NULL ||
        turbowasm_instance_module(caller) == NULL ||
        argument_count != 1u ||
        result_capacity < 1u ||
        arguments == NULL ||
        results == NULL ||
        result_count == NULL ||
        trap == NULL ||
        arguments[0].kind != TURBOWASM_VALUE_I32)
        return TURBOWASM_INVALID_ARGUMENT;

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = arguments[0].as.i32 + 1;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

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

    {
        static const uint8_t host_consumer_bytes[] = {
            WASM_HEADER,
            0x01, 0x06,
            0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
            0x02, 0x0c,
            0x01,
            0x04, 0x68, 0x6f, 0x73, 0x74,
            0x03, 0x69, 0x6e, 0x63,
            0x00, 0x00
        };
        static const uint8_t host_name_bytes[] = {
            (uint8_t)'h', (uint8_t)'o',
            (uint8_t)'s', (uint8_t)'t'
        };
        static const uint8_t inc_name_bytes[] = {
            (uint8_t)'i', (uint8_t)'n', (uint8_t)'c'
        };
        static const turbowasm_value_kind host_params[] = {
            TURBOWASM_VALUE_I32
        };
        static const turbowasm_value_kind host_results[] = {
            TURBOWASM_VALUE_I32
        };
        const turbowasm_host_function_type host_type = {
            host_params, 1u, host_results, 1u
        };
        const turbowasm_name host_name = {
            host_name_bytes, 4u
        };
        const turbowasm_name inc_name = {
            inc_name_bytes, 3u
        };
        turbowasm_module host_module = {0};
        turbowasm_instance host_consumer = {0};
        turbowasm_linker host_linker = {0};
        int host_error = 0;

        argument.kind = TURBOWASM_VALUE_I32;
        argument.as.i32 = 9;
        result = (turbowasm_value){0};
        result_count = 0u;
        trap = TURBOWASM_TRAP_NONE;

        if (turbowasm_module_load_borrowed(
                &host_module,
                host_consumer_bytes,
                sizeof(host_consumer_bytes)) != TURBOWASM_OK)
            host_error = 9;
        else if (turbowasm_linker_init(
                     &host_linker) != TURBOWASM_OK)
            host_error = 10;
        else if (turbowasm_linker_define_host_function(
                     &host_linker,
                     host_name,
                     inc_name,
                     &host_type,
                     host_inc,
                     NULL) != TURBOWASM_OK)
            host_error = 11;
        else if (turbowasm_instance_create_linked(
                     &host_consumer,
                     &host_module,
                     &host_linker) != TURBOWASM_OK)
            host_error = 12;

        turbowasm_linker_destroy(&host_linker);

        if (host_error == 0 &&
            turbowasm_instance_invoke(
                &host_consumer, 0u,
                &argument, 1u,
                &result, 1u,
                &result_count,
                &trap) != TURBOWASM_OK)
            host_error = 13;
        if (host_error == 0 &&
            (trap != TURBOWASM_TRAP_NONE ||
             result_count != 1u ||
             result.kind != TURBOWASM_VALUE_I32 ||
             result.as.i32 != 10))
            host_error = 14;

        turbowasm_instance_destroy(&host_consumer);
        turbowasm_module_destroy(&host_module);

        if (host_error != 0) {
            exit_code = host_error;
            goto done;
        }
    }

done:
    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&consumer);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&consumer_module);
    turbowasm_module_destroy(&provider_module);
    return exit_code;
}
