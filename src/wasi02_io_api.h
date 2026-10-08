#ifndef TURBOWASM_WASI02_IO_API_INTERNAL_H
#define TURBOWASM_WASI02_IO_API_INTERNAL_H
#include <turbowasm/wasi02_io.h>
void turbowasm_wasi02_io_wait_done(void *context, uintptr_t operation_token);
bool turbowasm_wasi02_io_retain_private(turbowasm_wasi02_io *io);
void turbowasm_wasi02_io_release_private(void *context);
void *turbowasm_wasi02_io_context_private(turbowasm_wasi02_io *io);
uint64_t turbowasm_wasi02_io_token_private(void);
#endif
