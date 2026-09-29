#include <turbowasm/wasi.h>

#include <stdint.h>

int main(void) {
    const char *args[] = {"app"};
    turbowasm_wasi_preview1_config config = {
        true, args, 1u,
        false, NULL, 0u
    };
    turbowasm_wasi_preview1 wasi = {0};
    turbowasm_linker linker = {0};

    if (turbowasm_wasi_preview1_init(
            &wasi, &config) != TURBOWASM_OK)
        return 1;
    if (turbowasm_linker_init(&linker) != TURBOWASM_OK)
        return 2;
    if (turbowasm_wasi_preview1_define(
            &wasi, &linker) != TURBOWASM_OK)
        return 3;

    turbowasm_linker_destroy(&linker);
    turbowasm_wasi_preview1_destroy(&wasi);
    return 0;
}
