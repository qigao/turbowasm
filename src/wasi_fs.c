#include "wasi_fs_private.h"
#include "wasi_provider_private.h"
#include "wasi_dispatch_private.h"

#include <salts/thread.h>
#include <tstr.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbowasm_wasi_fs_slot {
    bool active, reserved, closing, metadata_busy;
    uint32_t pins, leases;
    uint8_t claims, type;
    uint16_t flags;
    turbowasm_wasi_descriptor_ops ops;
    bool preopen;
    uint32_t generation, guest_fd;
    turbowasm_wasi_fs_file file;
    tstr guest_path;
    uint64_t rights_base, rights_inheriting;
} turbowasm_wasi_fs_slot;

typedef struct turbowasm_wasi_fs_impl {
    turbowasm_wasi_fs_provider provider;
    turbowasm_wasi_fs_slot *slots;
    uint32_t capacity, active_count;
    cmeta_mutex_t mutex;
    tw_wasi_dispatch *dispatch;
} turbowasm_wasi_fs_impl;

/* A pin covers one synchronous callback (or preopen copy), not an async
 * provider lease. The slot cannot close/recycle until this pin is released. */
typedef struct tw_fs_pin {
    turbowasm_wasi_fs_slot *slot;
    tw_wasi_fd_lease value;
} tw_fs_pin;

static turbowasm_wasi_fs_impl *tw_fs_impl(const turbowasm_wasi_fs *fs) {
    return fs ? fs->impl : NULL;
}
static turbowasm_status tw_dispatch_attachment(turbowasm_wasi_fs *fs,
    tw_wasi_dispatch *d, bool attach) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !tw_wasi_dispatch_is_owner(d)) return TURBOWASM_INVALID_ARGUMENT;
    cmeta_mutex_lock(&p->mutex);
    bool busy = attach ? p->dispatch != NULL : p->dispatch != d;
    for (uint32_t i = 0; i < p->capacity; ++i) {
        const turbowasm_wasi_fs_slot *s = &p->slots[i];
        busy |= s->pins || s->leases || s->reserved || s->closing || s->metadata_busy ||
            (s->active && s->ops.recv != NULL);
    }
    if (!busy) p->dispatch = attach ? d : NULL;
    cmeta_mutex_unlock(&p->mutex);
    return busy ? TURBOWASM_INVALID_ARGUMENT : TURBOWASM_OK;
}
turbowasm_status tw_wasi_fs_attach_dispatch(turbowasm_wasi_fs *fs, tw_wasi_dispatch *d) {
    return tw_dispatch_attachment(fs, d, true);
}
turbowasm_status tw_wasi_fs_detach_dispatch(turbowasm_wasi_fs *fs, tw_wasi_dispatch *d) {
    return tw_dispatch_attachment(fs, d, false);
}
/* All slot helpers below require the table mutex. */
static turbowasm_wasi_fs_slot *tw_fd(turbowasm_wasi_fs_impl *p, uint32_t fd) {
    for (uint32_t i = 0; i < p->capacity; ++i)
        if (p->slots[i].active && p->slots[i].guest_fd == fd) return &p->slots[i];
    return NULL;
}
static turbowasm_wasi_fs_slot *tw_identity(turbowasm_wasi_fs_impl *p,
    turbowasm_wasi_fs_descriptor d) {
    if (!d.generation || d.slot >= p->capacity) return NULL;
    turbowasm_wasi_fs_slot *s = &p->slots[d.slot];
    return s->generation == d.generation ? s : NULL;
}
static bool tw_fd_occupied(turbowasm_wasi_fs_impl *p, uint32_t fd) {
    for (uint32_t i = 0; i < p->capacity; ++i)
        if ((p->slots[i].active || p->slots[i].reserved) && p->slots[i].guest_fd == fd) return true;
    return false;
}
static turbowasm_wasi_fs_descriptor tw_descriptor(turbowasm_wasi_fs_impl *p,
    const turbowasm_wasi_fs_slot *s) {
    return (turbowasm_wasi_fs_descriptor){(uint32_t)(s - p->slots), s->generation};
}
static void tw_snapshot(turbowasm_wasi_fs_impl *p, const turbowasm_wasi_fs_slot *s,
    tw_wasi_fd_lease *out) {
    *out = (tw_wasi_fd_lease){.descriptor = tw_descriptor(p, s), .file = s->file,
        .ops = s->ops, .rights_base = s->rights_base, .rights_inheriting = s->rights_inheriting,
        .flags = s->flags, .type = s->type, .dispatch = s->ops.recv ? p->dispatch : NULL};
}
static uint32_t tw_pin_locked(turbowasm_wasi_fs_impl *p, turbowasm_wasi_fs_slot *s,
    uint64_t rights, tw_fs_pin *out) {
    if (!s || !s->active) return TURBOWASM_WASI_ERRNO_BADF;
    if (s->closing || s->pins == UINT32_MAX) return TURBOWASM_WASI_ERRNO_BUSY;
    if ((s->rights_base & rights) != rights) return TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    ++s->pins;
    out->slot = s;
    tw_snapshot(p, s, &out->value);
    return 0;
}
static uint32_t tw_pin_fd(turbowasm_wasi_fs_impl *p, uint32_t fd, uint64_t rights, tw_fs_pin *out) {
    if (!p) return TURBOWASM_WASI_ERRNO_BADF;
    cmeta_mutex_lock(&p->mutex);
    uint32_t error = tw_pin_locked(p, tw_fd(p, fd), rights, out);
    cmeta_mutex_unlock(&p->mutex);
    return error;
}
static uint32_t tw_unpin(turbowasm_wasi_fs_impl *p, tw_fs_pin *pin, uint32_t error) {
    if (pin->slot) {
        cmeta_mutex_lock(&p->mutex);
        --pin->slot->pins;
        cmeta_mutex_unlock(&p->mutex);
        pin->slot = NULL;
    }
    return error;
}
static void tw_info(turbowasm_wasi_fs_impl *p, const turbowasm_wasi_fs_slot *s,
    turbowasm_wasi_fs_descriptor_info *out) {
    *out = (turbowasm_wasi_fs_descriptor_info){.descriptor = tw_descriptor(p, s),
        .guest_fd = s->guest_fd, .file = s->file, .preopen = s->preopen,
        .guest_path = s->guest_path, .rights_base = s->rights_base,
        .rights_inheriting = s->rights_inheriting};
}
static turbowasm_wasi_fs_slot *tw_reserve_locked(turbowasm_wasi_fs_impl *p, uint32_t fd) {
    for (uint32_t i = 0; i < p->capacity; ++i) {
        turbowasm_wasi_fs_slot *s = &p->slots[i];
        if (!s->active && !s->reserved && !s->leases && !s->pins && s->generation != UINT32_MAX) {
            s->reserved = true;
            s->guest_fd = fd;
            ++s->generation;
            return s;
        }
    }
    return NULL;
}
static uint32_t tw_next_fd_locked(turbowasm_wasi_fs_impl *p) {
    /* At most capacity fds are occupied, so capacity+1 candidates suffice. */
    for (uint32_t fd = 3; fd <= p->capacity + 3; ++fd)
        if (!tw_fd_occupied(p, fd)) return fd;
    return 0;
}
static void tw_publish_locked(turbowasm_wasi_fs_impl *p, turbowasm_wasi_fs_slot *s,
    turbowasm_wasi_fs_file file, const turbowasm_wasi_descriptor_ops *ops,
    uint8_t type, uint16_t flags, uint64_t base, uint64_t inheriting, tstr path) {
    s->file = file; s->ops = *ops; s->type = type; s->flags = flags;
    s->rights_base = base; s->rights_inheriting = inheriting;
    s->guest_path = path; s->preopen = path != NULL;
    s->claims = 0; s->metadata_busy = false; s->closing = false;
    s->reserved = false; s->active = true;
    ++p->active_count;
}

turbowasm_status turbowasm_wasi_fs_init(turbowasm_wasi_fs *fs, const turbowasm_wasi_fs_config *config) {
    if (!fs || fs->impl || !config || !config->descriptor_capacity ||
        config->descriptor_capacity > UINT32_MAX - 3u ||
        config->descriptor_capacity > SIZE_MAX / sizeof(turbowasm_wasi_fs_slot) ||
        !config->provider.close || !config->provider.read || !config->provider.write)
        return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_wasi_fs_impl *p = calloc(1, sizeof(*p));
    if (!p) return TURBOWASM_OUT_OF_MEMORY;
    p->slots = calloc(config->descriptor_capacity, sizeof(*p->slots));
    if (!p->slots) { free(p); return TURBOWASM_OUT_OF_MEMORY; }
    cmeta_mutex_init(&p->mutex);
    if (!p->mutex) { free(p->slots); free(p); return TURBOWASM_OUT_OF_MEMORY; }
    p->provider = config->provider;
    p->capacity = (uint32_t)config->descriptor_capacity;
    fs->impl = p;
    return TURBOWASM_OK;
}
turbowasm_status turbowasm_wasi_fs_destroy(turbowasm_wasi_fs *fs) {
    if (!fs) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p) return TURBOWASM_OK;
    cmeta_mutex_lock(&p->mutex);
    bool busy = p->active_count != 0 || p->dispatch != NULL;
    for (uint32_t i = 0; i < p->capacity; ++i)
        busy |= p->slots[i].leases || p->slots[i].pins || p->slots[i].reserved;
    cmeta_mutex_unlock(&p->mutex);
    if (busy) return TURBOWASM_INVALID_ARGUMENT;
    cmeta_mutex_destroy(&p->mutex);
    free(p->slots); free(p); fs->impl = NULL;
    return TURBOWASM_OK;
}
static turbowasm_status tw_bind(turbowasm_wasi_fs *fs, uint32_t fd, bool next,
    turbowasm_wasi_fs_file file, bool preopen, const char *path, uint64_t base, uint64_t inheriting,
    const turbowasm_wasi_descriptor_ops *ops, uint8_t type, uint16_t flags,
    turbowasm_wasi_fs_descriptor *out, uint32_t *out_fd) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !out || !file.generation || (preopen && (!path || !path[0])) || (!preopen && path))
        return TURBOWASM_INVALID_ARGUMENT;
    *out = (turbowasm_wasi_fs_descriptor){0};
    if (out_fd) *out_fd = 0;
    tstr copy = preopen ? tstr_new_len(path, strlen(path)) : NULL;
    if (preopen && !copy) return TURBOWASM_OUT_OF_MEMORY;
    turbowasm_status status = TURBOWASM_OK;
    cmeta_mutex_lock(&p->mutex);
    if (ops && p->dispatch && !tw_wasi_dispatch_is_owner(p->dispatch)) {
        cmeta_mutex_unlock(&p->mutex); tstr_free(copy); return TURBOWASM_INVALID_ARGUMENT;
    }
    if (next) fd = tw_next_fd_locked(p);
    if (tw_fd_occupied(p, fd)) status = TURBOWASM_INVALID_ARGUMENT;
    else {
        turbowasm_wasi_fs_slot *s = tw_reserve_locked(p, fd);
        if (!s) status = TURBOWASM_OUT_OF_MEMORY;
        else {
            turbowasm_wasi_descriptor_ops provider = {.size = sizeof(provider), .api_version = 1, .file = p->provider};
            tw_publish_locked(p, s, file, ops ? ops : &provider, type, flags, base, inheriting, copy);
            copy = NULL;
            *out = tw_descriptor(p, s);
            if (out_fd) *out_fd = fd;
        }
    }
    cmeta_mutex_unlock(&p->mutex);
    tstr_free(copy);
    return status;
}
turbowasm_status turbowasm_wasi_fs_bind_descriptor(turbowasm_wasi_fs *fs, uint32_t fd,
    turbowasm_wasi_fs_file file, bool preopen, const char *path, turbowasm_wasi_fs_descriptor *out) {
    return turbowasm_wasi_fs_bind_descriptor_with_rights(fs, fd, file, preopen, path,
        UINT64_MAX, UINT64_MAX, out);
}
turbowasm_status turbowasm_wasi_fs_bind_descriptor_with_rights(turbowasm_wasi_fs *fs, uint32_t fd,
    turbowasm_wasi_fs_file file, bool preopen, const char *path, uint64_t base, uint64_t inheriting,
    turbowasm_wasi_fs_descriptor *out) {
    return tw_bind(fs, fd, false, file, preopen, path, base, inheriting, NULL,
        preopen ? TURBOWASM_WASI_FILETYPE_DIRECTORY : TURBOWASM_WASI_FILETYPE_UNKNOWN, 0, out, NULL);
}
turbowasm_status turbowasm_wasi_fs_bind_next_descriptor(turbowasm_wasi_fs *fs,
    turbowasm_wasi_fs_file file, uint64_t base, uint64_t inheriting,
    turbowasm_wasi_fs_descriptor *out, uint32_t *out_fd) {
    if (!out_fd) return TURBOWASM_INVALID_ARGUMENT;
    return tw_bind(fs, 0, true, file, false, NULL, base, inheriting, NULL,
        TURBOWASM_WASI_FILETYPE_UNKNOWN, 0, out, out_fd);
}
bool turbowasm_wasi_fs_descriptor_info_get(const turbowasm_wasi_fs *fs, uint32_t fd,
    turbowasm_wasi_fs_descriptor_info *out) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !out) return false;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_fd(p, fd);
    if (s) tw_info(p, s, out);
    cmeta_mutex_unlock(&p->mutex);
    return s != NULL;
}
bool turbowasm_wasi_fs_retained_descriptor_info_get(const turbowasm_wasi_fs *fs,
    turbowasm_wasi_fs_descriptor d, turbowasm_wasi_fs_descriptor_info *out) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !out) return false;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_identity(p, d);
    bool valid = s && s->active;
    if (valid) tw_info(p, s, out);
    cmeta_mutex_unlock(&p->mutex);
    return valid;
}
size_t turbowasm_wasi_fs_preopen_count(const turbowasm_wasi_fs *fs) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p) return 0;
    size_t count = 0;
    cmeta_mutex_lock(&p->mutex);
    for (uint32_t i = 0; i < p->capacity; ++i) count += p->slots[i].active && p->slots[i].preopen;
    cmeta_mutex_unlock(&p->mutex);
    return count;
}
bool turbowasm_wasi_fs_preopen_at(const turbowasm_wasi_fs *fs, size_t index,
    turbowasm_wasi_fs_descriptor_info *out) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !out) return false;
    bool found = false;
    cmeta_mutex_lock(&p->mutex);
    for (uint32_t i = 0; i < p->capacity; ++i) {
        turbowasm_wasi_fs_slot *s = &p->slots[i];
        if (s->active && s->preopen && index-- == 0) { tw_info(p, s, out); found = true; break; }
    }
    cmeta_mutex_unlock(&p->mutex);
    return found;
}
uint32_t tw_wasi_fd_preopen_pin(turbowasm_wasi_fs *fs, uint32_t fd,
    turbowasm_wasi_fs_descriptor_info *out) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !out) return TURBOWASM_WASI_ERRNO_INVAL;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_fd(p, fd);
    tw_fs_pin pin = {0};
    uint32_t error = !s ? TURBOWASM_WASI_ERRNO_BADF :
        !s->preopen ? TURBOWASM_WASI_ERRNO_NOTCAPABLE : tw_pin_locked(p, s, 0, &pin);
    if (!error) tw_info(p, s, out);
    cmeta_mutex_unlock(&p->mutex);
    return error;
}
void tw_wasi_fd_preopen_unpin(turbowasm_wasi_fs *fs, turbowasm_wasi_fs_descriptor d) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p) return;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_identity(p, d);
    if (s && s->pins) --s->pins;
    cmeta_mutex_unlock(&p->mutex);
}
static uint32_t tw_close(turbowasm_wasi_fs_impl *p, bool by_fd, uint32_t fd,
    turbowasm_wasi_fs_descriptor d) {
    if (!p) return TURBOWASM_WASI_ERRNO_BADF;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = by_fd ? tw_fd(p, fd) : tw_identity(p, d);
    if (!s || !s->active) { cmeta_mutex_unlock(&p->mutex); return TURBOWASM_WASI_ERRNO_BADF; }
    if (s->closing || s->pins || s->metadata_busy) {
        cmeta_mutex_unlock(&p->mutex); return TURBOWASM_WASI_ERRNO_BUSY;
    }
    s->closing = true;
    tw_wasi_fd_lease value;
    tw_snapshot(p, s, &value);
    cmeta_mutex_unlock(&p->mutex);
    uint32_t error = tw_wasi_provider_call(&value,
        &(tw_wasi_provider_request){.operation = TW_PROVIDER_CLOSE});
    tstr path = NULL;
    cmeta_mutex_lock(&p->mutex);
    s->closing = false;
    if (!error) {
        path = s->guest_path; s->guest_path = NULL;
        s->active = false; s->preopen = false; s->guest_fd = 0;
        s->rights_base = s->rights_inheriting = 0;
        if (!s->leases) s->file = (turbowasm_wasi_fs_file){0};
        --p->active_count;
    }
    cmeta_mutex_unlock(&p->mutex);
    tstr_free(path);
    return error;
}
uint32_t turbowasm_wasi_fs_close_descriptor(turbowasm_wasi_fs *fs, turbowasm_wasi_fs_descriptor d) {
    return tw_close(tw_fs_impl(fs), false, 0, d);
}
uint32_t turbowasm_wasi_fs_close_fd(turbowasm_wasi_fs *fs, uint32_t fd) {
    return tw_close(tw_fs_impl(fs), true, fd, (turbowasm_wasi_fs_descriptor){0});
}
uint32_t turbowasm_wasi_fs_path_open(turbowasm_wasi_fs *fs, uint32_t fd, uint32_t dirflags,
    const uint8_t *path, size_t length, uint32_t oflags, uint64_t base, uint64_t inheriting,
    uint32_t flags, uint32_t *out_fd) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !out_fd || (length && !path)) return TURBOWASM_WASI_ERRNO_INVAL;
    *out_fd = 0;
    tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, TURBOWASM_WASI_RIGHT_PATH_OPEN, &pin);
    if (error) return error;
    if ((base & ~pin.value.rights_inheriting) || (inheriting & ~pin.value.rights_inheriting))
        return tw_unpin(p, &pin, TURBOWASM_WASI_ERRNO_NOTCAPABLE);
    if (!pin.value.ops.file.path_open) return tw_unpin(p, &pin, TURBOWASM_WASI_ERRNO_NOSYS);
    cmeta_mutex_lock(&p->mutex);
    uint32_t child_fd = tw_next_fd_locked(p);
    turbowasm_wasi_fs_slot *child = tw_reserve_locked(p, child_fd);
    cmeta_mutex_unlock(&p->mutex);
    if (!child) return tw_unpin(p, &pin, TURBOWASM_WASI_ERRNO_MFILE);
    turbowasm_wasi_fs_file opened = {0};
    error = tw_wasi_provider_call(&pin.value, &(tw_wasi_provider_request){.operation = TW_PROVIDER_PATH_OPEN,
        .as.open = {dirflags, path, length, oflags, base, inheriting, flags, &opened}});
    /* Successful providers transfer a valid identity. A zero generation is a
     * provider contract violation, not a resource that can safely be closed. */
    if (!error && !opened.generation) error = TURBOWASM_WASI_ERRNO_IO;
    cmeta_mutex_lock(&p->mutex);
    if (error) { child->reserved = false; child->guest_fd = 0; }
    else {
        uint64_t current = pin.slot->rights_inheriting;
        tw_publish_locked(p, child, opened, &pin.value.ops, TURBOWASM_WASI_FILETYPE_UNKNOWN,
            (uint16_t)flags, base & current, inheriting & current, NULL);
        *out_fd = child_fd;
    }
    --pin.slot->pins;
    cmeta_mutex_unlock(&p->mutex);
    return error;
}
uint32_t turbowasm_wasi_fs_fd_read(void *context, uint32_t fd,
    const turbowasm_wasi_buffer *buffers, size_t count, uint32_t *out) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(context); tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, TURBOWASM_WASI_RIGHT_FD_READ, &pin);
    if (error) return error;
    error = tw_wasi_provider_call(&pin.value, &(tw_wasi_provider_request){.operation = TW_PROVIDER_READ,
        .as.read = {buffers, count, out}});
    return tw_unpin(p, &pin, error);
}
uint32_t turbowasm_wasi_fs_fd_write(void *context, uint32_t fd,
    const turbowasm_wasi_const_buffer *buffers, size_t count, uint32_t *out) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(context); tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, TURBOWASM_WASI_RIGHT_FD_WRITE, &pin);
    if (error) return error;
    error = tw_wasi_provider_call(&pin.value, &(tw_wasi_provider_request){.operation = TW_PROVIDER_WRITE,
        .as.write = {buffers, count, out}});
    return tw_unpin(p, &pin, error);
}
uint32_t turbowasm_wasi_fs_fd_seek(turbowasm_wasi_fs *fs, uint32_t fd,
    int64_t offset, uint8_t whence, uint64_t *out) {
    if (!out) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = 0;
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs); tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, TURBOWASM_WASI_RIGHT_FD_SEEK, &pin);
    if (error) return error;
    if (whence > TURBOWASM_WASI_WHENCE_END) error = TURBOWASM_WASI_ERRNO_INVAL;
    else if (!pin.value.ops.file.seek) error = TURBOWASM_WASI_ERRNO_NOSYS;
    else error = tw_wasi_provider_call(&pin.value, &(tw_wasi_provider_request){.operation = TW_PROVIDER_SEEK,
        .as.seek = {offset, whence, out}});
    return tw_unpin(p, &pin, error);
}
uint32_t turbowasm_wasi_fs_fd_tell(turbowasm_wasi_fs *fs, uint32_t fd, uint64_t *out) {
    if (!out) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = 0;
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs); tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, 0, &pin);
    if (error) return error;
    if (!(pin.value.rights_base & (TURBOWASM_WASI_RIGHT_FD_TELL | TURBOWASM_WASI_RIGHT_FD_SEEK)))
        error = TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    else if (!pin.value.ops.file.tell) error = TURBOWASM_WASI_ERRNO_NOSYS;
    else error = tw_wasi_provider_call(&pin.value,
        &(tw_wasi_provider_request){.operation = TW_PROVIDER_TELL, .as.tell = out});
    return tw_unpin(p, &pin, error);
}
uint32_t turbowasm_wasi_fs_fd_stat(turbowasm_wasi_fs *fs, uint32_t fd, turbowasm_wasi_fs_stat *out) {
    if (!out) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = (turbowasm_wasi_fs_stat){0};
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs); tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET, &pin);
    if (error) return error;
    error = pin.value.ops.file.stat ? tw_wasi_provider_call(&pin.value,
        &(tw_wasi_provider_request){.operation = TW_PROVIDER_STAT, .as.stat = out}) :
        TURBOWASM_WASI_ERRNO_NOSYS;
    return tw_unpin(p, &pin, error);
}
uint32_t turbowasm_wasi_fs_path_stat(turbowasm_wasi_fs *fs, uint32_t fd, uint32_t flags,
    const uint8_t *path, size_t length, turbowasm_wasi_fs_stat *out) {
    if (!out || (length && !path)) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = (turbowasm_wasi_fs_stat){0};
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs); tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, TURBOWASM_WASI_RIGHT_PATH_FILESTAT_GET, &pin);
    if (error) return error;
    error = pin.value.ops.file.path_stat ?
        tw_wasi_provider_call(&pin.value, &(tw_wasi_provider_request){.operation = TW_PROVIDER_PATH_STAT,
            .as.path_stat = {flags, path, length, out}}) :
        TURBOWASM_WASI_ERRNO_NOSYS;
    return tw_unpin(p, &pin, error);
}
static uint32_t tw_path_mutate(turbowasm_wasi_fs *fs, uint32_t fd, const uint8_t *path,
    size_t length, uint64_t rights, unsigned operation) {
    if (length && !path) return TURBOWASM_WASI_ERRNO_INVAL;
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs); tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, rights, &pin);
    if (error) return error;
    turbowasm_wasi_fs_path_mutation_fn fn = operation == 0 ? pin.value.ops.file.path_create_directory :
        operation == 1 ? pin.value.ops.file.path_remove_directory : pin.value.ops.file.path_unlink_file;
    error = fn ? tw_wasi_provider_call(&pin.value, &(tw_wasi_provider_request){.operation = TW_PROVIDER_PATH_MUTATE,
        .as.mutate = {fn, path, length}}) : TURBOWASM_WASI_ERRNO_NOSYS;
    return tw_unpin(p, &pin, error);
}
uint32_t turbowasm_wasi_fs_path_create_directory(turbowasm_wasi_fs *fs, uint32_t fd,
    const uint8_t *path, size_t length) {
    return tw_path_mutate(fs, fd, path, length, TURBOWASM_WASI_RIGHT_PATH_CREATE_DIRECTORY, 0);
}
uint32_t turbowasm_wasi_fs_path_remove_directory(turbowasm_wasi_fs *fs, uint32_t fd,
    const uint8_t *path, size_t length) {
    return tw_path_mutate(fs, fd, path, length, TURBOWASM_WASI_RIGHT_PATH_REMOVE_DIRECTORY, 1);
}
uint32_t turbowasm_wasi_fs_path_unlink_file(turbowasm_wasi_fs *fs, uint32_t fd,
    const uint8_t *path, size_t length) {
    return tw_path_mutate(fs, fd, path, length, TURBOWASM_WASI_RIGHT_PATH_UNLINK_FILE, 2);
}
uint32_t turbowasm_wasi_fs_fd_readdir(turbowasm_wasi_fs *fs, uint32_t fd, uint64_t cookie,
    turbowasm_wasi_fs_dirent *out, bool *has_entry) {
    if (!out || !has_entry) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = (turbowasm_wasi_fs_dirent){0}; *has_entry = false;
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs); tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, TURBOWASM_WASI_RIGHT_FD_READDIR, &pin);
    if (error) return error;
    error = pin.value.ops.file.readdir ?
        tw_wasi_provider_call(&pin.value, &(tw_wasi_provider_request){.operation = TW_PROVIDER_READDIR,
            .as.readdir = {cookie, out, has_entry}}) :
        TURBOWASM_WASI_ERRNO_NOSYS;
    tw_unpin(p, &pin, error);
    if (error || !*has_entry) return error;
    if (out->name_length > TURBOWASM_WASI_FS_DIRENT_NAME_MAX) return TURBOWASM_WASI_ERRNO_NAMETOOLONG;
    if (out->file_type > TURBOWASM_WASI_FILETYPE_SYMBOLIC_LINK || out->next_cookie == cookie)
        return TURBOWASM_WASI_ERRNO_INVAL;
    return 0;
}
uint32_t turbowasm_wasi_fs_path_rename(turbowasm_wasi_fs *fs,
    uint32_t source_fd, const uint8_t *source, size_t source_length,
    uint32_t target_fd, const uint8_t *target, size_t target_length) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !source || !target || !source_length || !target_length) return TURBOWASM_WASI_ERRNO_INVAL;
    tw_fs_pin a = {0}, b = {0};
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *as = tw_fd(p, source_fd), *bs = tw_fd(p, target_fd);
    uint32_t error = !as || !bs ? TURBOWASM_WASI_ERRNO_BADF :
        tw_pin_locked(p, as, TURBOWASM_WASI_RIGHT_PATH_RENAME_SOURCE, &a);
    if (!error) error = tw_pin_locked(p, bs, TURBOWASM_WASI_RIGHT_PATH_RENAME_TARGET, &b);
    cmeta_mutex_unlock(&p->mutex);
    if (!error) {
        if (a.value.ops.file.context != b.value.ops.file.context ||
            a.value.ops.file.path_rename != b.value.ops.file.path_rename) error = TURBOWASM_WASI_ERRNO_XDEV;
        else if (!a.value.ops.file.path_rename) error = TURBOWASM_WASI_ERRNO_NOTSUP;
        else {
            /* A socket on either side makes this an owner operation. Provider
             * context equality above still compares the original contexts. */
            if (!a.value.dispatch) a.value.dispatch = b.value.dispatch;
            error = tw_wasi_provider_call(&a.value, &(tw_wasi_provider_request){.operation = TW_PROVIDER_RENAME,
                .as.rename = {source, source_length, b.value.file, target, target_length}});
        }
    }
    tw_unpin(p, &b, error);
    return tw_unpin(p, &a, error);
}
bool tw_wasi_fd_is_socket(turbowasm_wasi_fs *fs, uint32_t fd) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p) return false;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_fd(p, fd);
    bool socket = s && s->ops.recv != NULL;
    cmeta_mutex_unlock(&p->mutex);
    return socket;
}
uint32_t tw_wasi_fd_acquire(turbowasm_wasi_fs *fs, uint32_t fd, uint64_t rights, tw_wasi_fd_lease *out) {
    if (!out) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = (tw_wasi_fd_lease){0};
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs); tw_fs_pin pin = {0};
    uint32_t error = tw_pin_fd(p, fd, rights, &pin);
    if (error) return error;
    cmeta_mutex_lock(&p->mutex);
    if (pin.slot->leases == UINT32_MAX) error = TURBOWASM_WASI_ERRNO_BUSY;
    else ++pin.slot->leases;
    cmeta_mutex_unlock(&p->mutex);
    if (error) return tw_unpin(p, &pin, error);
    uint8_t type = pin.value.type;
    if (!type && pin.value.ops.file.stat) {
        turbowasm_wasi_fs_stat stat = {0};
        error = tw_wasi_provider_call(&pin.value,
            &(tw_wasi_provider_request){.operation = TW_PROVIDER_STAT, .as.stat = &stat});
        type = stat.file_type;
    }
    if (!error && pin.value.ops.retain) error = tw_wasi_provider_call(&pin.value,
        &(tw_wasi_provider_request){.operation = TW_PROVIDER_RETAIN});
    cmeta_mutex_lock(&p->mutex);
    if (error) --pin.slot->leases;
    else { *out = pin.value; out->type = type; out->held = true; }
    --pin.slot->pins;
    cmeta_mutex_unlock(&p->mutex);
    return error;
}
void tw_wasi_fd_release(turbowasm_wasi_fs *fs, tw_wasi_fd_lease *lease) {
    if (!lease || !lease->held) return;
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p) return;
    if ((lease->claimed && lease->ops.finish) || lease->ops.release)
        (void)tw_wasi_provider_call(lease, &(tw_wasi_provider_request){.operation = TW_PROVIDER_CLEANUP});
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_identity(p, lease->descriptor);
    if (s) {
        if (lease->claimed) s->claims &= (uint8_t)~(1u << lease->direction);
        --s->leases;
        if (!s->active && !s->leases) s->file = (turbowasm_wasi_fs_file){0};
    }
    cmeta_mutex_unlock(&p->mutex);
    *lease = (tw_wasi_fd_lease){0};
}
uint32_t tw_wasi_fd_check(turbowasm_wasi_fs *fs, const tw_wasi_fd_lease *lease, uint64_t rights) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !lease || !lease->held) return TURBOWASM_WASI_ERRNO_BADF;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_identity(p, lease->descriptor);
    uint32_t error = !s || !s->active ? TURBOWASM_WASI_ERRNO_BADF : s->closing ? TURBOWASM_WASI_ERRNO_BUSY :
        (s->rights_base & rights) == rights ? 0 : TURBOWASM_WASI_ERRNO_NOTCAPABLE;
    cmeta_mutex_unlock(&p->mutex);
    return error;
}
uint32_t tw_wasi_fd_claim(turbowasm_wasi_fs *fs, tw_wasi_fd_lease *lease, uint8_t direction) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !lease || !lease->held || direction > 2) return TURBOWASM_WASI_ERRNO_INVAL;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_identity(p, lease->descriptor);
    uint8_t bit = (uint8_t)(1u << direction);
    uint32_t error = !s || !s->active ? TURBOWASM_WASI_ERRNO_BADF :
        s->closing || lease->claimed || (s->claims & bit) ? TURBOWASM_WASI_ERRNO_BUSY : 0;
    if (!error) { s->claims |= bit; lease->claimed = true; lease->direction = direction; }
    cmeta_mutex_unlock(&p->mutex);
    return error;
}
uint32_t tw_wasi_fd_set_flags(turbowasm_wasi_fs *fs, uint32_t fd, uint16_t flags) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs); tw_fs_pin pin = {0};
    if (!p) return TURBOWASM_WASI_ERRNO_BADF;
    cmeta_mutex_lock(&p->mutex);
    uint32_t error = tw_pin_locked(p, tw_fd(p, fd), TURBOWASM_WASI_RIGHT_FD_FDSTAT_SET_FLAGS, &pin);
    if (!error) {
        if (pin.slot->metadata_busy) { --pin.slot->pins; pin.slot = NULL; error = TURBOWASM_WASI_ERRNO_BUSY; }
        else pin.slot->metadata_busy = true;
    }
    cmeta_mutex_unlock(&p->mutex);
    if (error) return error;
    if (pin.value.ops.recv) {
        if (flags & ~TURBOWASM_WASI_FDFLAG_NONBLOCK) error = TURBOWASM_WASI_ERRNO_NOTSUP;
    } else if (pin.value.ops.file.set_flags) error = tw_wasi_provider_call(&pin.value,
        &(tw_wasi_provider_request){.operation = TW_PROVIDER_FLAGS, .as.flags = flags});
    else if (flags != pin.value.flags) error = TURBOWASM_WASI_ERRNO_NOTSUP;
    cmeta_mutex_lock(&p->mutex);
    if (!error) pin.slot->flags = flags;
    pin.slot->metadata_busy = false;
    --pin.slot->pins;
    cmeta_mutex_unlock(&p->mutex);
    return error;
}
uint32_t tw_wasi_fd_set_rights(turbowasm_wasi_fs *fs, uint32_t fd, uint64_t base, uint64_t inheriting) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p) return TURBOWASM_WASI_ERRNO_BADF;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_fd(p, fd);
    uint32_t error = !s ? TURBOWASM_WASI_ERRNO_BADF : s->closing ? TURBOWASM_WASI_ERRNO_BUSY :
        ((base & ~s->rights_base) || (inheriting & ~s->rights_inheriting)) ? TURBOWASM_WASI_ERRNO_NOTCAPABLE : 0;
    if (!error) { s->rights_base = base; s->rights_inheriting = inheriting; }
    cmeta_mutex_unlock(&p->mutex);
    return error;
}
uint32_t tw_wasi_fd_reserve(turbowasm_wasi_fs *fs, turbowasm_wasi_fs_descriptor *out, uint32_t *fd) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p || !out || !fd) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = (turbowasm_wasi_fs_descriptor){0}; *fd = 0;
    cmeta_mutex_lock(&p->mutex);
    uint32_t candidate = tw_next_fd_locked(p);
    turbowasm_wasi_fs_slot *s = tw_reserve_locked(p, candidate);
    if (s) { *out = tw_descriptor(p, s); *fd = candidate; }
    cmeta_mutex_unlock(&p->mutex);
    return s ? 0 : TURBOWASM_WASI_ERRNO_MFILE;
}
void tw_wasi_fd_abort(turbowasm_wasi_fs *fs, turbowasm_wasi_fs_descriptor d) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    if (!p) return;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_identity(p, d);
    if (s && s->reserved) { s->reserved = false; s->guest_fd = 0; }
    cmeta_mutex_unlock(&p->mutex);
}
void tw_wasi_fd_publish(turbowasm_wasi_fs *fs, turbowasm_wasi_fs_descriptor d,
    turbowasm_wasi_fs_file file, const tw_wasi_fd_lease *parent, uint16_t flags) {
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs);
    uint64_t rights = TURBOWASM_WASI_RIGHT_FD_READ | TURBOWASM_WASI_RIGHT_FD_WRITE |
        TURBOWASM_WASI_RIGHT_FD_FDSTAT_SET_FLAGS | TURBOWASM_WASI_RIGHT_FD_FILESTAT_GET |
        TURBOWASM_WASI_RIGHT_POLL_FD_READWRITE | TURBOWASM_WASI_RIGHT_SOCK_SHUTDOWN;
    cmeta_mutex_lock(&p->mutex);
    turbowasm_wasi_fs_slot *s = tw_identity(p, d), *current = tw_identity(p, parent->descriptor);
    /* The caller owns a live reservation and parent lease through publication. */
    tw_publish_locked(p, s, file, &parent->ops, TURBOWASM_WASI_FILETYPE_SOCKET_STREAM,
        flags, current->rights_inheriting & rights, 0, NULL);
    cmeta_mutex_unlock(&p->mutex);
}
uint32_t tw_wasi_fd_ready(turbowasm_wasi_fs *fs, const tw_wasi_fd_lease *lease, uint8_t direction,
    turbowasm_wasi_readiness *out) {
    if (!out) return TURBOWASM_WASI_ERRNO_INVAL;
    *out = (turbowasm_wasi_readiness){0};
    turbowasm_wasi_fs_impl *p = tw_fs_impl(fs); tw_fs_pin pin = {0};
    if (!p || !lease || !lease->held) return TURBOWASM_WASI_ERRNO_BADF;
    cmeta_mutex_lock(&p->mutex);
    uint32_t error = tw_pin_locked(p, tw_identity(p, lease->descriptor), TURBOWASM_WASI_RIGHT_POLL_FD_READWRITE, &pin);
    cmeta_mutex_unlock(&p->mutex);
    if (error) return error;
    if (lease->ops.ready) error = tw_wasi_provider_call(lease,
        &(tw_wasi_provider_request){.operation = TW_PROVIDER_READY, .as.ready = {direction, out}});
    else if (lease->type == TURBOWASM_WASI_FILETYPE_REGULAR_FILE || lease->type == TURBOWASM_WASI_FILETYPE_DIRECTORY) {
        out->ready = true;
        if (direction == TURBOWASM_WASI_EVENT_FD_READ && lease->ops.file.stat) {
            turbowasm_wasi_fs_stat stat = {0};
            error = tw_wasi_provider_call(lease,
                &(tw_wasi_provider_request){.operation = TW_PROVIDER_STAT, .as.stat = &stat});
            uint64_t offset = 0;
            if (!error && lease->ops.file.tell) error = tw_wasi_provider_call(lease,
                &(tw_wasi_provider_request){.operation = TW_PROVIDER_TELL, .as.tell = &offset});
            if (!error) out->bytes = stat.size > offset ? stat.size - offset : 0;
        }
    } else error = TURBOWASM_WASI_ERRNO_NOTSUP;
    return tw_unpin(p, &pin, error);
}
turbowasm_status turbowasm_wasi_fs_bind_socket_move(turbowasm_wasi_fs *fs, uint32_t fd,
    const turbowasm_wasi_descriptor_ops *ops, turbowasm_wasi_fs_file *file,
    uint8_t type, uint16_t flags, uint64_t base, uint64_t inheriting, turbowasm_wasi_fs_descriptor *out) {
    if (!ops || ops->size != sizeof(*ops) || ops->api_version != 1 || !file || !file->generation ||
        !ops->file.close || !ops->file.read || !ops->file.write || !ops->retain || !ops->release ||
        !ops->ready || !ops->recv || !ops->send || !ops->shutdown ||
        (type != TURBOWASM_WASI_FILETYPE_SOCKET_STREAM && type != TURBOWASM_WASI_FILETYPE_SOCKET_DGRAM) ||
        (flags & ~TURBOWASM_WASI_FDFLAG_NONBLOCK)) return TURBOWASM_INVALID_ARGUMENT;
    turbowasm_status status = tw_bind(fs, fd, false, *file, false, NULL, base, inheriting, ops, type, flags, out, NULL);
    if (status == TURBOWASM_OK) *file = (turbowasm_wasi_fs_file){0};
    return status;
}
