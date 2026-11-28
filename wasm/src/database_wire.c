#include "database_wire.h"
#include <string.h>

static bool take(tw_db_reader *r, size_t n, tw_db_bytes *out) {
    if (n > r->remaining) return false;
    *out = (tw_db_bytes){r->data, n};
    if (n) r->data += n;
    r->remaining -= n;
    return true;
}

static bool read_u32(tw_db_reader *r, uint32_t *out) {
    tw_db_bytes b;
    if (!take(r, 4, &b)) return false;
    *out = (uint32_t)b.data[0] | ((uint32_t)b.data[1] << 8) |
        ((uint32_t)b.data[2] << 16) | ((uint32_t)b.data[3] << 24);
    return true;
}

bool tw_db_read_bytes(tw_db_reader *reader, uint32_t max_bytes, tw_db_bytes *out) {
    tw_db_reader r = *reader;
    tw_db_bytes b;
    uint32_t n;
    if (!read_u32(&r, &n) || n > max_bytes || !take(&r, n, &b)) return false;
    *reader = r;
    *out = b;
    return true;
}

/* Strict UTF-8: reject overlong forms, surrogates, out-of-range code points
 * and truncated continuations. Text is counted; U+0000 is not a terminator. */
static bool utf8(tw_db_bytes s) {
    size_t i = 0;
    while (i < s.size) {
        uint8_t a = s.data[i++];
        uint32_t cp, min;
        unsigned n;
        if (a < 0x80) continue;
        if (a >= 0xc2 && a <= 0xdf) { cp = a & 31u; min = 0x80; n = 1; }
        else if (a >= 0xe0 && a <= 0xef) { cp = a & 15u; min = 0x800; n = 2; }
        else if (a >= 0xf0 && a <= 0xf4) { cp = a & 7u; min = 0x10000; n = 3; }
        else return false;
        if (n > s.size - i) return false;
        while (n--) {
            uint8_t b = s.data[i++];
            if ((b & 0xc0u) != 0x80u) return false;
            cp = (cp << 6) | (b & 63u);
        }
        if (cp < min || cp > 0x10ffffu || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}

static bool name_valid(tw_db_bytes s) {
    bool first = true;
    for (size_t i = 0; i < s.size; ++i) {
        uint8_t b = s.data[i];
        if (first) {
            if (b < 'a' || b > 'z') return false;
            first = false;
        } else if (b == '.') {
            first = true;
        } else if (!((b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
                     b == '_' || b == '-')) return false;
    }
    return !first;
}

static bool name_equal(tw_db_bytes b, const char *name) {
    size_t n = strlen(name);
    return b.size == n && memcmp(b.data, name, n) == 0;
}

bool tw_db_read_value(tw_db_reader *reader, uint32_t max_bytes, tw_db_value *out) {
    tw_db_reader r = *reader;
    tw_db_bytes tag, b;
    if (!take(&r, 1, &tag) || !tw_db_read_bytes(&r, max_bytes, &b))
        return false;
    switch (tag.data[0]) {
    case TW_DB_NULL: if (b.size != 0) return false; break;
    case TW_DB_BOOL: if (b.size != 1 || b.data[0] > 1) return false; break;
    case TW_DB_I64: case TW_DB_U64: case TW_DB_F64:
        if (b.size != 8) return false;
        break;
    case TW_DB_TEXT: if (!utf8(b)) return false; break;
    case TW_DB_BLOB: break;
    default: return false;
    }
    *reader = r;
    *out = (tw_db_value){(tw_db_value_kind)tag.data[0], b};
    return true;
}

static tw_db_status sql_request(const tw_db_limits *limits,
    const tw_db_alias *alias, tw_db_reader *r, tw_db_request *q) {
    tw_db_bytes name;
    if (!tw_db_read_bytes(r, limits->name_bytes, &name) || !name_valid(name) ||
        !read_u32(r, &q->argument_count)) return TW_DB_INVALID_REQUEST;
    if (q->argument_count > limits->parameters) return TW_DB_LIMIT;
    const tw_db_template *t = NULL;
    for (size_t i = 0; i < alias->template_count; ++i) {
        if (name_equal(name, alias->templates[i].name)) {
            t = &alias->templates[i];
            q->template_index = i;
            break;
        }
    }
    if (!t || !(t->opcode_mask & TW_DB_MASK(q->opcode))) return TW_DB_DENIED;
    if (q->argument_count != t->parameter_count) return TW_DB_INVALID_REQUEST;
    q->arguments = (tw_db_bytes){r->data, r->remaining};
    for (uint32_t i = 0; i < q->argument_count; ++i) {
        tw_db_value v;
        if (!tw_db_read_value(r, limits->value_bytes, &v) ||
            !(t->parameter_masks[i] & TW_DB_MASK(v.kind)))
            return TW_DB_INVALID_REQUEST;
    }
    return TW_DB_OK;
}

static tw_db_status redis_request(const tw_db_limits *limits,
    const tw_db_alias *alias, tw_db_reader *r, tw_db_request *q) {
    uint32_t expected;
    if (!read_u32(r, &q->command) || !read_u32(r, &q->argument_count))
        return TW_DB_INVALID_REQUEST;
    switch (q->command) {
    case TW_DB_GET: case TW_DB_DEL: case TW_DB_EXISTS: expected = 1; break;
    case TW_DB_SET: case TW_DB_HGET: expected = 2; break;
    case TW_DB_HSET: expected = 3; break;
    default: return TW_DB_UNSUPPORTED;
    }
    if (!(alias->redis_command_mask & TW_DB_MASK(q->command))) return TW_DB_DENIED;
    if (q->argument_count > limits->redis_arguments) return TW_DB_LIMIT;
    if (q->argument_count != expected) return TW_DB_INVALID_REQUEST;
    q->arguments = (tw_db_bytes){r->data, r->remaining};
    for (uint32_t i = 0; i < q->argument_count; ++i) {
        tw_db_bytes b;
        if (!tw_db_read_bytes(r, limits->value_bytes, &b) || (!i && !b.size))
            return TW_DB_INVALID_REQUEST;
    }
    return TW_DB_OK;
}

tw_db_status tw_db_admit(const tw_db_policy *policy, tw_db_owner owner,
    uint32_t opcode, const uint8_t *bytes, size_t size, tw_db_request *out) {
    const tw_db_limits *limits = &policy->limits;
    if (size > limits->request_bytes) return TW_DB_LIMIT;
    if (size && !bytes) return TW_DB_INVALID_REQUEST;
    if (opcode < TW_DB_OPEN_SQL || opcode > TW_DB_CLOSE) return TW_DB_UNSUPPORTED;
    tw_db_reader r = {bytes, size};
    tw_db_request q = {0};
    q.opcode = (tw_db_opcode)opcode;
    tw_db_status status = TW_DB_OK;
    if (opcode == TW_DB_OPEN_SQL || opcode == TW_DB_OPEN_REDIS) {
        if (owner.kind != TW_DB_NONE) return TW_DB_INVALID_HANDLE;
        tw_db_bytes name;
        if (!tw_db_read_bytes(&r, limits->name_bytes, &name) || !name_valid(name))
            return TW_DB_INVALID_REQUEST;
        tw_db_kind kind = opcode == TW_DB_OPEN_SQL ? TW_DB_SQL : TW_DB_REDIS;
        size_t i;
        for (i = 0; i < policy->alias_count; ++i) {
            if (policy->aliases[i].kind == kind &&
                name_equal(name, policy->aliases[i].name)) break;
        }
        if (i == policy->alias_count) return TW_DB_DENIED;
        q.alias_index = i;
    } else {
        if (owner.kind < TW_DB_SQL || owner.kind > TW_DB_CURSOR ||
            owner.alias_index >= policy->alias_count) return TW_DB_INVALID_HANDLE;
        q.alias_index = owner.alias_index;
        const tw_db_alias *a = &policy->aliases[q.alias_index];
        if ((owner.kind == TW_DB_REDIS && a->kind != TW_DB_REDIS) ||
            (owner.kind != TW_DB_REDIS && a->kind != TW_DB_SQL))
            return TW_DB_INVALID_HANDLE;
        switch (opcode) {
        case TW_DB_BEGIN:
            if (owner.kind != TW_DB_SQL) return TW_DB_INVALID_HANDLE;
            if (!read_u32(&r, &q.isolation) || q.isolation > TW_DB_ISOLATION_SERIALIZABLE)
                return TW_DB_INVALID_REQUEST;
            if (!(a->isolation_mask & TW_DB_MASK(q.isolation))) return TW_DB_DENIED;
            break;
        case TW_DB_EXECUTE: case TW_DB_QUERY:
            if (owner.kind != TW_DB_SQL && owner.kind != TW_DB_TRANSACTION)
                return TW_DB_INVALID_HANDLE;
            status = sql_request(limits, a, &r, &q);
            break;
        case TW_DB_FETCH:
            if (owner.kind != TW_DB_CURSOR) return TW_DB_INVALID_HANDLE;
            if (!read_u32(&r, &q.rows) || !q.rows) return TW_DB_INVALID_REQUEST;
            if (q.rows > limits->rows) return TW_DB_LIMIT;
            break;
        case TW_DB_COMMIT: case TW_DB_ROLLBACK:
            if (owner.kind != TW_DB_TRANSACTION) return TW_DB_INVALID_HANDLE;
            break;
        case TW_DB_REDIS_COMMAND:
            if (owner.kind != TW_DB_REDIS) return TW_DB_INVALID_HANDLE;
            status = redis_request(limits, a, &r, &q);
            break;
        case TW_DB_CLOSE: break;
        default: return TW_DB_UNSUPPORTED;
        }
    }
    if (status != TW_DB_OK) return status;
    if (r.remaining != 0) return TW_DB_INVALID_REQUEST;
    *out = q;
    return TW_DB_OK;
}

turbowasm_status tw_db_admit_guest(const tw_db_policy *policy,
    const turbowasm_instance *instance, turbowasm_host_call *call,
    tw_db_owner owner, uint32_t opcode, uint64_t address, uint64_t length,
    uint8_t *scratch, size_t scratch_capacity, tw_db_request *out,
    tw_db_status *status, turbowasm_trap *trap) {
    *trap = TURBOWASM_TRAP_NONE;
    const turbowasm_instance *caller = turbowasm_host_call_instance(call);
    /* The Runtime exposes a call-local facade, not the original C handle.
       Compare the live opaque instance identity, not facade addresses. */
    if (!instance || !instance->impl || !caller || caller->impl != instance->impl) {
        *status = TW_DB_DENIED;
        return TURBOWASM_OK;
    }
    if (length > policy->limits.request_bytes || length > scratch_capacity) {
        *status = TW_DB_LIMIT;
        return TURBOWASM_OK;
    }
    turbowasm_status s = turbowasm_host_call_memory_read64(
        call, 0, address, scratch, (size_t)length, trap);
    if (s != TURBOWASM_OK) return s;
    *status = tw_db_admit(policy, owner, opcode, scratch, (size_t)length, out);
    return TURBOWASM_OK;
}
