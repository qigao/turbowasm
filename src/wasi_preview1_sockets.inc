/* Preview1 guest-memory projection and bounded host-wait orchestration.
 * Transport progress belongs to the host; these callbacks never poll OS IO. */
typedef struct tw_p1_subscription {
    uint64_t userdata, deadline, precision, minimum;
    union { uint32_t clock, fd; };
    uint32_t error;
    uint8_t type;
    bool selected, absolute_deadline;
    tw_wasi_fd_lease lease;
    uint64_t right;
    turbowasm_wasi_readiness readiness;
} tw_p1_subscription;

typedef struct tw_p1_wait {
    bool active, completed, worker, inspecting, cancelled;
    uint32_t completion;
    turbowasm_host_call *call;
    turbowasm_host_wait handle;
    tw_p1_subscription *subscriptions;
    size_t count;
} tw_p1_wait;

struct tw_p1_async {
    bool sockets, poll, stopping, progressing;
    size_t capacity, subscriptions, io_bytes, budget, used, active, calls;
    tw_p1_wait *waits;
    tw_wasi_dispatch *dispatch;
    uint64_t interrupt_interval_ns;
    struct tw_p1_binding *bindings;
};

typedef struct tw_p1_binding {
    turbowasm_wasi_preview1_impl *owner;
    turbowasm_host_function_fn function;
    bool cleanup;
} tw_p1_binding;

static uint64_t tw_p1_load64(const uint8_t *p) {
    return (uint64_t)turbowasm_wasi_load_u32(p) |
        ((uint64_t)turbowasm_wasi_load_u32(p+4) << 32);
}
static void tw_p1_store16(uint8_t *p, uint16_t x) { p[0]=(uint8_t)x; p[1]=(uint8_t)(x>>8); }
static uint16_t tw_p1_load16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1]<<8); }

static bool tw_p1_arguments(const turbowasm_value *a, size_t n, size_t expected) {
    if (!a || n != expected) return false;
    for (size_t i=0; i<n; ++i) if (a[i].kind != TURBOWASM_VALUE_I32) return false;
    return true;
}

static bool tw_p1_subscription_ready(turbowasm_wasi_preview1_impl *p, tw_p1_subscription *s) {
    if (s->error) return true;
    if (s->type == TURBOWASM_WASI_EVENT_CLOCK) {
        uint64_t now = 0;
        s->error = p->clock_time(p->clock_context, s->clock, s->precision, &now);
        return s->error || now >= s->deadline;
    }
    s->error = tw_wasi_fd_check(p->filesystem, &s->lease, s->right);
    if (s->error) return true;
    if (s->right == TURBOWASM_WASI_RIGHT_POLL_FD_READWRITE)
        s->error = tw_wasi_fd_ready(p->filesystem, &s->lease, s->type, &s->readiness);
    else if (s->lease.ops.ready)
        s->error = tw_wasi_provider_call(&s->lease,
            &(tw_wasi_provider_request){.operation = TW_PROVIDER_READY, .as.ready = {s->type, &s->readiness}});
    else s->error = TURBOWASM_WASI_ERRNO_NOTSUP;
    if (!s->error && s->minimum && s->readiness.bytes < s->minimum &&
        !(s->readiness.flags & TURBOWASM_WASI_EVENT_HANGUP)) return false;
    return s->error || s->readiness.ready;
}

#include "wasi_preview1_wait.inc"

#define TW_P1_HOST_ARGS void *context, turbowasm_host_call *call, const turbowasm_value *arguments, \
    size_t argument_count, turbowasm_value *results, size_t result_capacity, size_t *result_count, turbowasm_trap *trap
#define TW_P1_RETURN(e) turbowasm_wasi_return_errno(results, result_capacity, result_count, trap, (e))

static turbowasm_status tw_p1_fd_fdstat_get(TW_P1_HOST_ARGS) {
    turbowasm_wasi_preview1_impl *p=context;
    if (!p || !p->async || !tw_p1_arguments(arguments, argument_count, 2)) return TURBOWASM_INVALID_ARGUMENT;
    uint8_t output[24] = {0}; tw_wasi_fd_lease lease={0};
    uint32_t e=turbowasm_wasi_memory_check(call, (uint32_t)arguments[1].as.i32, sizeof(output));
    if (!e) e=tw_wasi_fd_acquire(p->filesystem, (uint32_t)arguments[0].as.i32, 0, &lease);
    if (!e) {
        output[0]=lease.type; tw_p1_store16(output+2, lease.flags);
        turbowasm_wasi_store_u64(output+8, lease.rights_base);
        turbowasm_wasi_store_u64(output+16, lease.rights_inheriting);
        e=turbowasm_wasi_memory_write(call, (uint32_t)arguments[1].as.i32, output, sizeof(output));
    }
    tw_wasi_fd_release(p->filesystem, &lease); return TW_P1_RETURN(e);
}
static turbowasm_status tw_p1_fd_fdstat_set_flags(TW_P1_HOST_ARGS) {
    (void)call;
    turbowasm_wasi_preview1_impl *p=context;
    if (!p || !p->async || !tw_p1_arguments(arguments, argument_count, 2)) return TURBOWASM_INVALID_ARGUMENT;
    uint32_t flags=(uint32_t)arguments[1].as.i32;
    uint32_t e=flags > UINT16_MAX ? TURBOWASM_WASI_ERRNO_INVAL :
        tw_wasi_fd_set_flags(p->filesystem, (uint32_t)arguments[0].as.i32, (uint16_t)flags);
    return TW_P1_RETURN(e);
}
static turbowasm_status tw_p1_fd_fdstat_set_rights(TW_P1_HOST_ARGS) {
    (void)call;
    turbowasm_wasi_preview1_impl *p=context;
    if (!p || !p->async || !arguments || argument_count != 3 || arguments[0].kind != TURBOWASM_VALUE_I32 ||
        arguments[1].kind != TURBOWASM_VALUE_I64 || arguments[2].kind != TURBOWASM_VALUE_I64) return TURBOWASM_INVALID_ARGUMENT;
    return TW_P1_RETURN(tw_wasi_fd_set_rights(p->filesystem, (uint32_t)arguments[0].as.i32,
        (uint64_t)arguments[1].as.i64, (uint64_t)arguments[2].as.i64));
}

static turbowasm_status tw_p1_sock_accept(TW_P1_HOST_ARGS) {
    turbowasm_wasi_preview1_impl *p=context;
    if (!p || !p->async || !tw_p1_arguments(arguments, argument_count, 3)) return TURBOWASM_INVALID_ARGUMENT;
    uint32_t flags=(uint32_t)arguments[1].as.i32, e=0, fd=0;
    turbowasm_status status=TURBOWASM_OK;
    uint8_t output[4]; tw_p1_subscription sub={0}; turbowasm_wasi_fs_descriptor reserved={0};
    sub.type=TURBOWASM_WASI_EVENT_FD_READ; sub.right=TURBOWASM_WASI_RIGHT_SOCK_ACCEPT;
    if (flags & ~TURBOWASM_WASI_FDFLAG_NONBLOCK) e=TURBOWASM_WASI_ERRNO_INVAL;
    if (!e) e=turbowasm_wasi_memory_check(call, (uint32_t)arguments[2].as.i32, sizeof(output));
    if (!e) e=tw_wasi_fd_acquire(p->filesystem, (uint32_t)arguments[0].as.i32, sub.right, &sub.lease);
    if (!e && !sub.lease.ops.accept) e=TURBOWASM_WASI_ERRNO_NOTSOCK;
    if (!e) e=tw_wasi_fd_claim(p->filesystem, &sub.lease, 0);
    if (!e) e=tw_wasi_fd_reserve(p->filesystem, &reserved, &fd);
    while (!e) {
        status=tw_p1_interrupt(p->async, call); if (status != TURBOWASM_OK) break;
        if (tw_p1_stopping(p->async)) { e=TURBOWASM_WASI_ERRNO_INTR; break; }
        e=tw_wasi_fd_check(p->filesystem, &sub.lease, sub.right);
        if (e) break;
        e=turbowasm_wasi_memory_check(call, (uint32_t)arguments[2].as.i32, sizeof(output));
        if (e) break;
        turbowasm_wasi_fs_file child={0};
        e=tw_wasi_provider_call(&sub.lease,
            &(tw_wasi_provider_request){.operation = TW_PROVIDER_ACCEPT, .as.accept = &child});
        if (!e) {
            tw_wasi_fd_publish(p->filesystem, reserved, child, &sub.lease, (uint16_t)flags);
            reserved=(turbowasm_wasi_fs_descriptor){0};
            turbowasm_wasi_store_u32(output, fd);
            e=turbowasm_wasi_memory_write(call, (uint32_t)arguments[2].as.i32, output, sizeof(output)); break;
        }
        if (e != TURBOWASM_WASI_ERRNO_AGAIN || (sub.lease.flags & TURBOWASM_WASI_FDFLAG_NONBLOCK)) break;
        status=tw_p1_park(p, call, &sub, 1, &e); if (status != TURBOWASM_OK) break;
    }
    if (reserved.generation) tw_wasi_fd_abort(p->filesystem, reserved);
    tw_wasi_fd_release(p->filesystem, &sub.lease);
    return status == TURBOWASM_OK ? TW_P1_RETURN(e) : status;
}

/* One shared projection for sock_recv/send and fd_read/write on a socket.
 * Copies iovec addresses and stages bytes before effects; no guest pointer is
 * held by a transport. WAITALL consumes into this bounded frame until complete.
 */
static turbowasm_status tw_p1_socket_io(turbowasm_wasi_preview1_impl *p, turbowasm_host_call *call,
    const turbowasm_value *arguments, bool receive, bool socket_import,
    turbowasm_value *results, size_t result_capacity, size_t *result_count, turbowasm_trap *trap) {
    uint32_t e=0, count=(uint32_t)arguments[2].as.i32;
    uint32_t result_address=(uint32_t)arguments[socket_import ? 4 : 3].as.i32;
    uint32_t flags=socket_import ? (uint32_t)arguments[3].as.i32 : 0;
    uint32_t flags_address=receive && socket_import ? (uint32_t)arguments[5].as.i32 : 0;
    uint8_t output[4], flag_output[2], table[TURBOWASM_WASI_IOV_MAX * 8u];
    uint32_t addresses[TURBOWASM_WASI_IOV_MAX]={0}, lengths[TURBOWASM_WASI_IOV_MAX]={0};
    uint64_t capacity=0; uint32_t transferred=0; uint16_t returned_flags=0;
    uint8_t *staging=NULL; size_t allocation=0;
    tw_p1_subscription sub={0}; sub.type=receive ? TURBOWASM_WASI_EVENT_FD_READ : TURBOWASM_WASI_EVENT_FD_WRITE;
    sub.right=receive ? TURBOWASM_WASI_RIGHT_FD_READ : TURBOWASM_WASI_RIGHT_FD_WRITE;
    turbowasm_status status=TURBOWASM_OK;
    if ((receive && (flags & ~(TURBOWASM_WASI_RECV_PEEK|TURBOWASM_WASI_RECV_WAITALL))) || (!receive && flags))
        e=TURBOWASM_WASI_ERRNO_INVAL;
    if (!e) e=turbowasm_wasi_memory_check(call, result_address, sizeof(output));
    if (!e && receive && socket_import) e=turbowasm_wasi_memory_check(call, flags_address, sizeof(flag_output));
    if (!e && count > TURBOWASM_WASI_IOV_MAX) e=TURBOWASM_WASI_ERRNO_INVAL;
    if (!e) e=turbowasm_wasi_memory_read(call, (uint32_t)arguments[1].as.i32, table, (size_t)count*8);
    for (uint32_t i=0; i<count && !e; ++i) {
        addresses[i]=turbowasm_wasi_load_u32(table+(size_t)i*8);
        lengths[i]=turbowasm_wasi_load_u32(table+(size_t)i*8+4);
        e=turbowasm_wasi_memory_check(call, addresses[i], lengths[i]);
        capacity+=lengths[i]; /* At most 64 uint32_t lengths. */
    }
    if (!e && capacity > p->async->io_bytes) e=TURBOWASM_WASI_ERRNO_MSGSIZE;
    if (!e) e=tw_wasi_fd_acquire(p->filesystem, (uint32_t)arguments[0].as.i32, sub.right, &sub.lease);
    if (!e && (!sub.lease.ops.recv || !sub.lease.ops.send)) e=TURBOWASM_WASI_ERRNO_NOTSOCK;
    if (!e && receive && (flags & 3u) == 3u &&
        sub.lease.type == TURBOWASM_WASI_FILETYPE_SOCKET_STREAM &&
        !(sub.lease.flags & TURBOWASM_WASI_FDFLAG_NONBLOCK)) sub.minimum=capacity;
    if (!e) e=tw_wasi_fd_claim(p->filesystem, &sub.lease, receive ? 1 : 2);
    if (!e) { allocation=(size_t)capacity; staging=tw_p1_allocate(p, allocation); if (!staging) e=TURBOWASM_WASI_ERRNO_NOMEM; }
    if (!e && !receive) {
        size_t pos=0;
        for (uint32_t i=0; i<count && !e; ++i) {
            e=turbowasm_wasi_memory_read(call, addresses[i], staging+pos, lengths[i]); pos+=lengths[i];
        }
    }
    while (!e) {
        status=tw_p1_interrupt(p->async, call); if (status != TURBOWASM_OK) break;
        if (tw_p1_stopping(p->async)) { e=TURBOWASM_WASI_ERRNO_INTR; break; }
        e=tw_wasi_fd_check(p->filesystem, &sub.lease, sub.right); if (e) break;
        uint32_t n=0;
        if (receive) {
            turbowasm_wasi_buffer b={staging+transferred, (size_t)capacity-transferred};
            e=tw_wasi_provider_call(&sub.lease, &(tw_wasi_provider_request){.operation = TW_PROVIDER_RECV,
                .as.recv = {&b, 1, (uint16_t)flags,
                    (sub.lease.flags & TURBOWASM_WASI_FDFLAG_NONBLOCK) != 0, &n, &returned_flags}});
        } else {
            turbowasm_wasi_const_buffer b={staging, (size_t)capacity};
            e=tw_wasi_provider_call(&sub.lease,
                &(tw_wasi_provider_request){.operation = TW_PROVIDER_SEND, .as.write = {&b, 1, &n}});
        }
        if (!e) {
            if (n > capacity-transferred) { e=TURBOWASM_WASI_ERRNO_IO; break; }
            transferred+=n;
            if (!receive || !n || transferred == capacity || !(flags & TURBOWASM_WASI_RECV_WAITALL) ||
                (flags & TURBOWASM_WASI_RECV_PEEK) || sub.lease.type == TURBOWASM_WASI_FILETYPE_SOCKET_DGRAM ||
                (sub.lease.flags & TURBOWASM_WASI_FDFLAG_NONBLOCK)) break;
            continue;
        }
        if (e != TURBOWASM_WASI_ERRNO_AGAIN || (sub.lease.flags & TURBOWASM_WASI_FDFLAG_NONBLOCK)) break;
        status=tw_p1_park(p, call, &sub, 1, &e); if (status != TURBOWASM_OK) break;
    }
    /* A terminal error after consuming a prefix is reported on the next read.
     * Cancellation/closed capability never publishes partially consumed data. */
    if (transferred && e && e != TURBOWASM_WASI_ERRNO_INTR && e != TURBOWASM_WASI_ERRNO_BADF &&
        e != TURBOWASM_WASI_ERRNO_NOTCAPABLE && receive) e=0;
    if (status == TURBOWASM_OK && !e) status=tw_p1_interrupt(p->async, call);
    if (status == TURBOWASM_OK && !e) {
        e=turbowasm_wasi_memory_check(call, result_address, sizeof(output));
        if (!e && receive && socket_import) e=turbowasm_wasi_memory_check(call, flags_address, sizeof(flag_output));
        if (!e && receive) {
            for (uint32_t i=0; i<count && !e; ++i)
                e=turbowasm_wasi_memory_check(call, addresses[i], lengths[i]);
            size_t pos=0;
            for (uint32_t i=0; i<count && !e && pos<transferred; ++i) {
                size_t n=lengths[i]; if (n > transferred-pos) n=transferred-pos;
                e=turbowasm_wasi_memory_write(call, addresses[i], staging+pos, n);
                pos+=n;
            }
        }
        if (!e) {
            turbowasm_wasi_store_u32(output, transferred);
            e=turbowasm_wasi_memory_write(call, result_address, output, sizeof(output));
            if (!e && receive && socket_import) {
                tw_p1_store16(flag_output, returned_flags);
                e=turbowasm_wasi_memory_write(call, flags_address, flag_output, sizeof(flag_output));
            }
        }
    }
    tw_p1_free(p, staging, allocation); tw_wasi_fd_release(p->filesystem, &sub.lease);
    return status == TURBOWASM_OK ? TW_P1_RETURN(e) : status;
}

static turbowasm_status tw_p1_sock_recv(TW_P1_HOST_ARGS) {
    turbowasm_wasi_preview1_impl *p=context;
    if (!p || !p->async || !tw_p1_arguments(arguments, argument_count, 6)) return TURBOWASM_INVALID_ARGUMENT;
    return tw_p1_socket_io(p, call, arguments, true, true, results, result_capacity, result_count, trap);
}
static turbowasm_status tw_p1_sock_send(TW_P1_HOST_ARGS) {
    turbowasm_wasi_preview1_impl *p=context;
    if (!p || !p->async || !tw_p1_arguments(arguments, argument_count, 5)) return TURBOWASM_INVALID_ARGUMENT;
    return tw_p1_socket_io(p, call, arguments, false, true, results, result_capacity, result_count, trap);
}
static turbowasm_status tw_p1_sock_shutdown(TW_P1_HOST_ARGS) {
    (void)call;
    turbowasm_wasi_preview1_impl *p=context;
    if (!p || !p->async || !tw_p1_arguments(arguments, argument_count, 2)) return TURBOWASM_INVALID_ARGUMENT;
    uint32_t how=(uint32_t)arguments[1].as.i32, e=0; tw_wasi_fd_lease lease={0};
    if (!how || (how & ~3u)) e=TURBOWASM_WASI_ERRNO_INVAL;
    if (!e) e=tw_wasi_fd_acquire(p->filesystem, (uint32_t)arguments[0].as.i32, TURBOWASM_WASI_RIGHT_SOCK_SHUTDOWN, &lease);
    if (!e) e=lease.ops.shutdown ? tw_wasi_provider_call(&lease,
        &(tw_wasi_provider_request){.operation = TW_PROVIDER_SHUTDOWN, .as.shutdown = (uint8_t)how}) : TURBOWASM_WASI_ERRNO_NOTSOCK;
    tw_wasi_fd_release(p->filesystem, &lease); return TW_P1_RETURN(e);
}

static turbowasm_status tw_p1_poll_oneoff(TW_P1_HOST_ARGS) {
    turbowasm_wasi_preview1_impl *p=context;
    if (!p || !p->async || !tw_p1_arguments(arguments, argument_count, 4)) return TURBOWASM_INVALID_ARGUMENT;
    uint32_t count=(uint32_t)arguments[2].as.i32, e=0, events=0;
    size_t allocation=0; tw_p1_subscription *subs=NULL;
    uint8_t input[48], output[32], nevents[4];
    turbowasm_status status=TURBOWASM_OK;
    if (!count || count > p->async->subscriptions) e=TURBOWASM_WASI_ERRNO_INVAL;
    if (!e) e=turbowasm_wasi_memory_check(call, (uint32_t)arguments[0].as.i32, (uint64_t)count*sizeof(input));
    if (!e) e=turbowasm_wasi_memory_check(call, (uint32_t)arguments[1].as.i32, (uint64_t)count*sizeof(output));
    if (!e) e=turbowasm_wasi_memory_check(call, (uint32_t)arguments[3].as.i32, sizeof(nevents));
    if (!e) { allocation=(size_t)count*sizeof(*subs); subs=tw_p1_allocate(p, allocation); if (!subs) e=TURBOWASM_WASI_ERRNO_NOMEM; }
    /* Decode the whole vector before callbacks can mutate/grow guest memory.
     * The owned subscription records also survive suspension. */
    for (uint32_t i=0; i<count && !e; ++i) {
        e=turbowasm_wasi_memory_read(call, (uint64_t)(uint32_t)arguments[0].as.i32+(uint64_t)i*sizeof(input), input, sizeof(input));
        if (e) break;
        tw_p1_subscription *s=&subs[i];
        s->userdata=tw_p1_load64(input); s->type=input[8];
        if (s->type > TURBOWASM_WASI_EVENT_FD_WRITE) e=TURBOWASM_WASI_ERRNO_INVAL;
        if (s->type == TURBOWASM_WASI_EVENT_CLOCK) {
            uint16_t flags=tw_p1_load16(input+40);
            if (flags & ~1u) e=TURBOWASM_WASI_ERRNO_INVAL;
            s->absolute_deadline=(flags & 1u) != 0;
            s->clock=turbowasm_wasi_load_u32(input+16);
            s->deadline=tw_p1_load64(input+24); s->precision=tw_p1_load64(input+32);
        } else s->fd=turbowasm_wasi_load_u32(input+16);
    }
    for (uint32_t i=0; i<count && !e; ++i) {
        tw_p1_subscription *s=&subs[i];
        if (s->type == TURBOWASM_WASI_EVENT_CLOCK) {
            if (!p->allow_clock) s->error=TURBOWASM_WASI_ERRNO_NOTCAPABLE;
            else if (s->clock > TURBOWASM_WASI_CLOCKID_MONOTONIC) s->error=TURBOWASM_WASI_ERRNO_INVAL;
            else if (!s->absolute_deadline) {
                uint64_t now=0; s->error=p->clock_time(p->clock_context, s->clock, s->precision, &now);
                if (!s->error) {
                    if (UINT64_MAX-now < s->deadline) s->error=TURBOWASM_WASI_ERRNO_INVAL;
                    else s->deadline+=now;
                }
            }
        } else {
            s->right=TURBOWASM_WASI_RIGHT_POLL_FD_READWRITE;
            s->error=tw_wasi_fd_acquire(p->filesystem, s->fd, s->right, &s->lease);
        }
    }
    while (!e) {
        status=tw_p1_interrupt(p->async, call); if (status != TURBOWASM_OK) break;
        if (tw_p1_stopping(p->async)) { e=TURBOWASM_WASI_ERRNO_INTR; break; }
        events=0;
        for (uint32_t i=0; i<count; ++i) if ((subs[i].selected=tw_p1_subscription_ready(p, &subs[i]))) ++events;
        if (events) break;
        status=tw_p1_park(p, call, subs, count, &e); if (status != TURBOWASM_OK) break;
    }
    if (!e && status == TURBOWASM_OK) status=tw_p1_interrupt(p->async, call);
    if (!e && status == TURBOWASM_OK) {
        e=turbowasm_wasi_memory_check(call, (uint32_t)arguments[1].as.i32, (uint64_t)count*sizeof(output));
        if (!e) e=turbowasm_wasi_memory_check(call, (uint32_t)arguments[3].as.i32, sizeof(nevents));
        events=0;
        for (uint32_t i=0; i<count && !e; ++i) {
            tw_p1_subscription *s=&subs[i];
            /* Use the readiness snapshot taken together above. */
            if (!s->selected) continue;
            uint8_t *out=output; memset(out, 0, sizeof(output));
            turbowasm_wasi_store_u64(out, s->userdata); tw_p1_store16(out+8, (uint16_t)s->error); out[10]=s->type;
            if (!s->error && s->type != TURBOWASM_WASI_EVENT_CLOCK) {
                turbowasm_wasi_store_u64(out+16, s->readiness.bytes); tw_p1_store16(out+24, s->readiness.flags);
            }
            e=turbowasm_wasi_memory_write(call, (uint64_t)(uint32_t)arguments[1].as.i32+(uint64_t)events*sizeof(output), output, sizeof(output));
            ++events;
        }
        if (!e) {
            turbowasm_wasi_store_u32(nevents, events);
            e=turbowasm_wasi_memory_write(call, (uint32_t)arguments[3].as.i32, nevents, sizeof(nevents));
        }
    }
    if (subs) for (uint32_t i=0; i<count; ++i) tw_wasi_fd_release(p->filesystem, &subs[i].lease);
    tw_p1_free(p, subs, allocation); return status == TURBOWASM_OK ? TW_P1_RETURN(e) : status;
}

#undef TW_P1_RETURN
#undef TW_P1_HOST_ARGS
