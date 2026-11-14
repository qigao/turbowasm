#include "wasi_provider_private.h"
#include "wasi_dispatch_private.h"

static uint32_t tw_provider_run(void *context) {
    tw_wasi_provider_request *r = context;
    const tw_wasi_fd_lease *l = r->lease;
    const turbowasm_wasi_descriptor_ops *o = &l->ops;
    void *c = o->file.context;
    turbowasm_wasi_fs_file f = l->file;
    switch (r->operation) {
    case TW_PROVIDER_CLOSE: return o->file.close(c, f);
    case TW_PROVIDER_READ: return o->file.read(c, f, r->as.read.buffers, r->as.read.count, r->as.read.out);
    case TW_PROVIDER_WRITE: return o->file.write(c, f, r->as.write.buffers, r->as.write.count, r->as.write.out);
    case TW_PROVIDER_SEEK: return o->file.seek(c, f, r->as.seek.offset, r->as.seek.whence, r->as.seek.out);
    case TW_PROVIDER_TELL: return o->file.tell(c, f, r->as.tell);
    case TW_PROVIDER_STAT: return o->file.stat(c, f, r->as.stat);
    case TW_PROVIDER_PATH_OPEN: return o->file.path_open(c, f, r->as.open.dirflags, r->as.open.path,
        r->as.open.length, r->as.open.oflags, r->as.open.base, r->as.open.inheriting,
        r->as.open.flags, r->as.open.out);
    case TW_PROVIDER_PATH_STAT: return o->file.path_stat(c, f, r->as.path_stat.flags,
        r->as.path_stat.path, r->as.path_stat.length, r->as.path_stat.out);
    case TW_PROVIDER_PATH_MUTATE: return r->as.mutate.fn(c, f, r->as.mutate.path, r->as.mutate.length);
    case TW_PROVIDER_READDIR: return o->file.readdir(c, f, r->as.readdir.cookie,
        r->as.readdir.out, r->as.readdir.has_entry);
    case TW_PROVIDER_RENAME: return o->file.path_rename(c, f, r->as.rename.source,
        r->as.rename.source_length, r->as.rename.target_file, r->as.rename.target, r->as.rename.target_length);
    case TW_PROVIDER_FLAGS: return o->file.set_flags(c, f, r->as.flags);
    case TW_PROVIDER_RETAIN: return o->retain(c, f);
    case TW_PROVIDER_CLEANUP:
        if (l->claimed && o->finish) o->finish(c, f, l->direction);
        if (o->release) o->release(c, f);
        return 0;
    case TW_PROVIDER_READY: return o->ready(c, f, r->as.ready.direction, r->as.ready.out);
    case TW_PROVIDER_ACCEPT: return o->accept(c, f, r->as.accept);
    case TW_PROVIDER_RECV: return o->recv(c, f, r->as.recv.buffers, r->as.recv.count,
        r->as.recv.flags, r->as.recv.nonblock, r->as.recv.out, r->as.recv.out_flags);
    case TW_PROVIDER_SEND: return o->send(c, f, r->as.write.buffers, r->as.write.count, r->as.write.out);
    case TW_PROVIDER_SHUTDOWN: return o->shutdown(c, f, r->as.shutdown);
    }
    return TURBOWASM_WASI_ERRNO_INVAL;
}

uint32_t tw_wasi_provider_call(const tw_wasi_fd_lease *lease, tw_wasi_provider_request *r) {
    r->lease = lease;
    bool cleanup = r->operation == TW_PROVIDER_CLEANUP;
    return tw_wasi_dispatch_call(lease->dispatch, tw_provider_run, r,
        cleanup || r->operation == TW_PROVIDER_CLOSE, cleanup);
}
