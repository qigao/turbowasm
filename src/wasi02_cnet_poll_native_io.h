#ifndef TURBOWASM_WASI02_CNET_POLL_NATIVE_IO_H
#define TURBOWASM_WASI02_CNET_POLL_NATIVE_IO_H

#include "wasi02_cnet.h"
#include "wasi02_poll_native_io.h"

/*
 * Compose one CNet adapter with the existing W4 NativeIO poll namespace.
 * Both adapters must borrow the same NativeIO progress owner.
 */
turbowasm_status turbowasm_wasi02_cnet_attach_native_io_poll(
    turbowasm_wasi02_cnet *cnet,
    turbowasm_wasi02_native_io_poll *poll);

#endif /* TURBOWASM_WASI02_CNET_POLL_NATIVE_IO_H */
