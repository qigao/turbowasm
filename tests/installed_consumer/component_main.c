#include <turbowasm/component.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

static const uint8_t executable_component[] = {
    0x00,0x61,0x73,0x6d,0x0d,0x00,0x01,0x00,
    0x01,0x27,
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x05,0x01,0x60,0x00,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x07,0x0a,0x01,0x06,'a','n','s','w','e','r',0x00,0x00,
      0x0a,0x06,0x01,0x04,0x00,0x41,0x2a,0x0b,
    0x02,0x04,0x01,0x00,0x00,0x00,
    0x06,0x0c,0x01,
      0x00,0x00,0x01,0x00,
      0x06,'a','n','s','w','e','r',
    0x07,0x05,0x01,0x40,0x00,0x00,0x79,
    0x08,0x06,0x01,0x00,0x00,0x00,0x00,0x00,
    0x0b,0x0c,0x01,
      0x00,0x06,'a','n','s','w','e','r',
      0x01,0x00,0x00
};

int main(void) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_component_call call = {0};
    turbowasm_component_host_value result = {0};
    turbowasm_component_host_value call_result = {0};
    turbowasm_name name = {
        (const uint8_t *)"answer", 6u
    };
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;

    assert(turbowasm_component_load_borrowed(
               &component,
               executable_component,
               sizeof(executable_component)) == TURBOWASM_OK);
    assert(turbowasm_component_instance_create(
               &instance, &component) == TURBOWASM_OK);

    /*
     * Instance retains the private decoded state. Destroying the public
     * component handle must not invalidate the already-created instance.
     */
    turbowasm_component_destroy(&component);
    assert(component.impl == NULL);

    assert(turbowasm_component_instance_invoke(
               &instance,
               name,
               NULL, 0u,
               &result, 1u,
               &result_count,
               &trap) == TURBOWASM_OK);
    assert(trap == TURBOWASM_TRAP_NONE);
    assert(result_count == 1u);
    assert(result.kind == TURBOWASM_COMPONENT_HOST_U32);
    assert(result.as.u32 == 42u);

    assert(turbowasm_component_call_create(
               &call,
               &instance,
               name,
               NULL, 0u) == TURBOWASM_OK);
    assert(turbowasm_component_call_state_get(
               &call) == TURBOWASM_EXECUTION_READY);
    assert(turbowasm_component_call_resume(
               &call, NULL) == TURBOWASM_OK);
    assert(turbowasm_component_call_state_get(
               &call) == TURBOWASM_EXECUTION_COMPLETED);
    assert(turbowasm_component_call_terminal_status(
               &call) == TURBOWASM_OK);
    assert(turbowasm_component_call_result_count(
               &call) == 1u);
    assert(turbowasm_component_call_take_result(
               &call, &call_result) == TURBOWASM_OK);
    assert(call_result.kind == TURBOWASM_COMPONENT_HOST_U32);
    assert(call_result.as.u32 == 42u);

    turbowasm_component_host_value_destroy(&call_result);
    turbowasm_component_call_destroy(&call);
    turbowasm_component_host_value_destroy(&result);
    turbowasm_component_instance_destroy(&instance);
    assert(instance.impl == NULL);
    return 0;
}
