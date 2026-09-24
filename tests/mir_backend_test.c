#include "jit/mir_backend.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void) {
    assert(turbowasm_mir_backend_smoke());
    return 0;
}
