#ifndef TURBOWASM_MIR_BACKEND_H
#define TURBOWASM_MIR_BACKEND_H

#include "../jit_backend.h"

turbowasm_status turbowasm_mir_backend_create(
    turbowasm_jit_backend *out_backend);

size_t turbowasm_mir_backend_code_memory_limit(
    const turbowasm_jit_backend *backend);

size_t turbowasm_mir_backend_code_memory_used(
    const turbowasm_jit_backend *backend);

turbowasm_status turbowasm_mir_backend_smoke_constant(
    turbowasm_jit_backend *backend,
    int64_t expected);

#endif /* TURBOWASM_MIR_BACKEND_H */
