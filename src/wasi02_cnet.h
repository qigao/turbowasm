#ifndef TURBOWASM_WASI02_CNET_INTERNAL_H
#define TURBOWASM_WASI02_CNET_INTERNAL_H
#include <turbowasm/wasi02_cnet.h>
#include "wasi02_io_api.h"
/* Legacy internal control-plane provider, used by its existing test suite. */
turbowasm_status turbowasm_wasi02_cnet_init(turbowasm_wasi02_cnet *adapter,
    const turbowasm_wasi02_cnet_config *config);
#endif
