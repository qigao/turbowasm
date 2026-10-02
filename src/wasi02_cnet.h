#ifndef TURBOWASM_WASI02_CNET_H
#define TURBOWASM_WASI02_CNET_H

#include <turbowasm/wasi02_sockets.h>

#include <cnet/cnet.h>

#include <stddef.h>
#include <stdint.h>

typedef struct turbowasm_wasi02_cnet {
    void *impl;
} turbowasm_wasi02_cnet;

typedef struct turbowasm_wasi02_cnet_config {
    native_io_backend_kind backend;
    uint32_t socket_capacity;
    size_t default_listen_backlog;
} turbowasm_wasi02_cnet_config;

/*
 * Bounded released-Salts CNet provider for the WASI 0.2 TCP control plane.
 *
 * This first S3 slice owns only unbound/bound/listening CNet socket owners.
 * It creates no worker thread and never exposes a native fd/SOCKET/HANDLE.
 * Connected stream/poll ownership is added by later #393 slices.
 */
turbowasm_status turbowasm_wasi02_cnet_init(
    turbowasm_wasi02_cnet *adapter,
    const turbowasm_wasi02_cnet_config *config);

turbowasm_status turbowasm_wasi02_cnet_destroy(
    turbowasm_wasi02_cnet *adapter);

turbowasm_status turbowasm_wasi02_cnet_socket_provider(
    turbowasm_wasi02_cnet *adapter,
    turbowasm_wasi02_socket_provider *out_provider);

#endif /* TURBOWASM_WASI02_CNET_H */
