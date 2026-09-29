#include <turbowasm/runtime.h>
#include "runtime_alloc.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef union turbowasm_allocation_header {
    max_align_t alignment;
    struct {
        turbowasm_allocator allocator;
        size_t max_allocation_bytes;
        size_t payload_size;
    } metadata;
} turbowasm_allocation_header;

static void *default_allocate(void *c, size_t n) {(void)c; return malloc(n);}
static void *default_reallocate(void *c, void *p, size_t n) {(void)c; return realloc(p,n);}
static void default_deallocate(void *c, void *p) {(void)c; free(p);}

static const turbowasm_runtime_config default_config = {
    {NULL, default_allocate, default_reallocate, default_deallocate},
    {0u,0u,0u,0u}
};
static _Thread_local const turbowasm_runtime_config *current_config = &default_config;

void turbowasm_runtime_config_init(turbowasm_runtime_config *config) {
    if (config != NULL) memset(config,0,sizeof(*config));
}
bool turbowasm_runtime_config_normalize(const turbowasm_runtime_config *config, turbowasm_runtime_config *out) {
    bool a,r,d;
    if (out == NULL) return false;
    if (config == NULL) {*out=default_config; return true;}
    *out=*config;
    a=out->allocator.allocate!=NULL; r=out->allocator.reallocate!=NULL; d=out->allocator.deallocate!=NULL;
    if (!a && !r && !d) {out->allocator=default_config.allocator; return true;}
    return a && d;
}
turbowasm_runtime_scope turbowasm_runtime_scope_enter(const turbowasm_runtime_config *config) {
    turbowasm_runtime_scope s={current_config};
    current_config=config!=NULL?config:&default_config;
    return s;
}
void turbowasm_runtime_scope_leave(turbowasm_runtime_scope scope) {
    current_config=scope.previous!=NULL?scope.previous:&default_config;
}
static void *allocate_with(const turbowasm_allocator *allocator, size_t limit, size_t size) {
    turbowasm_allocation_header *h;
    size_t payload=size==0u?1u:size;
    if (allocator==NULL || allocator->allocate==NULL || allocator->deallocate==NULL) return NULL;
    if (limit!=0u && size>limit) return NULL;
    if (payload>SIZE_MAX-sizeof(*h)) return NULL;
    h=(turbowasm_allocation_header *)allocator->allocate(allocator->context,sizeof(*h)+payload);
    if (h==NULL) return NULL;
    h->metadata.allocator=*allocator; h->metadata.max_allocation_bytes=limit; h->metadata.payload_size=size;
    return (void *)(h+1);
}
void *turbowasm_rt_malloc(size_t size) {
    const turbowasm_runtime_config *c=current_config!=NULL?current_config:&default_config;
    return allocate_with(&c->allocator,c->limits.max_allocation_bytes,size);
}
void *turbowasm_rt_calloc(size_t count,size_t size) {
    size_t n; void *p;
    if (count!=0u && size>SIZE_MAX/count) return NULL;
    n=count*size; p=turbowasm_rt_malloc(n);
    if (p!=NULL && n!=0u) memset(p,0,n);
    return p;
}
void *turbowasm_rt_realloc(void *pointer,size_t size) {
    turbowasm_allocation_header *h,*g; turbowasm_allocator a; size_t limit,old;
    if (pointer==NULL) return turbowasm_rt_malloc(size);
    if (size==0u) {turbowasm_rt_free(pointer); return NULL;}
    h=((turbowasm_allocation_header *)pointer)-1; a=h->metadata.allocator; limit=h->metadata.max_allocation_bytes; old=h->metadata.payload_size;
    if (limit!=0u && size>limit) return NULL;
    if (size>SIZE_MAX-sizeof(*h)) return NULL;
    if (a.reallocate!=NULL) {
        g=(turbowasm_allocation_header *)a.reallocate(a.context,h,sizeof(*h)+size);
        if (g==NULL) return NULL;
        g->metadata.allocator=a; g->metadata.max_allocation_bytes=limit; g->metadata.payload_size=size;
        return (void *)(g+1);
    }
    {
        void *q=allocate_with(&a,limit,size);
        if (q==NULL) return NULL;
        memcpy(q,pointer,old<size?old:size);
        a.deallocate(a.context,h);
        return q;
    }
}
void turbowasm_rt_free(void *pointer) {
    turbowasm_allocation_header *h; turbowasm_allocator a;
    if (pointer==NULL) return;
    h=((turbowasm_allocation_header *)pointer)-1; a=h->metadata.allocator;
    a.deallocate(a.context,h);
}
