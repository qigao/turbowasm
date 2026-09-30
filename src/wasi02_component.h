#ifndef TURBOWASM_WASI02_COMPONENT_H
#define TURBOWASM_WASI02_COMPONENT_H

#include "component_exec.h"
#include "wasi02_provider.h"

/*
 * Instantiate the retained synchronous WASI 0.2 Component-import slice
 * through a W2a provider. The provider is borrowed and must outlive the
 * executable.
 *
 * W2b3 admits the retained scalar/string/list/record/tuple/option/result
 * shapes with pinned canonical memory/realloc semantics. Resource-valued
 * pollables remain fail-closed for W4.
 */
turbowasm_status turbowasm_wasi02_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_provider *provider);

#endif /* TURBOWASM_WASI02_COMPONENT_H */
