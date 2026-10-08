/* Preview1 transport projection; no dependency on the WASI descriptor table. */
extern bool turbowasm_wasi02_io_detach_pair_private(turbowasm_wasi02_io *,
    turbowasm_value, turbowasm_value, void *, turbowasm_value, bool);
extern bool turbowasm_wasi02_io_source_exclusive_private(turbowasm_wasi02_io *, turbowasm_wasi02_io_source);

static uint32_t p1_socket_error(turbowasm_wasi02_socket_error e) {
    switch (e) {
    case TURBOWASM_WASI02_SOCKET_ERROR_NONE: return 0;
    case TURBOWASM_WASI02_SOCKET_ERROR_ACCESS_DENIED: return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    case TURBOWASM_WASI02_SOCKET_ERROR_NOT_SUPPORTED: return TURBOWASM_WASI_ERRNO_NOTSUP;
    case TURBOWASM_WASI02_SOCKET_ERROR_INVALID_ARGUMENT: return TURBOWASM_WASI_ERRNO_INVAL;
    case TURBOWASM_WASI02_SOCKET_ERROR_OUT_OF_MEMORY: return TURBOWASM_WASI_ERRNO_NOMEM;
    case TURBOWASM_WASI02_SOCKET_ERROR_TIMEOUT: return TURBOWASM_WASI_ERRNO_TIMEDOUT;
    case TURBOWASM_WASI02_SOCKET_ERROR_CONCURRENCY_CONFLICT: return TURBOWASM_WASI_ERRNO_AGAIN;
    case TURBOWASM_WASI02_SOCKET_ERROR_WOULD_BLOCK: return TURBOWASM_WASI_ERRNO_AGAIN;
    case TURBOWASM_WASI02_SOCKET_ERROR_INVALID_STATE: return TURBOWASM_WASI_ERRNO_NOTCONN;
    case TURBOWASM_WASI02_SOCKET_ERROR_NEW_SOCKET_LIMIT: return TURBOWASM_WASI_ERRNO_MFILE;
    case TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_REFUSED: return TURBOWASM_WASI_ERRNO_CONNREFUSED;
    case TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_RESET: return TURBOWASM_WASI_ERRNO_CONNRESET;
    case TURBOWASM_WASI02_SOCKET_ERROR_CONNECTION_ABORTED: return TURBOWASM_WASI_ERRNO_CONNABORTED;
    case TURBOWASM_WASI02_SOCKET_ERROR_DATAGRAM_TOO_LARGE: return TURBOWASM_WASI_ERRNO_MSGSIZE;
    default: return TURBOWASM_WASI_ERRNO_IO;
    }
}
static uint32_t p1_native_error(int e) {
    if (e == SALTS_ENOBUFS || e == SALTS_EBUSY) return TURBOWASM_WASI_ERRNO_AGAIN;
    if (e == SALTS_ESHUTDOWN || e == SALTS_EPIPE) return TURBOWASM_WASI_ERRNO_PIPE;
    return p1_socket_error(map_error(e));
}
static tw_cnet_slot *p1_tcp(tw_cnet_impl *p, turbowasm_wasi_fs_file f) {
    tw_cnet_slot *s=slot_lookup(p, net_rep(f.object));
    return s && s->generation == f.generation ? s : NULL;
}
static tw_udp *p1_udp(tw_cnet_impl *p, turbowasm_wasi_fs_file f) {
    return f.generation == 1 ? udp_get(p, net_rep(f.object)) : NULL;
}
static uint32_t p1_retain(void *ctx, turbowasm_wasi_fs_file f) {
    tw_cnet_impl *p=ctx; tw_cnet_slot *s=p1_tcp(p,f); tw_udp *u=p1_udp(p,f);
    if (p->busy) return TURBOWASM_WASI_ERRNO_BUSY;
    if (s && s->p1_live) { if (s->refs == UINT32_MAX) return TURBOWASM_WASI_ERRNO_BUSY; ++s->refs; return 0; }
    if (u && u->p1_live) {
        if (u->refs == UINT32_MAX || u->pair->refs == UINT32_MAX) return TURBOWASM_WASI_ERRNO_BUSY;
        ++u->refs; ++u->pair->refs; return 0;
    }
    return TURBOWASM_WASI_ERRNO_BADF;
}
static void p1_release(void *ctx, turbowasm_wasi_fs_file f) {
    tw_cnet_impl *p=ctx; tw_cnet_slot *s=p1_tcp(p,f); tw_udp *u=p1_udp(p,f);
    if (s) --s->refs;
    else if (u) { --u->refs; --u->pair->refs; }
}
static void p1_finish(void *ctx, turbowasm_wasi_fs_file f, uint8_t direction) {
    tw_cnet_slot *s=p1_tcp(ctx,f);
    if (s && direction == 1) s->p1_peek_target=0;
}
static uint32_t p1_close(void *ctx, turbowasm_wasi_fs_file f) {
    tw_cnet_impl *p=ctx; tw_cnet_slot *s=p1_tcp(p,f); tw_udp *u=p1_udp(p,f);
    if (p->busy) return TURBOWASM_WASI_ERRNO_BUSY;
    if (s && s->p1_live) {
        s->p1_live=false; s->p1_peek_target=0;
        native_input_drop(p, net_rep(s->token)); native_output_drop(p, net_rep(s->token));
        s->socket_live=false; --s->refs;
        return 0;
    }
    if (u && u->p1_live) {
        /* These identities are consumed even if pause reports an error. CNet
         * retains requests until stop/routing acknowledges actual terminals. */
        u->p1_live=false;
        (void)net_pair_drop(p, net_rep(u->pair->token[0]), 0);
        (void)net_pair_drop(p, net_rep(u->pair->token[1]), 1);
        (void)net_udp_drop(p, net_rep(u->token));
        return 0;
    }
    return TURBOWASM_WASI_ERRNO_BADF;
}
static uint32_t p1_stat(void *ctx, turbowasm_wasi_fs_file f, turbowasm_wasi_fs_stat *out) {
    tw_cnet_slot *s=p1_tcp(ctx,f); tw_udp *u=p1_udp(ctx,f);
    if (!out) return TURBOWASM_WASI_ERRNO_INVAL;
    if ((!s || !s->p1_live) && (!u || !u->p1_live)) return TURBOWASM_WASI_ERRNO_BADF;
    *out=(turbowasm_wasi_fs_stat){0}; out->file_type=s ? TURBOWASM_WASI_FILETYPE_SOCKET_STREAM : TURBOWASM_WASI_FILETYPE_SOCKET_DGRAM;
    return 0;
}
static uint32_t p1_ready(void *ctx, turbowasm_wasi_fs_file f, uint8_t direction, turbowasm_wasi_readiness *out) {
    tw_cnet_impl *p=ctx; tw_cnet_slot *s=p1_tcp(p,f); tw_udp *u=p1_udp(p,f);
    *out=(turbowasm_wasi_readiness){0};
    if (p->busy) return TURBOWASM_WASI_ERRNO_BUSY;
    if (s && s->p1_live) {
        if (s->state == TW_CNET_SLOT_LISTENING) {
            out->ready=s->accept_ready || s->closing;
            if (s->accept_ready && s->accept_status) return p1_native_error(s->accept_status);
            return direction == TURBOWASM_WASI_EVENT_FD_READ ? 0 : TURBOWASM_WASI_ERRNO_NOTCONN;
        }
        bool read=direction == TURBOWASM_WASI_EVENT_FD_READ;
        bool closed=read ? s->rx_closed : s->tx_closed;
        out->ready=closed || s->terminal || p->stopping ||
            (read ? s->rx_size != 0 : !s->tx_busy && !s->flushing);
        out->bytes=read ? s->rx_size : out->ready && !closed && !s->terminal ? p->config.send_bytes : 0;
        if (closed || s->terminal) out->flags=TURBOWASM_WASI_EVENT_HANGUP;
        if (s->terminal_status && (!read || !s->rx_size)) return p1_native_error(s->terminal_status);
        return 0;
    }
    if (u && u->p1_live) {
        bool read=direction == TURBOWASM_WASI_EVENT_FD_READ;
        bool closed=read ? u->p1_read_closed : u->p1_write_closed;
        out->ready=closed || u->terminal || u->stopping || p->stopping ||
            (read ? u->count || u->receive_error || !p->network->config.allow_udp_receive :
                u->send_error || !p->network->config.allow_udp_send || u->sends < p->network->config.send_datagrams);
        out->bytes=read ? u->count ? u->packets[u->head].size : 0 : out->ready ? p->network->config.datagram_bytes : 0;
        if (closed || u->terminal || u->stopping) out->flags=TURBOWASM_WASI_EVENT_HANGUP;
        if (read && u->receive_error && !u->count) return p1_native_error(u->receive_error);
        if (!read && u->send_error) return p1_native_error(u->send_error);
        return 0;
    }
    return TURBOWASM_WASI_ERRNO_BADF;
}
static size_t p1_scatter(const uint8_t *data, size_t available, const turbowasm_wasi_buffer *buffers, size_t count) {
    size_t copied=0;
    for (size_t i=0; i<count && copied<available; ++i) {
        size_t n=buffers[i].size; if (n > available-copied) n=available-copied;
        if (n) memcpy(buffers[i].data, data+copied, n); copied+=n;
    }
    return copied;
}
static uint32_t p1_recv(void *ctx, turbowasm_wasi_fs_file f, const turbowasm_wasi_buffer *buffers,
    size_t count, uint16_t flags, bool nonblock, uint32_t *out, uint16_t *roflags) {
    tw_cnet_impl *p=ctx; tw_cnet_slot *s=p1_tcp(p,f); tw_udp *u=p1_udp(p,f);
    if (!out || !roflags || (count && !buffers) || count > TURBOWASM_WASI_IOV_MAX || (flags & ~3u))
        return TURBOWASM_WASI_ERRNO_INVAL;
    size_t capacity=0; *out=0; *roflags=0;
    if (p->busy) return TURBOWASM_WASI_ERRNO_BUSY;
    for (size_t i=0; i<count; ++i) {
        if (buffers[i].size > UINT32_MAX-capacity || (buffers[i].size && !buffers[i].data)) return TURBOWASM_WASI_ERRNO_INVAL;
        capacity+=buffers[i].size;
    }
    if (s && s->p1_live) {
        if (s->state != TW_CNET_SLOT_CONNECTED) return TURBOWASM_WASI_ERRNO_NOTCONN;
        if (!capacity) return 0;
        bool peek=(flags & TURBOWASM_WASI_RECV_PEEK) != 0;
        if (peek && (flags & TURBOWASM_WASI_RECV_WAITALL) && !nonblock && capacity > p->config.receive_bytes)
            return TURBOWASM_WASI_ERRNO_MSGSIZE;
        if (peek && (flags & TURBOWASM_WASI_RECV_WAITALL) && !nonblock && s->rx_size < capacity &&
            !s->rx_closed && !s->terminal && !s->terminal_status) {
            s->p1_peek_target=capacity; return TURBOWASM_WASI_ERRNO_AGAIN;
        }
        s->p1_peek_target=0;
        if (!s->rx_size) {
            if (s->terminal_status) return p1_native_error(s->terminal_status);
            return s->rx_closed || s->terminal ? 0 : TURBOWASM_WASI_ERRNO_AGAIN;
        }
        size_t n=p1_scatter(s->rx+s->rx_offset, s->rx_size, buffers, count); *out=(uint32_t)n;
        if (!peek) { s->rx_offset+=n; s->rx_size-=n; native_changed(s); }
        return 0;
    }
    if (u && u->p1_live) {
        if (!p->network->config.allow_udp_receive) return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
        if (u->p1_read_closed) return 0;
        if (!u->count) {
            if (u->receive_error) { int e=u->receive_error; u->receive_error=0; return p1_native_error(e); }
            return u->terminal || u->stopping ? TURBOWASM_WASI_ERRNO_NOTCONN : TURBOWASM_WASI_ERRNO_AGAIN;
        }
        size_t size=u->packets[u->head].size;
        size_t n=p1_scatter(u->bytes+u->head*p->network->config.datagram_bytes, size, buffers, count);
        *out=(uint32_t)n; if (n<size) *roflags=TURBOWASM_WASI_RECV_DATA_TRUNCATED;
        if (!(flags & TURBOWASM_WASI_RECV_PEEK)) { u->head=(u->head+1)%p->network->config.receive_datagrams; --u->count; udp_changed(u); }
        return 0;
    }
    return TURBOWASM_WASI_ERRNO_BADF;
}
static uint32_t p1_send(void *ctx, turbowasm_wasi_fs_file f, const turbowasm_wasi_const_buffer *buffers,
    size_t count, uint32_t *out) {
    tw_cnet_impl *p=ctx; tw_cnet_slot *s=p1_tcp(p,f); tw_udp *u=p1_udp(p,f);
    if (!out || (count && !buffers) || count > TURBOWASM_WASI_IOV_MAX) return TURBOWASM_WASI_ERRNO_INVAL;
    size_t total=0; *out=0;
    if (p->busy) return TURBOWASM_WASI_ERRNO_BUSY;
    for (size_t i=0; i<count; ++i) {
        if (buffers[i].size > UINT32_MAX-total || (buffers[i].size && !buffers[i].data)) return TURBOWASM_WASI_ERRNO_INVAL;
        total+=buffers[i].size;
    }
    if (s && s->p1_live) {
        if (s->state != TW_CNET_SLOT_CONNECTED) return TURBOWASM_WASI_ERRNO_NOTCONN;
        if (s->tx_closed || (s->terminal && !s->terminal_status)) return TURBOWASM_WASI_ERRNO_PIPE;
        if (s->terminal_status) return p1_native_error(s->terminal_status);
        if (!total) return 0;
        if (s->tx_busy || s->flushing) return TURBOWASM_WASI_ERRNO_AGAIN;
        size_t n=total < p->config.send_bytes ? total : p->config.send_bytes, pos=0;
        /* Use the existing retained transmit buffer; native_write's copy uses
         * identical addresses here, so avoid that overlap by admitting directly. */
        for (size_t i=0; i<count && pos<n; ++i) {
            size_t part=buffers[i].size; if (part>n-pos) part=n-pos;
            if (part) memcpy((uint8_t *)mem_buffer_data(s->tx)+pos, buffers[i].data, part); pos+=part;
        }
        mem_set_used(s->tx, n); s->tx_size=n; s->tx_busy=true;
        int rc=cnet_send_buffer(&p->client, s->connection, s->tx);
        if (rc != SALTS_OK) { s->tx_busy=false; s->tx_size=0; return p1_native_error(rc); }
        *out=(uint32_t)n; native_changed(s); return 0;
    }
    if (u && u->p1_live) {
        if (u->p1_write_closed) return TURBOWASM_WASI_ERRNO_PIPE;
        if (total > p->network->config.datagram_bytes) return TURBOWASM_WASI_ERRNO_MSGSIZE;
        uint64_t permit=0; turbowasm_wasi02_socket_error error;
        turbowasm_status status=net_check_send(p, net_rep(u->pair->token[1]), &permit, &error);
        if (status != TURBOWASM_OK) return TURBOWASM_WASI_ERRNO_IO;
        if (error) return p1_socket_error(error);
        if (!permit) return TURBOWASM_WASI_ERRNO_AGAIN;
        uint8_t *scratch=u->p1_scratch; size_t pos=0;
        for (size_t i=0; i<count; ++i) { if (buffers[i].size) memcpy(scratch+pos, buffers[i].data, buffers[i].size); pos+=buffers[i].size; }
        turbowasm_wasi02_outgoing_datagram record={0}; record.data=scratch; record.size=total;
        size_t sent=0; status=net_send(p, net_rep(u->pair->token[1]), &record, 1, &sent, &error);
        if (status != TURBOWASM_OK) return TURBOWASM_WASI_ERRNO_IO;
        if (error) return p1_socket_error(error);
        if (!sent) return TURBOWASM_WASI_ERRNO_AGAIN;
        *out=(uint32_t)total; return 0;
    }
    return TURBOWASM_WASI_ERRNO_BADF;
}
static uint32_t p1_read(void *c, turbowasm_wasi_fs_file f, const turbowasm_wasi_buffer *b, size_t n, uint32_t *out) {
    uint16_t flags=0; return p1_recv(c,f,b,n,0,true,out,&flags);
}
static uint32_t p1_shutdown(void *ctx, turbowasm_wasi_fs_file f, uint8_t how) {
    tw_cnet_impl *p=ctx; tw_cnet_slot *s=p1_tcp(p,f); tw_udp *u=p1_udp(p,f);
    if (p->busy) return TURBOWASM_WASI_ERRNO_BUSY;
    if (!how || (how & ~3u)) return TURBOWASM_WASI_ERRNO_INVAL;
    if (s && s->p1_live) {
        if (s->state != TW_CNET_SLOT_CONNECTED || s->terminal) return TURBOWASM_WASI_ERRNO_NOTCONN;
        int rc=cnet_connection_shutdown(&p->client, s->connection, (cnet_tcp_shutdown)how);
        if (rc != SALTS_OK) return p1_native_error(rc);
        if (how & 1) { s->rx_closed=true; s->rx_size=0; s->p1_peek_target=0; }
        if (how & 2) { s->tx_closed=true; s->permit=0; }
        native_changed(s); return 0;
    }
    if (u && u->p1_live) {
        if (how & 1) {
            int rc=cnet_datagram_pause_receive(&u->datagram);
            if (rc != SALTS_OK) return p1_native_error(rc);
            u->p1_read_closed=true; u->count=0;
        }
        if (how & 2) u->p1_write_closed=true;
        udp_changed(u); return 0;
    }
    return TURBOWASM_WASI_ERRNO_BADF;
}

static bool p1_move_allowed(tw_cnet_impl *p) {
    return p && p->external && !p->busy && !p->stopping && !p->facades;
}
static turbowasm_status p1_expand_rx(tw_cnet_slot *s) {
    tw_cnet_impl *p=s->owner;
    if (p->config.receive_bytes > SIZE_MAX/2) return TURBOWASM_OUT_OF_MEMORY;
    size_t capacity=2*p->config.receive_bytes;
    if (s->rx_capacity >= capacity) return TURBOWASM_OK;
    size_t extra=capacity-s->rx_capacity;
    if (extra > p->config.payload_bytes-p->payload_used) return TURBOWASM_OUT_OF_MEMORY;
    turbowasm_runtime_scope scope=turbowasm_runtime_scope_enter(&p->runtime);
    uint8_t *data=turbowasm_rt_malloc(capacity);
    turbowasm_runtime_scope_leave(scope);
    if (!data) return TURBOWASM_OUT_OF_MEMORY;
    if (s->rx_size) memcpy(data, s->rx+s->rx_offset, s->rx_size);
    turbowasm_rt_free(s->rx); s->rx=data; s->rx_offset=0; s->rx_capacity=capacity;
    s->payload_charge+=extra; p->payload_used+=extra; return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi_cnet_listener_move(turbowasm_wasi02_cnet *a,
    turbowasm_value *socket, turbowasm_wasi_fs_file *out) {
    tw_cnet_impl *p=impl_mut(a); tw_cnet_slot *s=socket ? slot_lookup(p,*socket) : NULL;
    if (!p1_move_allowed(p) || !s || !out || !s->socket_live || s->p1_live ||
        s->state != TW_CNET_SLOT_LISTENING ||
        !turbowasm_wasi02_io_source_exclusive_private(p->io,s->sources[0])) return TURBOWASM_INVALID_ARGUMENT;
    s->p1_live=true; *out=(turbowasm_wasi_fs_file){s->token,s->generation}; *socket=(turbowasm_value){0}; return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi_cnet_tcp_move(turbowasm_wasi02_cnet *a,
    turbowasm_value *socket, turbowasm_value *input, turbowasm_value *output, turbowasm_wasi_fs_file *out) {
    tw_cnet_impl *p=impl_mut(a); tw_cnet_slot *s=socket ? slot_lookup(p,*socket) : NULL;
    if (!p1_move_allowed(p) || !s || !out || !input || !output || socket==input || socket==output || input==output ||
        !s->socket_live || s->p1_live || s->state != TW_CNET_SLOT_CONNECTED || !s->input_live || !s->output_live ||
        s->tx_busy || s->flushing ||
        !turbowasm_wasi02_io_source_exclusive_private(p->io,s->sources[0]) ||
        !turbowasm_wasi02_io_detach_pair_private(p->io,*input,*output,p,*socket,false)) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status=p1_expand_rx(s); if (status != TURBOWASM_OK) return status;
    if (!turbowasm_wasi02_io_detach_pair_private(p->io,*input,*output,p,*socket,true)) return TURBOWASM_INVALID_ARGUMENT;
    s->p1_live=true; *out=(turbowasm_wasi_fs_file){s->token,s->generation};
    *socket=(turbowasm_value){0}; *input=(turbowasm_value){0}; *output=(turbowasm_value){0}; return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi_cnet_udp_move(turbowasm_wasi02_cnet *a,
    turbowasm_value *socket, turbowasm_value *input, turbowasm_value *output, turbowasm_wasi_fs_file *out) {
    tw_cnet_impl *p=impl_mut(a); tw_udp *u=socket ? udp_get(p,*socket) : NULL;
    tw_udp_pair *in=input ? pair_get(p,*input,0) : NULL, *o=output ? pair_get(p,*output,1) : NULL;
    if (!p1_move_allowed(p) || !u || !out || !input || !output || socket==input || socket==output || input==output ||
        !u->live || u->p1_live || !u->bound || !u->associated || u->stopping || u->terminal ||
        !in || in != o || u->pair != in || in->refs != 4 || u->sends ||
        !turbowasm_wasi02_io_source_exclusive_private(p->io,u->source) ||
        !turbowasm_wasi02_io_source_exclusive_private(p->io,in->source[0]) ||
        !turbowasm_wasi02_io_source_exclusive_private(p->io,in->source[1])) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_runtime_scope scope=turbowasm_runtime_scope_enter(&p->runtime);
    uint8_t *scratch=turbowasm_rt_malloc(p->network->config.datagram_bytes);
    turbowasm_runtime_scope_leave(scope);
    if (!scratch) return TURBOWASM_OUT_OF_MEMORY;
    u->p1_scratch=scratch;
    u->p1_live=true; *out=(turbowasm_wasi_fs_file){u->token,1};
    *socket=(turbowasm_value){0}; *input=(turbowasm_value){0}; *output=(turbowasm_value){0}; return TURBOWASM_OK;
}
static uint32_t p1_accept(void *ctx, turbowasm_wasi_fs_file f, turbowasm_wasi_fs_file *out) {
    tw_cnet_impl *p=ctx; tw_cnet_slot *listener=p1_tcp(p,f);
    if (!listener || !listener->p1_live) return TURBOWASM_WASI_ERRNO_BADF;
    if (!p1_move_allowed(p)) return TURBOWASM_WASI_ERRNO_BUSY;
    turbowasm_value child={0}, input={0}, output={0}; turbowasm_wasi02_socket_error error;
    p->p1_accept=true;
    turbowasm_status status=native_accept(p, net_rep(f.object), &child, &input, &output, &error);
    p->p1_accept=false;
    if (status != TURBOWASM_OK) return TURBOWASM_WASI_ERRNO_IO;
    if (error) return p1_socket_error(error);
    turbowasm_wasi02_cnet adapter={p};
    status=turbowasm_wasi_cnet_tcp_move(&adapter,&child,&input,&output,out);
    if (status != TURBOWASM_OK) {
        tw_cnet_slot *s=slot_lookup(p,child);
        if (s) { s->input_rep=input; s->output_rep=output; native_drop_pending(s); }
        (void)provider_tcp_drop(p,child); return TURBOWASM_WASI_ERRNO_IO;
    }
    return 0;
}
turbowasm_status turbowasm_wasi_cnet_descriptor_ops(turbowasm_wasi02_cnet *a, turbowasm_wasi_descriptor_ops *out) {
    tw_cnet_impl *p=impl_mut(a);
    if (!p || !p->external || p->busy || !out) return TURBOWASM_INVALID_ARGUMENT;
    *out=(turbowasm_wasi_descriptor_ops){0}; out->size=sizeof(*out); out->api_version=1;
    out->file.context=p; out->file.close=p1_close; out->file.read=p1_read; out->file.write=p1_send; out->file.stat=p1_stat;
    out->retain=p1_retain; out->release=p1_release; out->ready=p1_ready; out->accept=p1_accept;
    out->recv=p1_recv; out->send=p1_send; out->shutdown=p1_shutdown; out->finish=p1_finish;
    return TURBOWASM_OK;
}

