#include <turbowasm/wasi02.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void) {
    turbowasm_wasi02 wasi02 = {0};
    turbowasm_wasi02_config config = {0};

    assert(turbowasm_wasi02_init(
               &wasi02,
               &config,
               NULL) == TURBOWASM_OK);
    assert(wasi02.impl != NULL);
    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_OK);
    assert(wasi02.impl == NULL);
    return 0;
}
