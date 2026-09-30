#include <turbowasm/wasi02.h>

#include <cassert>

int main() {
    turbowasm_wasi02 wasi02{};
    turbowasm_wasi02_config config{};

    assert(turbowasm_wasi02_init(
               &wasi02,
               &config,
               nullptr) == TURBOWASM_OK);
    assert(wasi02.impl != nullptr);
    assert(turbowasm_wasi02_destroy(
               &wasi02) == TURBOWASM_OK);
    assert(wasi02.impl == nullptr);
    return 0;
}
