#ifndef TURBOWASM_WASI_FS_PRIVATE_H
#define TURBOWASM_WASI_FS_PRIVATE_H
#include <turbowasm/wasi_sockets.h>

typedef struct tw_wasi_dispatch tw_wasi_dispatch;

typedef struct tw_wasi_fd_lease {
    turbowasm_wasi_fs_descriptor descriptor;
    turbowasm_wasi_fs_file file;
    turbowasm_wasi_descriptor_ops ops;
    uint64_t rights_base, rights_inheriting;
    uint16_t flags;
    uint8_t type;
    bool held, claimed;
    uint8_t direction;
    tw_wasi_dispatch *dispatch;
} tw_wasi_fd_lease;

/* Lifecycle-exclusive attachment by threaded Preview1; sockets are bound only
 * after attachment. Detachment requires all socket identities/leases drained. */
turbowasm_status tw_wasi_fs_attach_dispatch(turbowasm_wasi_fs *, tw_wasi_dispatch *);
turbowasm_status tw_wasi_fs_detach_dispatch(turbowasm_wasi_fs *, tw_wasi_dispatch *);

/* A successful preopen pin owns one synchronous table admission. The borrowed
 * guest_path remains valid through unpin; no provider callback is invoked. */
uint32_t tw_wasi_fd_preopen_pin(turbowasm_wasi_fs *, uint32_t,
    turbowasm_wasi_fs_descriptor_info *);
void tw_wasi_fd_preopen_unpin(turbowasm_wasi_fs *, turbowasm_wasi_fs_descriptor);

uint32_t tw_wasi_fd_acquire(turbowasm_wasi_fs *, uint32_t, uint64_t, tw_wasi_fd_lease *);
bool tw_wasi_fd_is_socket(turbowasm_wasi_fs *, uint32_t);
void tw_wasi_fd_release(turbowasm_wasi_fs *, tw_wasi_fd_lease *);
uint32_t tw_wasi_fd_check(turbowasm_wasi_fs *, const tw_wasi_fd_lease *, uint64_t);
uint32_t tw_wasi_fd_claim(turbowasm_wasi_fs *, tw_wasi_fd_lease *, uint8_t);
uint32_t tw_wasi_fd_set_flags(turbowasm_wasi_fs *, uint32_t, uint16_t);
uint32_t tw_wasi_fd_set_rights(turbowasm_wasi_fs *, uint32_t, uint64_t, uint64_t);
uint32_t tw_wasi_fd_reserve(turbowasm_wasi_fs *, turbowasm_wasi_fs_descriptor *, uint32_t *);
void tw_wasi_fd_abort(turbowasm_wasi_fs *, turbowasm_wasi_fs_descriptor);
void tw_wasi_fd_publish(turbowasm_wasi_fs *, turbowasm_wasi_fs_descriptor,
    turbowasm_wasi_fs_file, const tw_wasi_fd_lease *, uint16_t);
uint32_t tw_wasi_fd_ready(turbowasm_wasi_fs *, const tw_wasi_fd_lease *, uint8_t,
    turbowasm_wasi_readiness *);
#endif
