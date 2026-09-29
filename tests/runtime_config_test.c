#include <turbowasm/turbowasm.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct probe {size_t allocs,reallocs,frees,live;} probe;
static void *pa(void *c,size_t n){probe *p=c;void *q=malloc(n);if(q){++p->allocs;++p->live;}return q;}
static void *pr(void *c,void *q,size_t n){probe *p=c;void *r=realloc(q,n);if(r)++p->reallocs;return r;}
static void pf(void *c,void *q){probe *p=c;if(q){assert(p->live);--p->live;++p->frees;}free(q);}
static turbowasm_runtime_config pc(probe *p){turbowasm_runtime_config c;turbowasm_runtime_config_init(&c);c.allocator.context=p;c.allocator.allocate=pa;c.allocator.reallocate=pr;c.allocator.deallocate=pf;return c;}

int main(void){
    static const uint8_t empty[]={0,0x61,0x73,0x6d,1,0,0,0};
    static const uint8_t mem[]={0,0x61,0x73,0x6d,1,0,0,0,5,3,1,0,1};
    static const uint8_t tab[]={0,0x61,0x73,0x6d,1,0,0,0,4,4,1,0x70,0,2};
    probe p={0}; turbowasm_runtime_config c=pc(&p); turbowasm_module m={0}; turbowasm_instance i={0}; turbowasm_linker l={0}; size_t calls,live;

    assert(turbowasm_module_load_borrowed_with_config(&m,empty,sizeof(empty),&c)==TURBOWASM_OK);
    calls=p.allocs+p.reallocs;
    assert(turbowasm_instance_create(&i,&m)==TURBOWASM_OK);
    assert(p.allocs+p.reallocs>calls);
    turbowasm_instance_destroy(&i); turbowasm_module_destroy(&m); assert(p.live==0);

    assert(turbowasm_linker_init_with_config(&l,&c)==TURBOWASM_OK); assert(p.live); turbowasm_linker_destroy(&l); assert(p.live==0);

    c.limits.max_module_bytes=sizeof(empty)-1u;
    assert(turbowasm_module_load_borrowed_with_config(&m,empty,sizeof(empty),&c)==TURBOWASM_OUT_OF_MEMORY);
    c.limits.max_module_bytes=0u;
    c.limits.max_allocation_bytes=1u;
    assert(turbowasm_module_load_borrowed_with_config(&m,empty,sizeof(empty),&c)==TURBOWASM_OUT_OF_MEMORY);
    c.limits.max_allocation_bytes=0u;

    c.limits.max_linear_memory_bytes=65535u;
    assert(turbowasm_module_load_borrowed_with_config(&m,mem,sizeof(mem),&c)==TURBOWASM_OK);
    live=p.live; assert(turbowasm_instance_create(&i,&m)==TURBOWASM_OUT_OF_MEMORY); assert(i.impl==NULL); assert(p.live==live);
    turbowasm_module_destroy(&m); assert(p.live==0); c.limits.max_linear_memory_bytes=0u;

    c.limits.max_table_elements=1u;
    assert(turbowasm_module_load_borrowed_with_config(&m,tab,sizeof(tab),&c)==TURBOWASM_OK);
    live=p.live; assert(turbowasm_instance_create(&i,&m)==TURBOWASM_OUT_OF_MEMORY); assert(i.impl==NULL); assert(p.live==live);
    turbowasm_module_destroy(&m); assert(p.live==0); c.limits.max_table_elements=0u;

    c.allocator.allocate=NULL;
    assert(turbowasm_module_load_borrowed_with_config(&m,empty,sizeof(empty),&c)==TURBOWASM_INVALID_ARGUMENT);
    return 0;
}
