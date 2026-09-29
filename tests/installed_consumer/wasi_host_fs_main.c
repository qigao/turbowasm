#include <turbowasm/wasi_host_fs.h>

int main(void) {
    turbowasm_wasi_host_fs adapter = {0};
    turbowasm_wasi_host_fs_config config = {0};

    return adapter.impl == 0 &&
           config.host_root == 0 &&
           config.file_capacity == 0u &&
           config.path_capacity == 0u
        ? 0
        : 1;
}
