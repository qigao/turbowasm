#ifndef TURBOWASM_LINK_H
#define TURBOWASM_LINK_H

#include <turbowasm/module.h>
#include <turbowasm/status.h>

#ifdef __cplusplus
extern "C" {
#endif

struct turbowasm_instance;

/*
 * Explicit capability scope for module linking.
 *
 * Namespace names are copied into the linker. Provider instances are borrowed;
 * they and their modules must outlive every consumer instance created from the
 * linker. The linker itself may be destroyed after linked instance creation.
 */
typedef struct turbowasm_linker {
    void *impl;
} turbowasm_linker;

turbowasm_status turbowasm_linker_init(turbowasm_linker *linker);
void turbowasm_linker_destroy(turbowasm_linker *linker);

turbowasm_status turbowasm_linker_define_instance(
    turbowasm_linker *linker,
    turbowasm_name module_name,
    struct turbowasm_instance *instance);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_LINK_H */
