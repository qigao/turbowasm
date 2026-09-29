#include <turbowasm/turbowasm.h>
#include <turbowasm/wasi.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t wasi_module_bytes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0c, 0x02,
    0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x60, 0x01, 0x7f, 0x01, 0x7f,
    0x02, 0x9b, 0x01, 0x04,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61,
    0x70, 0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76,
    0x69, 0x65, 0x77, 0x31,
    0x0e, 0x61, 0x72, 0x67, 0x73, 0x5f, 0x73, 0x69, 0x7a, 0x65,
    0x73, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x08, 0x61, 0x72, 0x67, 0x73, 0x5f, 0x67, 0x65, 0x74,
    0x00, 0x00,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x11, 0x65, 0x6e, 0x76, 0x69, 0x72, 0x6f, 0x6e, 0x5f, 0x73,
    0x69, 0x7a, 0x65, 0x73, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x0b, 0x65, 0x6e, 0x76, 0x69, 0x72, 0x6f, 0x6e, 0x5f, 0x67,
    0x65, 0x74, 0x00, 0x00,
    0x03, 0x03, 0x02, 0x01, 0x01,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x0a, 0x11, 0x02,
    0x07, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,
    0x07, 0x00, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b
};

static const uint8_t environ_only_module[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x07, 0x01,
    0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x02, 0x2c, 0x01,
    0x16, 0x77, 0x61, 0x73, 0x69, 0x5f, 0x73, 0x6e, 0x61, 0x70,
    0x73, 0x68, 0x6f, 0x74, 0x5f, 0x70, 0x72, 0x65, 0x76, 0x69,
    0x65, 0x77, 0x31,
    0x11, 0x65, 0x6e, 0x76, 0x69, 0x72, 0x6f, 0x6e, 0x5f, 0x73,
    0x69, 0x7a, 0x65, 0x73, 0x5f, 0x67, 0x65, 0x74, 0x00, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01
};

static turbowasm_value i32_value(int32_t value) {
    turbowasm_value out = {0};
    out.kind = TURBOWASM_VALUE_I32;
    out.as.i32 = value;
    return out;
}

static int32_t invoke_i32(
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
    assert(result.kind == TURBOWASM_VALUE_I32);
    return result.as.i32;
}

static int32_t guest_read(
    turbowasm_instance *instance,
    uint32_t reader_index,
    uint32_t address) {
    turbowasm_value argument = i32_value((int32_t)address);
    return invoke_i32(instance, reader_index, &argument, 1u);
}

static void test_args_and_environ(void) {
    char arg0[] = "prog";
    char arg1[] = "x";
    char env0[] = "A=B";
    const char *args[] = {arg0, arg1};
    const char *environment[] = {env0};
    turbowasm_wasi_preview1_config config = {
        true, args, 2u,
        true, environment, 1u
    };
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_value call_args[2] = {{0}};
    int32_t wasi_errno;

    assert(turbowasm_wasi_preview1_init(
               &wasi, &config) == TURBOWASM_OK);

    /* The capability owns copied strings, not caller storage. */
    arg0[0] = 'X';
    arg1[0] = 'Y';
    env0[0] = 'Z';

    assert(turbowasm_module_load_borrowed(
               &module,
               wasi_module_bytes,
               sizeof(wasi_module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);
    turbowasm_linker_destroy(&linker);

    /* args_sizes_get(argc=2, bytes=7) -> memory[0], memory[4]. */
    call_args[0] = i32_value(0);
    call_args[1] = i32_value(4);
    wasi_errno = invoke_i32(&instance, 0u, call_args, 2u);
    assert(wasi_errno == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read(&instance, 4u, 0u) == 2);
    assert(guest_read(&instance, 4u, 4u) == 7);

    /* args_get writes argv pointers at 8/12 and strings from 32. */
    call_args[0] = i32_value(8);
    call_args[1] = i32_value(32);
    wasi_errno = invoke_i32(&instance, 1u, call_args, 2u);
    assert(wasi_errno == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read(&instance, 4u, 8u) == 32);
    assert(guest_read(&instance, 4u, 12u) == 37);
    assert(guest_read(&instance, 5u, 32u) == 'p');
    assert(guest_read(&instance, 5u, 33u) == 'r');
    assert(guest_read(&instance, 5u, 36u) == 0);
    assert(guest_read(&instance, 5u, 37u) == 'x');
    assert(guest_read(&instance, 5u, 38u) == 0);

    /* environ_sizes_get(count=1, bytes=4). */
    call_args[0] = i32_value(16);
    call_args[1] = i32_value(20);
    wasi_errno = invoke_i32(&instance, 2u, call_args, 2u);
    assert(wasi_errno == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read(&instance, 4u, 16u) == 1);
    assert(guest_read(&instance, 4u, 20u) == 4);

    /* environ_get pointer at 24, bytes from 64. */
    call_args[0] = i32_value(24);
    call_args[1] = i32_value(64);
    wasi_errno = invoke_i32(&instance, 3u, call_args, 2u);
    assert(wasi_errno == TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read(&instance, 4u, 24u) == 64);
    assert(guest_read(&instance, 5u, 64u) == 'A');
    assert(guest_read(&instance, 5u, 65u) == '=');
    assert(guest_read(&instance, 5u, 66u) == 'B');
    assert(guest_read(&instance, 5u, 67u) == 0);

    /* Invalid guest output range maps deterministically to WASI FAULT. */
    call_args[0] = i32_value(65535);
    call_args[1] = i32_value(4);
    wasi_errno = invoke_i32(&instance, 0u, call_args, 2u);
    assert(wasi_errno == TURBOWASM_WASI_ERRNO_FAULT);

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&wasi);
}

static void test_zero_lists_and_capability_gating(void) {
    turbowasm_wasi_preview1_config empty_config = {
        true, NULL, 0u,
        true, NULL, 0u
    };
    turbowasm_wasi_preview1_config args_only_config = {
        true, NULL, 0u,
        false, NULL, 0u
    };
    turbowasm_wasi_preview1 empty_wasi = {0};
    turbowasm_wasi_preview1 args_only = {0};
    turbowasm_module module = {0};
    turbowasm_module env_module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_value call_args[2] = {{0}};

    assert(turbowasm_wasi_preview1_init(
               &empty_wasi, &empty_config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &module,
               wasi_module_bytes,
               sizeof(wasi_module_bytes)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &empty_wasi, &linker) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               &instance, &module, &linker) == TURBOWASM_OK);

    call_args[0] = i32_value(0);
    call_args[1] = i32_value(4);
    assert(invoke_i32(&instance, 0u, call_args, 2u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(guest_read(&instance, 4u, 0u) == 0);
    assert(guest_read(&instance, 4u, 4u) == 0);

    call_args[0] = i32_value(65536);
    call_args[1] = i32_value(65536);
    assert(invoke_i32(&instance, 1u, call_args, 2u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);
    assert(invoke_i32(&instance, 3u, call_args, 2u) ==
           TURBOWASM_WASI_ERRNO_SUCCESS);

    turbowasm_instance_destroy(&instance);
    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&module);
    turbowasm_wasi_preview1_destroy(&empty_wasi);

    assert(turbowasm_wasi_preview1_init(
               &args_only, &args_only_config) == TURBOWASM_OK);
    assert(turbowasm_module_load_borrowed(
               &env_module,
               environ_only_module,
               sizeof(environ_only_module)) == TURBOWASM_OK);
    assert(turbowasm_linker_init(&linker) == TURBOWASM_OK);
    assert(turbowasm_wasi_preview1_define(
               &args_only, &linker) == TURBOWASM_OK);

    /* environment functions are absent, not silently granted. */
    assert(turbowasm_instance_create_linked(
               &instance, &env_module, &linker) ==
           TURBOWASM_LINK_ERROR);
    assert(instance.impl == NULL);

    turbowasm_linker_destroy(&linker);
    turbowasm_module_destroy(&env_module);
    turbowasm_wasi_preview1_destroy(&args_only);
}

int main(void) {
    test_args_and_environ();
    test_zero_lists_and_capability_gating();
    return 0;
}
