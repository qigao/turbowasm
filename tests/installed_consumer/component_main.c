#include <turbowasm/component.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../fixtures/component_host_tasks.h"

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

static void async_behavior(void) {
    turbowasm_component component = {0};
    turbowasm_component_instance instance = {0};
    turbowasm_component_async_task task = {0};
    turbowasm_component_async_endpoint reader = {0}, writer = {0};
    turbowasm_component_async_transfer read = {0}, write = {0};
    turbowasm_component_async_transfer_result received = {0}, sent = {0};
    turbowasm_component_async_options options;
    turbowasm_component_type_token stream, payload;
    turbowasm_component_async_endpoint_type info;
    turbowasm_component_async_endpoint_state endpoint_state;
    turbowasm_component_async_task_state task_state;
    turbowasm_component_async_transfer_state transfer_state;
    turbowasm_component_async_shutdown_state shutdown;
    turbowasm_component_async_wait wait = {0};
    turbowasm_component_host_value input = {0}, output = {0};
    turbowasm_component_host_value number = {.kind = TURBOWASM_COMPONENT_HOST_U32, .as.u32 = 42u};
    turbowasm_name echo = {(const uint8_t *)"echo-stream", 11u};
    size_t count = 99u;
    turbowasm_component_async_options_init(&options);
    assert(options.tasks == 64u && options.handles == 4096u && options.transfers == 64u);
    assert(turbowasm_component_load_borrowed(&component, component_host_tasks_bytes,
        sizeof(component_host_tasks_bytes)) == TURBOWASM_UNSUPPORTED);
    assert(component.impl == NULL);
    assert(turbowasm_component_load_async_borrowed_with_config(&component, component_host_tasks_bytes,
        sizeof(component_host_tasks_bytes), NULL) == TURBOWASM_OK);
    assert(turbowasm_component_instance_create_async_with_options(&instance, &component, &options) == TURBOWASM_OK);
    assert(turbowasm_component_instance_parameter_type(&instance, echo, 0u, &stream) == TURBOWASM_OK);
    assert(turbowasm_component_instance_result_type(&instance, echo, &payload) == TURBOWASM_OK);
    assert(payload.scope == stream.scope && payload.id == stream.id);
    assert(turbowasm_component_instance_endpoint_type_get(&instance, stream, &info) == TURBOWASM_OK);
    assert(!info.future && info.has_payload);
    assert(turbowasm_component_instance_type_child(&instance, stream,
        TURBOWASM_COMPONENT_TYPE_EDGE_PAYLOAD, 0u, &payload) == TURBOWASM_OK);
    assert(turbowasm_component_async_endpoint_pair_create(&reader, &writer, &instance, stream) == TURBOWASM_OK);
    assert(turbowasm_component_async_endpoint_into_value(&reader, &input) == TURBOWASM_OK);
    assert(turbowasm_component_async_task_create(&task, &instance, echo, &input, 1u) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_component_async_task_create_move(&task, &instance, echo, &input, 1u) == TURBOWASM_OK);
    assert((int)input.kind == 0);
    assert(turbowasm_component_async_task_state_get(&task, &task_state) == TURBOWASM_OK && !task_state.terminal);
    assert(!turbowasm_component_async_task_pending_host_wait(&task, &wait));
    assert(turbowasm_component_async_task_complete_host_wait(&task, wait, 0) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_component_async_task_resume(&task, NULL) == TURBOWASM_OK);
    assert(turbowasm_component_async_task_take_result(&task, &output, &count) == TURBOWASM_OK && count == 1u);
    assert(turbowasm_component_async_task_request_cancel(&task) == TURBOWASM_OK);
    assert(turbowasm_component_async_task_destroy(&task) == TURBOWASM_OK);
    assert(turbowasm_component_async_endpoint_from_value(&reader, &output) == TURBOWASM_OK);
    assert(turbowasm_component_async_transfer_write(&write, &writer, &number, 1u) == TURBOWASM_OK);
    assert(turbowasm_component_async_transfer_read(&read, &reader, 1u, 0u) == TURBOWASM_OK);
    assert(turbowasm_component_async_transfer_poll(&read) == TURBOWASM_OK);
    assert(turbowasm_component_async_transfer_poll(&write) == TURBOWASM_OK);
    assert(turbowasm_component_async_transfer_state_get(&read, &transfer_state) == TURBOWASM_OK);
    assert(transfer_state.terminal && transfer_state.progress == 1u);
    assert(turbowasm_component_async_transfer_request_cancel(&write) == TURBOWASM_OK);
    assert(turbowasm_component_async_transfer_take_result(&read, &received) == TURBOWASM_OK);
    assert(turbowasm_component_async_transfer_take_result(&write, &sent) == TURBOWASM_OK);
    assert(received.values.kind == TURBOWASM_COMPONENT_HOST_LIST && received.logical_count == 1u);
    assert(received.values.as.list.items[0].as.u32 == 42u && sent.logical_count == 0u);
    assert(turbowasm_component_async_transfer_destroy(&read) == TURBOWASM_OK);
    assert(turbowasm_component_async_transfer_destroy(&write) == TURBOWASM_OK);
    assert(turbowasm_component_async_endpoint_state_get(&received.endpoint, &endpoint_state) == TURBOWASM_OK);
    assert(endpoint_state.readable && !endpoint_state.future);
    assert(turbowasm_component_instance_request_shutdown(&instance) == TURBOWASM_OK);
    assert(turbowasm_component_instance_poll_shutdown(&instance, NULL) == TURBOWASM_YIELDED);
    assert(turbowasm_component_instance_shutdown_state_get(&instance, &shutdown) == TURBOWASM_OK);
    assert(shutdown.requested && !shutdown.complete);
    assert(!turbowasm_component_instance_shutdown_pending_host_wait(&instance, &wait));
    assert(turbowasm_component_instance_shutdown_complete_host_wait(&instance, wait, 0) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_component_async_transfer_result_destroy(&received) == TURBOWASM_OK);
    assert(turbowasm_component_async_transfer_result_destroy(&sent) == TURBOWASM_OK);
    assert(turbowasm_component_instance_poll_shutdown(&instance, NULL) == TURBOWASM_OK);
    turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
    /* The no-config loader and default-options constructor have the same
     * usable ownership contract, including repeated empty destruction. */
    assert(turbowasm_component_load_async_borrowed(&component, component_host_tasks_bytes,
        sizeof(component_host_tasks_bytes)) == TURBOWASM_OK);
    assert(turbowasm_component_instance_create_async_with_options(&instance, &component, NULL) == TURBOWASM_OK);
    assert(turbowasm_component_async_endpoint_destroy(&reader) == TURBOWASM_OK);
    turbowasm_component_instance_destroy(&instance); turbowasm_component_destroy(&component);
}

int main(void) {
    async_behavior();
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
    turbowasm_component_host_value borrowed = {0};

    assert(turbowasm_component_host_value_borrow(&result, &borrowed) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_component_call_create_move(&call, &instance, name, NULL, 0u) == TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_component_instance_invoke_move(&instance, name, NULL, 0u,
        &result, 1u, &result_count, &trap) == TURBOWASM_INVALID_ARGUMENT);

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
    turbowasm_component_instance_destroy(&instance);
    assert(instance.impl == NULL);
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

    assert(turbowasm_component_host_value_destroy(&call_result) == TURBOWASM_OK);
    turbowasm_component_call_destroy(&call);
    assert(turbowasm_component_host_value_destroy(&result) == TURBOWASM_OK);
    turbowasm_component_instance_destroy(&instance);
    assert(instance.impl == NULL);
    return 0;
}
