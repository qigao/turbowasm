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

typedef struct wait_probe {
    uint32_t can_wait_checks;
    uint32_t submitted;
    uint32_t resumed;
    int last_completion;
    uintptr_t operation_token;
} wait_probe;

static turbowasm_name name_span(const char *text, uint32_t size) {
    turbowasm_name name;
    name.bytes = (const uint8_t *)text;
    name.size = size;
    return name;
}

static turbowasm_status host_wait_i32(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
    wait_probe *probe = (wait_probe *)context;
    int completion_status = 0;
    turbowasm_status status;

    assert(probe != NULL);
    assert(call != NULL);
    assert(argument_count == 0u);
    assert(arguments == NULL);
    assert(result_capacity >= 1u);
    assert(results != NULL);
    assert(result_count != NULL);
    assert(trap != NULL);

    ++probe->can_wait_checks;
    if (!turbowasm_host_call_can_wait(call))
        return TURBOWASM_UNSUPPORTED;

    ++probe->submitted;
    status = turbowasm_host_call_wait(
        call, probe->operation_token, &completion_status);
    if (status != TURBOWASM_OK)
        return status;

    ++probe->resumed;
    probe->last_completion = completion_status;

    results[0].kind = TURBOWASM_VALUE_I32;
    results[0].as.i32 = completion_status;
    *result_count = 1u;
    *trap = TURBOWASM_TRAP_NONE;
    return TURBOWASM_OK;
}

static const uint8_t module_bytes[] = {
    WASM_HEADER,

    /* type0: () -> i32 */
    0x01, 0x05,
    0x01, 0x60, 0x00, 0x01, 0x7f,

    /* import host.wait type0 */
    0x02, 0x0d,
    0x01,
    0x04, 0x68, 0x6f, 0x73, 0x74,
    0x04, 0x77, 0x61, 0x69, 0x74,
    0x00, 0x00,

    /* two local wrappers, both type0 */
    0x03, 0x03,
    0x02, 0x00, 0x00,

    0x0a, 0x0e,
    0x02,

    /* function1: call host.wait; i32.const 1; add */
    0x07,
    0x00,
    0x10, 0x00,
    0x41, 0x01,
    0x6a,
    0x0b,

    /* function2: return_call host.wait */
    0x04,
    0x00,
    0x12, 0x00,
    0x0b
};

static void setup(
    turbowasm_module *module,
    turbowasm_instance *instance,
    turbowasm_linker *linker,
    wait_probe *probe) {
    static const turbowasm_value_kind results[] = {
        TURBOWASM_VALUE_I32
    };
    const turbowasm_host_function_type type = {
        NULL, 0u, results, 1u
    };

    assert(turbowasm_module_load_borrowed(
               module, module_bytes, sizeof(module_bytes)) ==
           TURBOWASM_OK);
    assert(turbowasm_linker_init(linker) == TURBOWASM_OK);
    assert(turbowasm_linker_define_host_function(
               linker,
               name_span("host", 4u),
               name_span("wait", 4u),
               &type,
               host_wait_i32,
               probe) == TURBOWASM_OK);
    assert(turbowasm_instance_create_linked(
               instance, module, linker) == TURBOWASM_OK);
}

static void test_nested_host_wait_resumes_without_replay(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_execution execution = {0};
    turbowasm_host_wait wait = {0};
    turbowasm_host_wait stale = {0};
    wait_probe probe = {0};
    const turbowasm_value *result;

    probe.operation_token = (uintptr_t)0x1234u;
    setup(&module, &instance, &linker, &probe);
    turbowasm_linker_destroy(&linker);

    assert(turbowasm_execution_create(
               &execution, &instance, 1u, NULL, 0u) == TURBOWASM_OK);

    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_state_get(&execution) ==
           TURBOWASM_EXECUTION_YIELDED);
    assert(turbowasm_execution_yield_reason_get(&execution) ==
           TURBOWASM_YIELD_HOST_WAIT);
    assert(probe.can_wait_checks == 1u);
    assert(probe.submitted == 1u);
    assert(probe.resumed == 0u);

    assert(turbowasm_execution_pending_host_wait(
               &execution, &wait));
    assert(wait.generation != 0u);
    assert(wait.operation_token == probe.operation_token);

    /*
     * No terminal completion yet: resume must not re-enter the callback or
     * advance the Wasm instruction stream.
     */
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_YIELDED);
    assert(probe.can_wait_checks == 1u);
    assert(probe.submitted == 1u);
    assert(probe.resumed == 0u);

    stale = wait;
    ++stale.generation;
    assert(turbowasm_execution_complete_host_wait(
               &execution, stale, 41) == TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_execution_complete_host_wait(
               &execution, wait, 41) == TURBOWASM_OK);
    assert(turbowasm_execution_complete_host_wait(
               &execution, wait, 41) == TURBOWASM_INVALID_ARGUMENT);

    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_OK);
    assert(probe.can_wait_checks == 1u);
    assert(probe.submitted == 1u);
    assert(probe.resumed == 1u);
    assert(probe.last_completion == 41);
    assert(!turbowasm_execution_pending_host_wait(
        &execution, &wait));

    result = turbowasm_execution_result_at(&execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 42);

    /* Completion after consumption is stale. */
    assert(turbowasm_execution_complete_host_wait(
               &execution, stale, 99) == TURBOWASM_INVALID_ARGUMENT);

    turbowasm_execution_destroy(&execution);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_tail_call_host_wait_retains_callback_frame(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    turbowasm_execution execution = {0};
    turbowasm_host_wait wait = {0};
    wait_probe probe = {0};
    const turbowasm_value *result;

    probe.operation_token = (uintptr_t)0x77u;
    setup(&module, &instance, &linker, &probe);

    assert(turbowasm_execution_create(
               &execution, &instance, 2u, NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_execution_pending_host_wait(
               &execution, &wait));
    assert(wait.operation_token == (uintptr_t)0x77u);

    assert(turbowasm_execution_complete_host_wait(
               &execution, wait, 7) == TURBOWASM_OK);
    assert(turbowasm_execution_resume(
               &execution, NULL) == TURBOWASM_OK);

    result = turbowasm_execution_result_at(&execution, 0u);
    assert(result != NULL);
    assert(result->kind == TURBOWASM_VALUE_I32);
    assert(result->as.i32 == 7);
    assert(probe.submitted == 1u);
    assert(probe.resumed == 1u);

    turbowasm_execution_destroy(&execution);
    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

static void test_one_shot_rejects_before_async_submission(void) {
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_linker linker = {0};
    wait_probe probe = {0};
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    probe.operation_token = (uintptr_t)0x99u;
    setup(&module, &instance, &linker, &probe);

    assert(turbowasm_instance_invoke(
               &instance, 1u, NULL, 0u,
               &result, 1u,
               &result_count, &trap) == TURBOWASM_UNSUPPORTED);
    assert(result_count == 0u);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(probe.can_wait_checks == 1u);
    assert(probe.submitted == 0u);
    assert(probe.resumed == 0u);

    turbowasm_linker_destroy(&linker);
    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
}

int main(void) {
    test_nested_host_wait_resumes_without_replay();
    test_tail_call_host_wait_retains_callback_frame();
    test_one_shot_rejects_before_async_submission();
    return 0;
}
