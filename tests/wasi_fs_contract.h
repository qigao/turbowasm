#ifndef TURBOWASM_WASI_FS_CONTRACT_H
#define TURBOWASM_WASI_FS_CONTRACT_H

#include <turbowasm/wasi_fs.h>

typedef struct turbowasm_wasi_fs_contract_fixture {
    void *context;
    turbowasm_status (*setup)(
        void *context,
        turbowasm_wasi_fs_provider *out_provider,
        turbowasm_wasi_fs_file *out_root);
    void (*teardown)(void *context);
} turbowasm_wasi_fs_contract_fixture;

int turbowasm_wasi_fs_contract_run(
    const turbowasm_wasi_fs_contract_fixture *fixture);

#endif /* TURBOWASM_WASI_FS_CONTRACT_H */
