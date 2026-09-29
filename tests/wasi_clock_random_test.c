#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t module_bytes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x18, 0x04,
    0x60, 0x03, 0x7f, 0x7e, 0x7f, 0x01, 0x7f,
    0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x01, 0x7f, 0x01, 0x7e,
    0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x02, 0x4d, 0x02,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x0e, 0x63, 0x6c, 0x6f, 0x63, 0x6b, 0x5f, 0x74, 0x69, 0x6d,
    0x65, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x0a, 0x72, 0x61, 0x6e, 0x64, 0x6f, 0x6d, 0x5f, 0x67, 0x65,
    0x74, 0x00, 0x01,
    0x03, 0x03, 0x02, 0x02, 0x03,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x0a, 0x11, 0x02,
    0x07, 0x00, 0x20, 0x00, 0x29, 0x03, 0x00, 0x0b,
    0x07, 0x00, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b
};

typedef struct clock_probe {
    uint32_t calls;
    uint32_t last_id;
    uint64_t last_precision;
} clock_probe;

typedef struct random_probe {
    uint32_t calls;
    bool fail;
} random_probe;

static uint32_t clock_time(
    void *context,
    uint32_t clock_id,
    uint64_t precision_ns,
    uint64_t *out_timestamp_ns) {
    clock_probe *probe = (clock_probe *)context;
    assert(probe != NULL);
    assert(out_timestamp_ns != NULL);

    ++probe->calls;
    probe->last_id = clock_id;
    probe->last_precision = precision_ns;

    if (clock_id != TURBOWASM_WASI_CLOCKID_REALTIME &&
        clock_id != TURBOWASM_WASI_CLOCKID_MONOTONIC)
        return TURBOWASM_WASI_ERRNO_INVAL;

    *out_timestamp_ns =
        clock_id == TURBOWASM_WASI_CLOCKID_REALTIME
            ? UINT64_C(123456789)
            : UINT64_C(987654321);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static uint32_t random_fill(
    void *context,
    uint8_t *buffer,
    size_t length) {
    random_probe *probe = (random_probe *)context;
    size_t index;

    assert(probe != NULL);
    ++probe->calls;
    if (probe->fail)
        return TURBOWASM_WASI_ERRNO_IO;

    for (index = 0u; index < length; ++index)
        buffer[index] = (uint8_t)(0xa0u + index);
    return TURBOWASM_WASI_ERRNO_SUCCESS;
}

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
    return out;
}

static turbowasm_value i64_value(int64_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I64;
    out.as.i64 = value;
    return out;
}

static turbowasm_value invoke_value(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_instance_invoke(
               instance, function_index,
               arguments, argument_count,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    return result;
}

static int32_t invoke_errno(
    turbowasm_instance *instance,
    uint32_t function_index,
    const turbowasm_value *arguments,
    size_t argument_count) {
    turbowasm_value result =
        invoke_value(instance, function_index, arguments, argument_count);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static int64_t read_i64(
    turbowasm_instance *instance,
    uint32_t address) {
    turbowasm_value arg = i32_value((int32_t)address);
    turbowasm_value result =
        invoke_value(instance, 2u, &arg, 1u);
    assert(result.kind == TURBOWASM_VALUE_I64);
    return result.as.i64;
}

static int32_t read_u8(
    turbowasm_instance *instance,
    uint32_t address) {
    turbowasm_value arg = i32_value((int32_t)address);
    turbowasm_value result =
        invoke_value(instance, 3u, &arg, 1u);
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static void test_clock_and_random(void) {
    clock_probe clock = {0};
    random_probe random = {0};
    turbowasm_wasi_preview1_config config = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_value clock_args[3] = {{0}};
    turbowasm_value random_args[2] = {{0}};
    uint32_t before;

    config.allow_clock = true;
    config.clock_time = clock_time;
    config.clock_context = &clock;
    config.allow_random = true;
    config.random_fill = random_fill;
    config.random_context = &random;

    assert(turbowasm_wasi_preview1_init(
               &wasi, &config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module, module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    clock_args[0] = i32_value(TURBOWASM_WASI_CLOCKID_MONOTONIC);
    clock_args[1] = i64_value(99);
    clock_args[2] = i32_value(0);
    assert(invoke_errno(
               &instance, 0u, clock_args, 3u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(clock.calls == 1u);
    assert(clock.last_id == TURBOWASM_WASI_CLOCKID_MONOTONIC);
    assert(clock.last_precision == 99u);
    assert((uint64_t)read_i64(&instance, 0u) ==
           UINT64_C(987654321));

    /* Unsupported provider clock id returns its errno. */
    clock_args[0] = i32_value(9);
    clock_args[2] = i32_value(8);
    assert(invoke_errno(
               &instance, 0u, clock_args, 3u) ==
           TURBOWASM_WASI_ERRNO_INVAL);
    assert(clock.calls == 2u);

    /* OOB timestamp is rejected before provider invocation. */
    before = clock.calls;
    clock_args[0] = i32_value(TURBOWASM_WASI_CLOCKID_REALTIME);
    clock_args[2] = i32_value(65532);
    assert(invoke_errno(
               &instance, 0u, clock_args, 3u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(clock.calls == before);

    random_args[0] = i32_value(32);
    random_args[1] = i32_value(4);
    assert(invoke_errno(
               &instance, 1u, random_args, 2u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(random.calls == 1u);
    assert(read_u8(&instance, 32u) == 0xa0);
    assert(read_u8(&instance, 33u) == 0xa1);
    assert(read_u8(&instance, 35u) == 0xa3);

    /* OOB is detected before calling the entropy provider. */
    before = random.calls;
    random_args[0] = i32_value(65535);
    random_args[1] = i32_value(2);
    assert(invoke_errno(
               &instance, 1u, random_args, 2u) ==
           TURBOWASM_WASI_ERRNO_FAULT);
    assert(random.calls == before);

    random.fail = true;
    random_args[0] = i32_value(64);
    random_args[1] = i32_value(1);
    assert(invoke_errno(
               &instance, 1u, random_args, 2u) ==
           TURBOWASM_WASI_ERRNO_IO);
    assert(random.calls == before + 1u);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
}

static void test_capability_validation_and_gating(void) {
    turbowasm_wasi_preview1_config invalid = {0};
    turbowasm_wasi_preview1_config disabled = {0};
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};

    invalid.allow_clock = true;
    assert(turbowasm_wasi_preview1_init(
               &wasi, &invalid) == TURBOWASM_INVALID_ARGUMENT);
    assert(wasi.impl == NULL);

    assert(turbowasm_wasi_preview1_init(
               &wasi, &disabled) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module, module_bytes,
               sizeof(module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);

    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) ==
           TURBOWASM_LINK_ERROR);
    assert(instance.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
}

int main(void) {
    test_clock_and_random();
    test_capability_validation_and_gating();
    return 0;
}
