#ifndef TURBOWASM_WASI_SOCKETS_H
#define TURBOWASM_WASI_SOCKETS_H

#include <turbowasm/wasi_fs.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TURBOWASM_WASI_FDFLAG_NONBLOCK = 4,
    TURBOWASM_WASI_RECV_PEEK = 1,
    TURBOWASM_WASI_RECV_WAITALL = 2,
    TURBOWASM_WASI_RECV_DATA_TRUNCATED = 1,
    TURBOWASM_WASI_SHUTDOWN_RD = 1,
    TURBOWASM_WASI_SHUTDOWN_WR = 2,
    TURBOWASM_WASI_EVENT_CLOCK = 0,
    TURBOWASM_WASI_EVENT_FD_READ = 1,
    TURBOWASM_WASI_EVENT_FD_WRITE = 2,
    TURBOWASM_WASI_EVENT_HANGUP = 1
};

typedef struct turbowasm_wasi_readiness {
    bool ready;
    uint16_t flags;
    uint64_t bytes;
} turbowasm_wasi_readiness;

/* Versioned, copied per descriptor. All operations are nonblocking and run on
 * the same host progress thread. AGAIN requests a readiness wait. A successful
 * accept transfers one identity to the caller. recv must preserve data on PEEK,
 * and must not return a partial blocking PEEK|WAITALL (unless EOF/error).
 * retain/release keep an identity valid after close until every parked call has
 * unwound; release is infallible. Context must outlive all identities/leases.
 * buffers are borrowed only for the callback; no provider retains guest memory.
 */
typedef struct turbowasm_wasi_descriptor_ops {
    size_t size;
    uint32_t api_version;
    turbowasm_wasi_fs_provider file;
    uint32_t (*retain)(void *, turbowasm_wasi_fs_file);
    void (*release)(void *, turbowasm_wasi_fs_file);
    uint32_t (*ready)(void *, turbowasm_wasi_fs_file, uint8_t,
        turbowasm_wasi_readiness *);
    uint32_t (*accept)(void *, turbowasm_wasi_fs_file,
        turbowasm_wasi_fs_file *);
    uint32_t (*recv)(void *, turbowasm_wasi_fs_file,
        const turbowasm_wasi_buffer *, size_t, uint16_t, bool,
        uint32_t *, uint16_t *);
    uint32_t (*send)(void *, turbowasm_wasi_fs_file,
        const turbowasm_wasi_const_buffer *, size_t, uint32_t *);
    uint32_t (*shutdown)(void *, turbowasm_wasi_fs_file, uint8_t);
    /* End a claimed accept/read/write operation (directions 0/1/2), including
     * cancellation. Optional; releases provider-specific operation state. */
    void (*finish)(void *, turbowasm_wasi_fs_file, uint8_t);
} turbowasm_wasi_descriptor_ops;

/* Move on success only; failure preserves *file. File type is STREAM or DGRAM.
 * Flags support NONBLOCK only. Listener child rights are rights_inheriting.
 * The table owns the identity after success and closes it exactly once.
 */
turbowasm_status turbowasm_wasi_fs_bind_socket_move(
    turbowasm_wasi_fs *, uint32_t guest_fd,
    const turbowasm_wasi_descriptor_ops *, turbowasm_wasi_fs_file *file,
    uint8_t file_type, uint16_t flags, uint64_t rights_base,
    uint64_t rights_inheriting, turbowasm_wasi_fs_descriptor *out_descriptor);

typedef struct turbowasm_wasi_preview1_config_v2 {
    size_t size;
    uint32_t api_version;
    turbowasm_wasi_preview1_config base;
    bool allow_sockets;
    bool allow_poll;
    size_t wait_capacity;
    size_t subscription_capacity;
    size_t io_bytes;
    size_t pending_bytes;
} turbowasm_wasi_preview1_config_v2;

/* Socket/poll imports use protected copies for shared and unshared memory.
 * Iovec addresses and poll arguments are saved before transport/clock effects;
 * no guest pointer crosses a provider callback or wait. Transfer payloads use
 * io_bytes (MSGSIZE on excess) and pending_bytes (NOMEM on exhausted storage).
 * Receive copies only the returned prefix; errors before successful publication
 * preserve output. Result buffers must be synchronized by the guest: separate
 * protected writes do not make a multi-buffer result atomic.
 * Socket/poll calls and progress/lifecycle functions still require the same
 * host progress thread, even when instances import shared memory. For example,
 * execute/resume a socket guest and advance its providers on that owner thread;
 * shared-memory support alone does not admit calls from guest worker threads.
 */
void turbowasm_wasi_preview1_config_v2_init(turbowasm_wasi_preview1_config_v2 *);
turbowasm_status turbowasm_wasi_preview1_init_v2(turbowasm_wasi_preview1 *,
    const turbowasm_wasi_preview1_config_v2 *);

typedef struct turbowasm_wasi_preview1_threaded_config {
    size_t request_capacity;
    uint64_t interrupt_interval_ns;
    void (*wake_owner)(void *context);
    void *wake_context;
} turbowasm_wasi_preview1_threaded_config;

/* Opt-in worker calls with socket providers dispatched to the initializing
 * native thread. Initialize with an empty WASI handle and a quiescent FS table
 * containing no sockets, leases, reservations or dispatcher; ordinary preopens
 * may exist. Bind sockets on that owner after initialization. Configs are copied;
 * callbacks/contexts and the table are borrowed until checked destruction.
 *
 * request_capacity bounds queued/running provider commands. Full admission
 * returns WASI BUSY before effects; infallible finish/release waits for capacity,
 * including after shutdown. Accepted commands wait for owner acknowledgement.
 * interrupt_interval_ns is 1..1,000,000,000 ns and bounds the worker's interruption
 * polling interval, not accepted provider completion time. Runtime's configured
 * interruption callback is queried only on the invoking worker. Existing v2
 * wait/subscription/payload limits still apply. Ordinary files stay concurrent.
 * Clock/random/custom file callbacks must support their admitted concurrency;
 * the owner bridge transfers socket callbacks only.
 *
 * wake_owner must be thread-safe, nonblocking and retain a wake predicate until
 * observed; it must not reenter this adapter or wait for the owner. It runs on
 * guest workers. The owner drives native transports then advance/next_timeout;
 * these functions and shutdown/destroy/define are owner-only and reject reentry.
 * No native thread or event loop is created. An owner call which cannot suspend
 * returns AGAIN. Existing resumable owner calls retain their wait protocol.
 *
 * Shutdown stops ordinary calls and wakes waits with INTR. Continue owner
 * progress while consumers and cleanup drain; never join a worker needing this
 * owner. After consumer quiescence, close sockets, destroy WASI (detaching the
 * dispatcher), then destroy FS/providers. shutdown_poll does not close sockets.
 * Destroy rejects live calls/waits or socket identities/leases without consuming
 * ownership. Lifecycle admission remains exclusively coordinated by the host.
 *
 * Returns INVALID_ARGUMENT for invalid bounds/configuration, occupied handles
 * or table state, OUT_OF_MEMORY on allocation failure, OK on success. Failure
 * preserves both handles. Example config: {64, 10000000, signal_owner, context};
 * run root/children on workers while the initializing thread drives progress.
 */
turbowasm_status turbowasm_wasi_preview1_init_threaded(turbowasm_wasi_preview1 *,
    const turbowasm_wasi_preview1_config_v2 *, const turbowasm_wasi_preview1_threaded_config *);
/* Call after progressing transport providers. Never polls native handles.
 * next_timeout returns a relative nanosecond delay, UINT64_MAX if no timer.
 * shutdown wakes parked calls with INTR; destroy_checked requires them to have
 * unwound. The descriptor table remains caller-owned and must outlive WASI.
 */
turbowasm_status turbowasm_wasi_preview1_advance(turbowasm_wasi_preview1 *);
turbowasm_status turbowasm_wasi_preview1_next_timeout(turbowasm_wasi_preview1 *, uint64_t *);
turbowasm_status turbowasm_wasi_preview1_shutdown_request(turbowasm_wasi_preview1 *);
turbowasm_status turbowasm_wasi_preview1_shutdown_poll(turbowasm_wasi_preview1 *, bool *);
turbowasm_status turbowasm_wasi_preview1_destroy_checked(turbowasm_wasi_preview1 *);

#ifdef __cplusplus
}
#endif
#endif
