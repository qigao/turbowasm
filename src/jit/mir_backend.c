#include "jit/mir_backend.h"

#include <mir-gen.h>
#include <mir.h>

bool turbowasm_mir_backend_smoke(void) {
    MIR_context_t context = MIR_init();

    if (context == NULL)
        return false;

    MIR_gen_init(context);
    MIR_gen_finish(context);
    MIR_finish(context);
    return true;
}
