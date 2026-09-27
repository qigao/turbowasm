#ifndef TURBOWASM_LINK_INTERNAL_H
#define TURBOWASM_LINK_INTERNAL_H

#include <turbowasm/link.h>

struct turbowasm_instance_impl;
struct turbowasm_module_impl;

turbowasm_status turbowasm_linker_bind_instance(
    struct turbowasm_instance_impl *instance,
    const struct turbowasm_module_impl *module,
    const turbowasm_linker *linker);

#endif /* TURBOWASM_LINK_INTERNAL_H */
