#ifndef TURBOWASM_WASI02_COMPONENT_H
#define TURBOWASM_WASI02_COMPONENT_H

#include "component_exec.h"
#include "wasi02_provider.h"

/*
 * Instantiate the W2b flat-scalar Component-import slice through a W2a
 * provider. The provider is borrowed and must outlive the executable.
 *
 * Only W1 interfaces/functions whose retained Component type is a direct
 * scalar canonical shape are admitted here. Memory-bearing record/list/string
 * and resource shapes remain fail-closed for W2b3/W3/W4.
 */
turbowasm_status turbowasm_wasi02_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_provider *provider);

#endif /* TURBOWASM_WASI02_COMPONENT_H */
