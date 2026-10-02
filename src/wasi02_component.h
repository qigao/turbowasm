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
/*
 * Internal typed value bridge shared by WASI 0.2 capability adapters.
 * RESOURCE values map from Component own/borrow on input and to own on output.
 * The caller validates nominal resource identity before conversion.
 */
turbowasm_status turbowasm_wasi02_component_value_to_wasi(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_component_value *value,
    turbowasm_wasi02_value *out);

turbowasm_status turbowasm_wasi02_component_value_from_wasi(
    const turbowasm_wasi02_type_desc *type,
    const turbowasm_wasi02_value *value,
    turbowasm_component_value *out);

bool turbowasm_wasi02_component_type_matches(
    const turbowasm_component_type_graph *graph,
    turbowasm_component_type_ref component_ref,
    const turbowasm_wasi02_type_desc *wasi_type);

turbowasm_status turbowasm_wasi02_component_imports(
    turbowasm_wasi02_provider *provider,
    turbowasm_component_exec_imports *out_imports);

turbowasm_status turbowasm_wasi02_component_exec_init(
    turbowasm_component_exec *exec,
    const turbowasm_component_binary *binary,
    turbowasm_wasi02_provider *provider);

#endif /* TURBOWASM_WASI02_COMPONENT_H */
