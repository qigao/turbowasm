#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define WASM_HEADER \
    0x00, 0x61, 0x73, 0x6d, \
    0x01, 0x00, 0x00, 0x00

static turbowasm_name name_of(const char *text, uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

typedef struct host_probe {
    uint32_t calls;
} host_probe;

static turbowasm_status host_add(
    void *context,
    turbowasm_instance *caller,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    host_probe *probe = (host_probe *)context;

    assert(probe != NULL);
    assert(caller != NULL);
    assert(turbowasm_instance_module(caller) != NULL);
    assert(argument_count == 2u);
    assert(result_capacity >= 1u);
    assert(arguments[0].kind == TURBOWASM_VALUE_I32);
    assert(arguments[1].kind == TURBOWASM_VALUE_I32);

    ++probe->calls;
    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 =
        arguments[0].as.i32 + arguments[1].as.i32;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status host_sub(
    void *context,
    turbowasm_instance *caller,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    host_probe *probe = (host_probe *)context;

    (void)caller;
    assert(probe != NULL);
    assert(argument_count == 2u);
    assert(result_capacity >= 1u);

    ++probe->calls;
    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 =
        arguments[0].as.i32 - arguments[1].as.i32;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static turbowasm_status host_trap(
    void *context,
    turbowasm_instance *caller,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    (void)context;
    (void)caller;
    (void)arguments;
    (void)results;
    (void)result_capacity;

    assert(argument_count == 0u);
    *result_count = 0u;
    *trap = TURBOWASM_TRAP_UNREACHABLE;
    return TURBOWASM_TRAPPED;
}

static const uint8_t consumer_bytes[] = {
    WASM_HEADER,

    /* type0: (i32,i32)->i32; type1: ()->i32 */
    0x01, 0x0b, 0x02,
    0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x00, 0x01, 0x7f,

    /* import host.add type0 and host.sub type0 */
    0x02, 0x17, 0x02,
    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x03, 0x61, 0x64, 0x64,
    0x00, 0x00,
    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x03, 0x73, 0x75, 0x62,
    0x00, 0x00,

    /* two local wrappers type1 */
    0x03, 0x03, 0x02, 0x01, 0x01,

    0x0a, 0x13, 0x02,

    /* f2: add(7,5) => 12 */
    0x08, 0x00,
    0x41, 0x07,
    0x41, 0x05,
    0x10, 0x00,
    0x0b,

    /* f3: return_call sub(9,4) => 5 */
    0x08, 0x00,
    0x41, 0x09,
    0x41, 0x04,
    0x12, 0x01,
    0x0b
};

static const uint8_t mismatch_bytes[] = {
    WASM_HEADER,
    /* type0: (i64,i32)->i32 */
    0x01, 0x07, 0x01,
    0x60, 0x02, 0x7e, 0x7f, 0x01, 0x7f,
    0x02, 0x0c, 0x01,
    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x03, 0x61, 0x64, 0x64,
    0x00, 0x00
};

static const uint8_t missing_bytes[] = {
    WASM_HEADER,
    /* type0: (i32,i32)->i32 */
    0x01, 0x07, 0x01,
    0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x02, 0x10, 0x01,
    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x07, 0x6d, 0x69, 0x73, 0x73, 0x69, 0x6e, 0x67,
    0x00, 0x00
};

static const uint8_t trap_consumer_bytes[] = {
    WASM_HEADER,
    /* type0: ()->() */
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    /* import host.fail */
    0x02, 0x0d, 0x01,
    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x04, 0x66, 0x61, 0x69, 0x6c,
    0x00, 0x00
};

static void register_arithmetic(
    turbowasm_linker *linker,
    host_probe *probe) {
    static const turbowasm_value_kind params[] = {
        TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32
    };
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type type = {
        params, 2u, results, 1u
    };

    assert(turbowasm_linker_define_host_function(
               linker,
               name_of("host", 4u),
               name_of("add", 3u),
               &type,
               host_add,
               probe) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               linker,
               name_of("host", 4u),
               name_of("sub", 3u),
               &type,
               host_sub,
               probe) == TURBOWASM_OK);
}

static int32_t invoke_i32(
    turbowasm_instance *instance,
    uint32_t function_index) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               NULL, 0u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void test_direct_and_tail_host_calls(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    host_probe probe = {0};

    assert(turbowasm_module_load_borrowed(
               &module, consumer_bytes,
               sizeof(consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    register_arithmetic(&linker, &probe);

    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);

    /* The binding is copied into the consumer; linker storage is no longer needed. */
    turbowasm_linker_destroy(&linker);

    assert(invoke_i32(&instance, 2u) == 12);
    assert(invoke_i32(&instance, 3u) == 5);
    assert(probe.calls == 2u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_signature_and_missing_field_rejected(void) {
    turbowasm_module mismatch = {0};
    turbowasm_module missing = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    host_probe probe = {0};

    assert(turbowasm_module_load_borrowed(
               &mismatch, mismatch_bytes,
               sizeof(mismatch_bytes)) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &missing, missing_bytes,
               sizeof(missing_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    register_arithmetic(&linker, &probe);

    assert(turbowasm_instance_create_linked(
               &instance, &mismatch, &linker) ==
           TURBOWASM_TYPE_MISMATCH);
    assert(instance.impl == NULL);

    assert(turbowasm_instance_create_linked(
               &instance, &missing, &linker) ==
           TURBOWASM_LINK_ERROR);
    assert(instance.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&missing);
    turbowasm_module_destroy(&mismatch);
}

static void test_namespace_collision_rejected(void) {
    static const uint8_t empty_module_bytes[] = {
        WASM_HEADER
    };
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type host_type = {
        NULL, 0u, results, 1u
    };
    turbowasm_module provider_module = {0};
    turbowasm_instance provider = {0};
    turbowasm_linker linker = {0};

    assert(turbowasm_module_load_borrowed(
               &provider_module,
               empty_module_bytes,
               sizeof(empty_module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_instance_create(
               &provider, &provider_module) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);

    assert(turbowasm_linker_define_instance(
               &linker, name_of("host", 4u),
               &provider) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_of("host", 4u),
               name_of("x", 1u),
               &host_type,
               host_add,
               NULL) == TURBOWASM_LINK_ERROR);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&provider);
    turbowasm_module_destroy(&provider_module);
}

static void test_host_trap_propagates(void) {
    const turbowasm_host_function_type type = {
        NULL, 0u, NULL, 0u
    };
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_module_load_borrowed(
               &module,
               trap_consumer_bytes,
               sizeof(trap_consumer_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               &linker,
               name_of("host", 4u),
               name_of("fail", 4u),
               &type,
               host_trap,
               NULL) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);

    assert(turbowasm_instance_invoke(
               &instance, 0u,
               NULL, 0u,
               NULL, 0u,
               &result_count, &trap) == TURBOWASM_TRAPPED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_UNREACHABLE);

    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_direct_and_tail_host_calls();
    test_signature_and_missing_field_rejected();
    test_namespace_collision_rejected();
    test_host_trap_propagates();
    return 0;
}
