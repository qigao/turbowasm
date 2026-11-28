#include "database_policy.h"
#include "runtime_alloc.h"
#include <string.h>

bool tw_db_budget_take(size_t *budget, size_t count, size_t size) {
    if (size && count > *budget / size) return false;
    *budget -= count * size;
    return true;
}

static bool add(size_t *total, size_t count) {
    if (count > SIZE_MAX - *total) return false;
    *total += count;
    return true;
}

static bool name_size(const char *s, uint32_t max, size_t *out) {
    if (!s) return false;
    size_t n = 0;
    bool first = true;
    while (n < max && s[n]) {
        unsigned char b = (unsigned char)s[n++];
        if (first) {
            if (b < 'a' || b > 'z') return false;
            first = false;
        } else if (b == '.') first = true;
        else if (!((b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
                   b == '_' || b == '-')) return false;
    }
    if (first || s[n] != 0 || n == SIZE_MAX) return false;
    *out = n + 1;
    return true;
}

void tw_db_policy_destroy(tw_db_owned_policy *p) {
    turbowasm_rt_free(p->names);
    turbowasm_rt_free(p->masks);
    turbowasm_rt_free(p->templates);
    turbowasm_rt_free(p->aliases);
    *p = (tw_db_owned_policy){0};
}

tw_db_status tw_db_policy_copy(tw_db_owned_policy *p,
    const tw_db_policy *s, size_t *budget) {
    const tw_db_limits *l = &s->limits;
    size_t names = 0, templates = 0, masks = 0, n;
    const uint32_t sql_mask = TW_DB_MASK(TW_DB_EXECUTE) | TW_DB_MASK(TW_DB_QUERY);
    const uint32_t isolation_mask = (TW_DB_MASK(TW_DB_ISOLATION_SERIALIZABLE + 1) - 1u);
    const uint32_t redis_mask = (TW_DB_MASK(TW_DB_HSET + 1) - 2u);
    const uint32_t value_mask = (TW_DB_MASK(TW_DB_BLOB + 1) - 1u);
    if (!l->request_bytes || !l->name_bytes || l->name_bytes > l->request_bytes ||
        l->value_bytes > l->request_bytes || (s->alias_count && !s->aliases))
        return TW_DB_INVALID_REQUEST;
    /* Reserve table bytes first, so trusted config traversal is also bounded. */
    if (!tw_db_budget_take(budget,s->alias_count,sizeof(tw_db_alias)))
        return TW_DB_LIMIT;
    for (size_t i=0;i<s->alias_count;++i) {
        const tw_db_alias *a=&s->aliases[i];
        if (!name_size(a->name,l->name_bytes,&n)) return TW_DB_INVALID_REQUEST;
        if (!add(&names,n) || !tw_db_budget_take(budget,n,1)) return TW_DB_LIMIT;
        for (size_t k=0;k<i;++k)
            if (strcmp(a->name,s->aliases[k].name)==0) return TW_DB_INVALID_REQUEST;
        if (a->kind==TW_DB_SQL) {
            if (a->redis_command_mask || (a->isolation_mask & ~isolation_mask) ||
                (a->template_count && !a->templates)) return TW_DB_INVALID_REQUEST;
        } else if (a->kind==TW_DB_REDIS) {
            if (a->isolation_mask || a->template_count ||
                (a->redis_command_mask & ~redis_mask)) return TW_DB_INVALID_REQUEST;
        } else return TW_DB_INVALID_REQUEST;
        if (!add(&templates,a->template_count) ||
            !tw_db_budget_take(budget,a->template_count,sizeof(tw_db_template)))
            return TW_DB_LIMIT;
        for (size_t j=0;j<a->template_count;++j) {
            const tw_db_template *t=&a->templates[j];
            if (!name_size(t->name,l->name_bytes,&n) || !t->opcode_mask ||
                (t->opcode_mask & ~sql_mask) || t->parameter_count>l->parameters ||
                (t->parameter_count && !t->parameter_masks))
                return TW_DB_INVALID_REQUEST;
            for (size_t k=0;k<j;++k)
                if (strcmp(t->name,a->templates[k].name)==0) return TW_DB_INVALID_REQUEST;
            if (!add(&names,n) || !tw_db_budget_take(budget,n,1) ||
                !add(&masks,t->parameter_count) ||
                !tw_db_budget_take(budget,t->parameter_count,sizeof(uint32_t)))
                return TW_DB_LIMIT;
            for (uint32_t k=0;k<t->parameter_count;++k)
                if (!t->parameter_masks[k] || (t->parameter_masks[k] & ~value_mask))
                    return TW_DB_INVALID_REQUEST;
        }
    }
    if (s->alias_count) p->aliases=turbowasm_rt_calloc(s->alias_count,sizeof(*p->aliases));
    if (templates) p->templates=turbowasm_rt_calloc(templates,sizeof(*p->templates));
    if (masks) p->masks=turbowasm_rt_calloc(masks,sizeof(*p->masks));
    if (names) p->names=turbowasm_rt_malloc(names);
    if ((s->alias_count&&!p->aliases)||(templates&&!p->templates)||
        (masks&&!p->masks)||(names&&!p->names)) {
        tw_db_policy_destroy(p);
        return TW_DB_OUT_OF_MEMORY;
    }
    size_t ni=0, ti=0, mi=0;
    for (size_t i=0;i<s->alias_count;++i) {
        const tw_db_alias *a=&s->aliases[i];
        tw_db_alias *d=&p->aliases[i];
        *d=*a;
        n=strlen(a->name)+1; memcpy(p->names+ni,a->name,n);
        d->name=p->names+ni; ni+=n;
        d->templates=a->template_count ? p->templates+ti : NULL;
        for(size_t j=0;j<a->template_count;++j) {
            const tw_db_template *t=&a->templates[j];
            tw_db_template *u=&p->templates[ti++];
            *u=*t;
            n=strlen(t->name)+1; memcpy(p->names+ni,t->name,n);
            u->name=p->names+ni; ni+=n;
            u->parameter_masks=t->parameter_count ? p->masks+mi : NULL;
            if(t->parameter_count) {
                memcpy(p->masks+mi,t->parameter_masks,
                    (size_t)t->parameter_count*sizeof(uint32_t));
                mi+=t->parameter_count;
            }
        }
    }
    p->policy=(tw_db_policy){s->limits,p->aliases,s->alias_count};
    return TW_DB_OK;
}
