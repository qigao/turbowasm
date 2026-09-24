#include "../src/jit/mir_backend.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

int main(void) {
    turbowasm_jit_backend backend = {0};
    size_t initial_mapped;

    assert(turbowasm_mir_backend_create(&backend) == TURBOWASM_OK);
    assert(backend.context != NULL);
    assert(backend.is_function_eligible != NULL);
    assert(backend.compile_function != NULL);
    assert(backend.invoke != NULL);
    assert(backend.destroy_function != NULL);
    assert(backend.destroy_backend != NULL);

    assert(!backend.is_function_eligible(
        backend.context, NULL, 0u, NULL));

    assert(turbowasm_mir_backend_code_memory_limit(
               &backend) == 8u * 1024u * 1024u);
    initial_mapped =
        turbowasm_mir_backend_code_memory_used(&backend);
    assert(initial_mapped <=
           turbowasm_mir_backend_code_memory_limit(&backend));

    assert(turbowasm_mir_backend_smoke_constant(
               &backend, INT64_C(42)) == TURBOWASM_OK);
    assert(turbowasm_mir_backend_code_memory_used(
               &backend) >= initial_mapped);
    assert(turbowasm_mir_backend_code_memory_used(
               &backend) <=
           turbowasm_mir_backend_code_memory_limit(&backend));

    backend.destroy_backend(backend.context);
    backend.context = NULL;
    return 0;
}
