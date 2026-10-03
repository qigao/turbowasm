#include "wasi02_cnet_poll_native_io.h"

static turbowasm_status register_dynamic_source(
    void *registry_context,
    void *source_context,
    turbowasm_value source_rep,
    turbowasm_wasi02_cnet_poll_ready_fn ready_fn,
    turbowasm_wasi02_cnet_poll_prepare_fn prepare_fn,
    turbowasm_wasi02_cnet_poll_drop_fn drop_fn,
    turbowasm_value *out_pollable_rep) {
    return turbowasm_wasi02_native_io_poll_register_dynamic(
        (turbowasm_wasi02_native_io_poll *)registry_context,
        source_context,
        source_rep,
        ready_fn,
        prepare_fn,
        drop_fn,
        out_pollable_rep);
}

turbowasm_status turbowasm_wasi02_cnet_attach_native_io_poll(
    turbowasm_wasi02_cnet *cnet,
    turbowasm_wasi02_native_io_poll *poll) {
    if (poll == NULL || poll->impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    return turbowasm_wasi02_cnet_attach_poll_registry(
        cnet,
        poll,
        register_dynamic_source);
}
