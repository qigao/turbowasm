#include "wasi02_io_api.h"
#include "runtime_alloc.h"
#include <salts/error_codes.h>
#include <stdatomic.h>
#include <string.h>

/* Unique identities only; this counter owns no runtime state. Exhaustion fails
 * rather than allowing an old domain/provider token to alias a new owner. */
static atomic_uint_fast64_t io_identity;
static uint64_t identity(void) {
    uint_fast64_t value = atomic_load(&io_identity);
    while (value != UINT64_MAX) {
        if (atomic_compare_exchange_weak(&io_identity, &value, value + 1u)) return (uint64_t)value + 1u;
    }
    return 0;
}
uint64_t turbowasm_wasi02_io_token_private(void) { return identity(); }
typedef struct io_source {
    uint64_t token;
    uint32_t refs;
    bool closed;
    turbowasm_wasi02_io_source_ops ops;
} io_source;
typedef struct io_pollable { uint64_t token; uint32_t source; } io_pollable;
typedef struct io_stream {
    uint64_t token;
    uint32_t source;
    turbowasm_wasi02_io_stream_kind kind;
    turbowasm_value rep;
    turbowasm_wasi02_stream_provider ops;
} io_stream;
typedef struct io_error {
    uint64_t token;
    uint32_t source;
    bool retained;
    turbowasm_value rep;
    turbowasm_wasi02_stream_provider ops;
} io_error;
typedef struct io_route {
    uintptr_t token;
    turbowasm_host_call *call;
    turbowasm_host_wait wait;
    uint32_t count;
    uint32_t *sources;
} io_route;
typedef struct io_impl {
    uint64_t domain;
    bool busy;
    bool published;
    bool factory_running;
    uint32_t bindings;
    bool dirty;
    turbowasm_wasi02_io_config config;
    turbowasm_runtime_config runtime;
    io_source *sources;
    io_pollable *pollables;
    io_stream *streams;
    io_error *errors;
    io_route *routes;
    uint32_t *members;
    turbowasm_wasi02_stream_factory_fn factories[3];
    void *factory_contexts[3];
} io_impl;
static io_impl *impl_of(turbowasm_wasi02_io *io) { return io == NULL ? NULL : (io_impl *)io->impl; }
static bool enter(io_impl *p) { if (p == NULL || p->busy) return false; p->busy = true; return true; }
static turbowasm_value token_rep(uint64_t token) {
    turbowasm_value rep = {0}; rep.kind = TURBOWASM_VALUE_I64; rep.as.i64 = (int64_t)token; return rep;
}
static uint64_t rep_token(turbowasm_value rep) { return rep.kind == TURBOWASM_VALUE_I64 ? (uint64_t)rep.as.i64 : 0; }
static io_source *source_of(io_impl *p, turbowasm_wasi02_io_source handle) {
    uint32_t i;
    if (p == NULL || handle.domain != p->domain || handle.token == 0) return NULL;
    for (i = 0; i < p->config.sources; ++i) if (p->sources[i].token == handle.token) return &p->sources[i];
    return NULL;
}
static io_pollable *pollable_of(io_impl *p, turbowasm_value rep) {
    uint32_t i; uint64_t token = rep_token(rep);
    if (p == NULL || token == 0) return NULL;
    for (i = 0; i < p->config.subscriptions; ++i) if (p->pollables[i].token == token) return &p->pollables[i];
    return NULL;
}
static io_stream *stream_of(io_impl *p, turbowasm_value rep, turbowasm_wasi02_io_stream_kind kind) {
    uint32_t i; uint64_t token = rep_token(rep);
    if (p == NULL || token == 0) return NULL;
    for (i = 0; i < p->config.streams; ++i)
        if (p->streams[i].token == token && p->streams[i].kind == kind) return &p->streams[i];
    return NULL;
}
static void source_release(io_impl *p, uint32_t index) {
    io_source *s = &p->sources[index];
    if (--s->refs == 0) {
        turbowasm_wasi02_io_source_ops ops = s->ops;
        memset(s, 0, sizeof(*s));
        ops.release(ops.context);
    }
}
static turbowasm_status source_ready(io_impl *p, uint32_t index, bool *ready) {
    io_source *s = &p->sources[index];
    *ready = s->closed;
    return s->closed ? TURBOWASM_OK : s->ops.ready(s->ops.context, ready);
}
void turbowasm_wasi02_io_config_init(turbowasm_wasi02_io_config *config) {
    if (config != NULL) *config = (turbowasm_wasi02_io_config){sizeof(*config), 1u, 512u, 512u, 128u, 1024u, 64u, 64u};
}
turbowasm_status turbowasm_wasi02_io_init(turbowasm_wasi02_io *io,
    const turbowasm_wasi02_io_config *config, const turbowasm_runtime_config *runtime) {
    io_impl *p; uint32_t i; size_t count; turbowasm_runtime_config normalized; turbowasm_runtime_scope scope;
    turbowasm_wasi02_io_config defaults;
    io_impl constructing = {0};
    if (config == NULL) { turbowasm_wasi02_io_config_init(&defaults); config = &defaults; }
    if (io == NULL || io->impl != NULL || config->size != sizeof(*config) || config->api_version != 1u ||
        config->sources == 0 || config->streams == 0 || config->errors == 0 || config->subscriptions == 0 ||
        config->routes == 0 || config->members_per_route == 0 ||
        config->routes > SIZE_MAX / config->members_per_route) return TURBOWASM_INVALID_ARGUMENT;
    if (!turbowasm_runtime_config_normalize(runtime, &normalized)) return TURBOWASM_INVALID_ARGUMENT;
    count = (size_t)config->routes * config->members_per_route;
    constructing.busy = true; io->impl = &constructing;
    scope = turbowasm_runtime_scope_enter(&normalized);
    p = turbowasm_rt_calloc(1, sizeof(*p));
    if (p != NULL) {
        p->config = *config; p->runtime = normalized; p->domain = identity();
        p->sources = turbowasm_rt_calloc(config->sources, sizeof(*p->sources));
        p->pollables = turbowasm_rt_calloc(config->subscriptions, sizeof(*p->pollables));
        p->streams = turbowasm_rt_calloc(config->streams, sizeof(*p->streams));
        p->errors = turbowasm_rt_calloc(config->errors, sizeof(*p->errors));
        p->routes = turbowasm_rt_calloc(config->routes, sizeof(*p->routes));
        p->members = turbowasm_rt_calloc(count, sizeof(*p->members));
        if (!p->domain || !p->sources || !p->pollables || !p->streams || !p->errors || !p->routes || !p->members) {
            turbowasm_rt_free(p->members); turbowasm_rt_free(p->routes); turbowasm_rt_free(p->errors);
            turbowasm_rt_free(p->streams); turbowasm_rt_free(p->pollables); turbowasm_rt_free(p->sources);
            turbowasm_rt_free(p); p = NULL;
        } else for (i = 0; i < config->routes; ++i) p->routes[i].sources = p->members + (size_t)i * config->members_per_route;
    }
    turbowasm_runtime_scope_leave(scope);
    if (p == NULL) { io->impl = NULL; return TURBOWASM_OUT_OF_MEMORY; }
    io->impl = p; return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi02_io_destroy(turbowasm_wasi02_io *io) {
    io_impl *p = impl_of(io); uint32_t i;
    if (io == NULL) return TURBOWASM_INVALID_ARGUMENT;
    if (p == NULL) return TURBOWASM_OK;
    if (p->factory_running || p->bindings || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0; i < p->config.sources; ++i) if (p->sources[i].token) goto live;
    for (i = 0; i < p->config.errors; ++i) if (p->errors[i].token) goto live;
    for (i = 0; i < p->config.routes; ++i) if (p->routes[i].token) goto live;
    io->impl = NULL;
    turbowasm_rt_free(p->members); turbowasm_rt_free(p->routes); turbowasm_rt_free(p->errors);
    turbowasm_rt_free(p->streams); turbowasm_rt_free(p->pollables); turbowasm_rt_free(p->sources); turbowasm_rt_free(p);
    return TURBOWASM_OK;
live: p->busy = false; return TURBOWASM_INVALID_ARGUMENT;
}
turbowasm_status turbowasm_wasi02_io_source_register(turbowasm_wasi02_io *io,
    const turbowasm_wasi02_io_source_ops *ops, turbowasm_wasi02_io_source *out) {
    io_impl *p = impl_of(io); uint32_t i; uint64_t token;
    if (ops == NULL || out == NULL || out->token || out->domain || !ops->ready || !ops->retain || !ops->release || !enter(p))
        return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0; i < p->config.sources; ++i) if (!p->sources[i].token) break;
    token = i == p->config.sources ? 0 : identity();
    if (!token) { p->busy = false; return TURBOWASM_OUT_OF_MEMORY; }
    p->sources[i] = (io_source){token, 1u, false, *ops};
    ops->retain(ops->context);
    *out = (turbowasm_wasi02_io_source){p->domain, token}; p->busy = false; return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi02_io_source_changed(turbowasm_wasi02_io *io, turbowasm_wasi02_io_source handle) {
    io_impl *p = impl_of(io); io_source *s = source_of(p, handle);
    if (s == NULL || s->closed) return TURBOWASM_INVALID_ARGUMENT;
    p->dirty = true; return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi02_io_source_close(turbowasm_wasi02_io *io, turbowasm_wasi02_io_source *handle) {
    io_impl *p = impl_of(io); io_source *s;
    if (handle == NULL || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    s = source_of(p, *handle);
    if (s == NULL || s->closed) { p->busy = false; return TURBOWASM_INVALID_ARGUMENT; }
    s->closed = true; p->dirty = true; memset(handle, 0, sizeof(*handle));
    source_release(p, (uint32_t)(s - p->sources)); p->busy = false; return TURBOWASM_OK;
}
static turbowasm_status pollable_new(io_impl *p, uint32_t source, turbowasm_value *out) {
    uint32_t i; uint64_t token;
    if (p->sources[source].refs == UINT32_MAX) return TURBOWASM_OUT_OF_MEMORY;
    for (i = 0; i < p->config.subscriptions; ++i) if (!p->pollables[i].token) break;
    token = i == p->config.subscriptions ? 0 : identity();
    if (!token) return TURBOWASM_OUT_OF_MEMORY;
    p->pollables[i] = (io_pollable){token, source}; ++p->sources[source].refs; *out = token_rep(token); return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi02_io_pollable_register(turbowasm_wasi02_io *io,
    turbowasm_wasi02_io_source handle, turbowasm_value *out) {
    io_impl *p = impl_of(io); io_source *s; turbowasm_status status;
    if (out == NULL || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    s = source_of(p, handle);
    status = s == NULL || s->closed ? TURBOWASM_INVALID_ARGUMENT : pollable_new(p, (uint32_t)(s - p->sources), out);
    p->busy = false; return status;
}
static turbowasm_status poll_ready(void *context, turbowasm_value rep, bool *ready) {
    io_impl *p = context; io_pollable *slot; turbowasm_status status;
    if (ready == NULL || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    slot = pollable_of(p, rep); status = slot ? source_ready(p, slot->source, ready) : TURBOWASM_TRAPPED;
    p->busy = false; return status;
}
static turbowasm_status poll_drop(void *context, turbowasm_value rep) {
    io_impl *p = context; io_pollable *slot; uint32_t index;
    if (!enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    slot = pollable_of(p, rep);
    if (!slot) { p->busy = false; return TURBOWASM_TRAPPED; }
    index = slot->source; memset(slot, 0, sizeof(*slot)); source_release(p, index); p->busy = false; return TURBOWASM_OK;
}
static void route_release(io_impl *p, io_route *r) {
    uint32_t i; uint32_t *members = r->sources;
    for (i = 0; i < r->count; ++i) source_release(p, members[i]);
    memset(r, 0, sizeof(*r)); r->sources = members;
}
void turbowasm_wasi02_io_wait_done(void *context, uintptr_t token) {
    io_impl *p = context; uint32_t i;
    if (!enter(p)) return;
    for (i = 0; i < p->config.routes; ++i)
        if (p->routes[i].token == token) { route_release(p, &p->routes[i]); break; }
    p->busy = false;
}
static turbowasm_status poll_arm_many(void *context, const turbowasm_value *reps, size_t count,
    turbowasm_host_call *call, uintptr_t *token, turbowasm_host_wait **storage) {
    io_impl *p = context; io_route *r = NULL; size_t i; uint32_t j; uint64_t id;
    turbowasm_status status = TURBOWASM_OUT_OF_MEMORY;
    if (reps == NULL || call == NULL || token == NULL || storage == NULL || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    if (!count || count > p->config.members_per_route) { status = TURBOWASM_INVALID_ARGUMENT; goto done; }
    for (j = 0; j < p->config.routes; ++j) if (!p->routes[j].token) { r = &p->routes[j]; break; }
    if (r == NULL || !(id = identity()) || id > UINTPTR_MAX) goto done;
    r->token = (uintptr_t)id; r->call = call;
    for (i = 0; i < count; ++i) {
        io_pollable *slot = pollable_of(p, reps[i]);
        if (slot == NULL) { status = TURBOWASM_TRAPPED; goto rollback; }
        if (p->sources[slot->source].refs == UINT32_MAX) goto rollback;
        r->sources[r->count++] = slot->source; ++p->sources[slot->source].refs;
    }
    /* No callback can progress another owner recursively. The drive rechecks
     * after the Runtime has committed its exact wait generation. */
    p->dirty = true; *token = r->token; *storage = &r->wait; status = TURBOWASM_OK; goto done;
rollback: route_release(p, r);
done: p->busy = false; return status;
}
static turbowasm_status poll_arm(void *context, turbowasm_value rep, turbowasm_host_call *call,
    uintptr_t *token, turbowasm_host_wait **storage) { return poll_arm_many(context, &rep, 1u, call, token, storage); }
turbowasm_status turbowasm_wasi02_io_advance(turbowasm_wasi02_io *io) {
    io_impl *p = impl_of(io); uint32_t i, j; turbowasm_status first = TURBOWASM_OK;
    if (!enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    /* Always scan: readiness can advance through a timer/provider even without
     * an explicit source_changed notification. The configured bounds limit work. */
    for (i = 0; i < p->config.routes; ++i) {
        io_route *r = &p->routes[i]; turbowasm_status status = TURBOWASM_OK; bool ready = false;
        if (!r->token || !r->wait.generation) continue;
        for (j = 0; j < r->count && !ready; ++j) {
            status = source_ready(p, r->sources[j], &ready);
            if (status != TURBOWASM_OK) break;
        }
        if (!ready && status == TURBOWASM_OK) continue;
        if (first == TURBOWASM_OK && status != TURBOWASM_OK) first = status;
        status = turbowasm_host_call_complete_wait(r->call, r->wait, status == TURBOWASM_OK ? SALTS_OK : SALTS_EIO);
        route_release(p, r);
        if (first == TURBOWASM_OK && status != TURBOWASM_OK) first = status;
    }
    p->dirty = false; p->busy = false; return first;
}
turbowasm_status turbowasm_wasi02_io_stream_register(turbowasm_wasi02_io *io,
    turbowasm_wasi02_io_stream_kind kind, const turbowasm_wasi02_stream_provider *ops,
    turbowasm_value rep, turbowasm_wasi02_io_source handle, turbowasm_value *out) {
    io_impl *p = impl_of(io); io_source *s; uint32_t i; uint64_t token;
    if (ops == NULL || out == NULL || (kind != TURBOWASM_WASI02_IO_INPUT && kind != TURBOWASM_WASI02_IO_OUTPUT) ||
        (kind == TURBOWASM_WASI02_IO_INPUT ? !ops->input_drop : !ops->output_drop) || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    s = source_of(p, handle);
    if (s == NULL || s->closed) { p->busy = false; return TURBOWASM_INVALID_ARGUMENT; }
    for (i = 0; i < p->config.streams; ++i) if (!p->streams[i].token) break;
    token = i == p->config.streams || s->refs == UINT32_MAX ? 0 : identity();
    if (!token) { p->busy = false; return TURBOWASM_OUT_OF_MEMORY; }
    p->streams[i] = (io_stream){token, (uint32_t)(s - p->sources), kind, rep, *ops}; ++s->refs;
    *out = token_rep(token); p->busy = false; return TURBOWASM_OK;
}
static turbowasm_status wrap_error(io_impl *p, io_stream *s, turbowasm_wasi02_stream_error *error, io_error *reserved) {
    if (error->kind != TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED) return TURBOWASM_OK;
    if (!s->ops.error_drop || !s->ops.error_debug) return TURBOWASM_TRAPPED;
    reserved->source = s->source; reserved->retained = true; ++p->sources[s->source].refs;
    reserved->rep = error->error_rep; reserved->ops = s->ops; error->error_rep = token_rep(reserved->token); return TURBOWASM_OK;
}
static io_error *error_reserve(io_impl *p) {
    uint32_t i;
    for (i = 0; i < p->config.errors; ++i) if (!p->errors[i].token) {
        p->errors[i].token = identity(); return p->errors[i].token ? &p->errors[i] : NULL;
    }
    return NULL;
}
static io_error *stream_error_reserve(io_impl *p, io_stream *stream) {
    return stream && p->sources[stream->source].refs != UINT32_MAX ? error_reserve(p) : NULL;
}
static turbowasm_status input_read(void *context, turbowasm_value rep, uint64_t max,
    const uint8_t **data, size_t *size, turbowasm_wasi02_stream_error *error) {
    io_impl *p = context; io_stream *s; io_error *e; turbowasm_status status;
    if (!data || !size || !error || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    s = stream_of(p, rep, TURBOWASM_WASI02_IO_INPUT); e = stream_error_reserve(p, s);
    if (!s || !s->ops.input_read) status = TURBOWASM_TRAPPED;
    else if (!e) status = TURBOWASM_OUT_OF_MEMORY;
    else { status = s->ops.input_read(s->ops.context, s->rep, max, data, size, error);
        if (status == TURBOWASM_OK) status = wrap_error(p, s, error, e); }
    if (e && (status != TURBOWASM_OK || error->kind != TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED)) memset(e, 0, sizeof(*e));
    p->busy = false; return status;
}
static turbowasm_status input_skip(void *context, turbowasm_value rep, uint64_t max,
    uint64_t *skipped, turbowasm_wasi02_stream_error *error) {
    io_impl *p = context; io_stream *s; io_error *e; turbowasm_status status;
    if (!skipped || !error || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    s = stream_of(p, rep, TURBOWASM_WASI02_IO_INPUT); e = stream_error_reserve(p, s);
    if (!s || !s->ops.input_skip) status = TURBOWASM_TRAPPED;
    else if (!e) status = TURBOWASM_OUT_OF_MEMORY;
    else { status = s->ops.input_skip(s->ops.context, s->rep, max, skipped, error);
        if (status == TURBOWASM_OK) status = wrap_error(p, s, error, e); }
    if (e && (status != TURBOWASM_OK || error->kind != TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED)) memset(e, 0, sizeof(*e));
    p->busy = false; return status;
}
static turbowasm_status output_check(void *context, turbowasm_value rep, uint64_t *permit, turbowasm_wasi02_stream_error *error) {
    io_impl *p = context; io_stream *s; io_error *e; turbowasm_status status;
    if (!permit || !error || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    s = stream_of(p, rep, TURBOWASM_WASI02_IO_OUTPUT); e = stream_error_reserve(p, s);
    if (!s || !s->ops.output_check_write) status = TURBOWASM_TRAPPED;
    else if (!e) status = TURBOWASM_OUT_OF_MEMORY;
    else { status = s->ops.output_check_write(s->ops.context, s->rep, permit, error);
        if (status == TURBOWASM_OK) status = wrap_error(p, s, error, e); }
    if (e && (status != TURBOWASM_OK || error->kind != TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED)) memset(e, 0, sizeof(*e));
    p->busy = false; return status;
}
static turbowasm_status output_operation(io_impl *p, turbowasm_value rep, const uint8_t *data,
    size_t size, uint64_t zeroes, unsigned operation, turbowasm_wasi02_stream_error *error) {
    io_stream *s; io_error *e; turbowasm_status status;
    if (!error || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    s = stream_of(p, rep, TURBOWASM_WASI02_IO_OUTPUT); e = stream_error_reserve(p, s);
    if (!s) status = TURBOWASM_TRAPPED;
    else if (!e) status = TURBOWASM_OUT_OF_MEMORY;
    else {
        if (operation == 0 && s->ops.output_write) status = s->ops.output_write(s->ops.context, s->rep, data, size, error);
        else if (operation == 1 && s->ops.output_flush) status = s->ops.output_flush(s->ops.context, s->rep, error);
        else if (operation == 2 && s->ops.output_write_zeroes) status = s->ops.output_write_zeroes(s->ops.context, s->rep, zeroes, error);
        else status = TURBOWASM_UNSUPPORTED;
        if (status == TURBOWASM_OK) status = wrap_error(p, s, error, e);
    }
    if (e && (status != TURBOWASM_OK || error->kind != TURBOWASM_WASI02_STREAM_ERROR_LAST_OPERATION_FAILED)) memset(e, 0, sizeof(*e));
    p->busy = false; return status;
}
static turbowasm_status output_write(void *p, turbowasm_value r, const uint8_t *d, size_t n, turbowasm_wasi02_stream_error *e) {
    return output_operation(p, r, d, n, 0, 0, e);
}
static turbowasm_status output_flush(void *p, turbowasm_value r, turbowasm_wasi02_stream_error *e) {
    return output_operation(p, r, NULL, 0, 0, 1, e);
}
static turbowasm_status output_zeroes(void *p, turbowasm_value r, uint64_t n, turbowasm_wasi02_stream_error *e) {
    return output_operation(p, r, NULL, 0, n, 2, e);
}
static turbowasm_status stream_subscribe(io_impl *p, turbowasm_value rep, unsigned kind, turbowasm_value *out) {
    io_stream *s; turbowasm_status status;
    if (!out || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    s = stream_of(p, rep, (turbowasm_wasi02_io_stream_kind)kind);
    status = s ? pollable_new(p, s->source, out) : TURBOWASM_TRAPPED; p->busy = false; return status;
}
static turbowasm_status input_subscribe(void *p, turbowasm_value r, turbowasm_value *out) { return stream_subscribe(p, r, 1, out); }
static turbowasm_status output_subscribe(void *p, turbowasm_value r, turbowasm_value *out) { return stream_subscribe(p, r, 2, out); }
static void stream_drop(io_impl *p, turbowasm_value rep, unsigned kind) {
    io_stream *s; io_stream saved;
    if (!enter(p)) return;
    s = stream_of(p, rep, (turbowasm_wasi02_io_stream_kind)kind);
    if (s) { saved = *s; memset(s, 0, sizeof(*s));
        if (kind == 1) saved.ops.input_drop(saved.ops.context, saved.rep); else saved.ops.output_drop(saved.ops.context, saved.rep);
        source_release(p, saved.source); }
    p->busy = false;
}
static void input_drop(void *p, turbowasm_value r) { stream_drop(p, r, 1); }
static void output_drop(void *p, turbowasm_value r) { stream_drop(p, r, 2); }
static turbowasm_status error_debug(void *context, turbowasm_value rep, turbowasm_wasi02_string_view *out) {
    io_impl *p = context; uint32_t i; turbowasm_status status = TURBOWASM_TRAPPED;
    if (!out || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    for (i = 0; i < p->config.errors; ++i) if (p->errors[i].token == rep_token(rep) && p->errors[i].token) {
        io_error *e = &p->errors[i]; status = e->ops.error_debug(e->ops.context, e->rep, out); break; }
    p->busy = false; return status;
}
static void error_drop(void *context, turbowasm_value rep) {
    io_impl *p = context; uint32_t i;
    if (!enter(p)) return;
    for (i = 0; i < p->config.errors; ++i) if (p->errors[i].token == rep_token(rep) && p->errors[i].token) {
        io_error saved = p->errors[i]; memset(&p->errors[i], 0, sizeof(saved)); saved.ops.error_drop(saved.ops.context, saved.rep);
        if (saved.retained) source_release(p, saved.source); break; }
    p->busy = false;
}
static turbowasm_status cli_create(io_impl *p, unsigned kind, turbowasm_value *out) {
    turbowasm_status status;
    if (!p || p->busy || p->factory_running || !out || !p->factories[kind]) return TURBOWASM_UNSUPPORTED;
    /* Factories intentionally register a newly owned stream in this domain. */
    p->factory_running = true;
    status = p->factories[kind](p->factory_contexts[kind], out);
    p->factory_running = false;
    if (status == TURBOWASM_OK && !stream_of(p, *out, kind == 0 ? TURBOWASM_WASI02_IO_INPUT : TURBOWASM_WASI02_IO_OUTPUT)) return TURBOWASM_TRAPPED;
    return status;
}
static turbowasm_status cli_stdin(void *p, turbowasm_value *out) { return cli_create(p, 0, out); }
static turbowasm_status cli_stdout(void *p, turbowasm_value *out) { return cli_create(p, 1, out); }
static turbowasm_status cli_stderr(void *p, turbowasm_value *out) { return cli_create(p, 2, out); }
turbowasm_status turbowasm_wasi02_io_cli_factory_set(turbowasm_wasi02_io *io,
    turbowasm_wasi02_io_cli_kind kind, turbowasm_wasi02_stream_factory_fn create, void *context) {
    io_impl *p = impl_of(io);
    if (kind > TURBOWASM_WASI02_IO_STDERR || kind < TURBOWASM_WASI02_IO_STDIN || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    if (p->published) { p->busy = false; return TURBOWASM_INVALID_ARGUMENT; }
    p->factories[kind] = create; p->factory_contexts[kind] = context; p->busy = false; return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi02_io_providers(turbowasm_wasi02_io *io,
    turbowasm_wasi02_stream_provider *streams, turbowasm_wasi02_poll_provider *poll) {
    io_impl *p = impl_of(io);
    if (!streams || !poll || !enter(p)) return TURBOWASM_INVALID_ARGUMENT;
    memset(streams, 0, sizeof(*streams)); memset(poll, 0, sizeof(*poll));
    streams->context = p; streams->get_stdin = cli_stdin; streams->get_stdout = cli_stdout; streams->get_stderr = cli_stderr;
    streams->input_read = input_read; streams->input_skip = input_skip; streams->input_subscribe = input_subscribe;
    streams->output_check_write = output_check; streams->output_write = output_write; streams->output_flush = output_flush;
    streams->output_write_zeroes = output_zeroes; streams->output_subscribe = output_subscribe;
    streams->input_drop = input_drop; streams->output_drop = output_drop; streams->error_debug = error_debug; streams->error_drop = error_drop;
    poll->context = p; poll->ready = poll_ready; poll->drop = poll_drop; poll->arm_routed = poll_arm; poll->arm_many_routed = poll_arm_many;
    p->published = true; p->busy = false; return TURBOWASM_OK;
}

/* Facade private bridge: cleanup runs immediately when a routed Runtime wait
 * returns, including interruption. It removes only membership, never I/O. */
bool turbowasm_wasi02_io_retain_private(turbowasm_wasi02_io *io) {
    io_impl *p = impl_of(io);
    if (!p || p->busy || p->bindings == UINT32_MAX) return false;
    ++p->bindings; return true;
}
void turbowasm_wasi02_io_release_private(void *context) {
    io_impl *p = context; --p->bindings;
}
void *turbowasm_wasi02_io_context_private(turbowasm_wasi02_io *io) { return impl_of(io); }
extern turbowasm_status turbowasm_wasi02_set_wait_cleanup(turbowasm_wasi02 *, void (*)(void *, uintptr_t), void *, void (*)(void *));
turbowasm_status turbowasm_wasi02_io_wasi02_init(turbowasm_wasi02_io *io, turbowasm_wasi02 *wasi,
    const turbowasm_wasi02_config *config, const turbowasm_runtime_config *runtime) {
    turbowasm_wasi02_config copy; turbowasm_status status; io_impl *p = impl_of(io);
    if (p == NULL || config == NULL || config->poll.context || config->poll.ready || config->poll.arm ||
        config->poll.arm_routed || config->poll.arm_many || config->poll.arm_many_routed || config->poll.drop ||
        config->streams.context || config->streams.get_stdin || config->streams.get_stdout || config->streams.get_stderr ||
        config->streams.input_read || config->streams.input_skip || config->streams.input_subscribe ||
        config->streams.output_check_write || config->streams.output_write || config->streams.output_flush ||
        config->streams.output_write_zeroes || config->streams.output_subscribe || config->streams.error_debug ||
        config->streams.input_drop || config->streams.output_drop || config->streams.error_drop ||
        !config->pollable_capacity || !config->stream_resource_capacity) return TURBOWASM_INVALID_ARGUMENT;
    copy = *config; status = turbowasm_wasi02_io_providers(io, &copy.streams, &copy.poll);
    if (status != TURBOWASM_OK) return status;
    if (!turbowasm_wasi02_io_retain_private(io)) return TURBOWASM_INVALID_ARGUMENT;
    status = turbowasm_wasi02_init(wasi, &copy, runtime);
    if (status != TURBOWASM_OK) { turbowasm_wasi02_io_release_private(p); return status; }
    status = turbowasm_wasi02_set_wait_cleanup(wasi, turbowasm_wasi02_io_wait_done, p, turbowasm_wasi02_io_release_private);
    if (status != TURBOWASM_OK) { (void)turbowasm_wasi02_destroy(wasi); turbowasm_wasi02_io_release_private(p); }
    return status;
}
