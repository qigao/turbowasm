#ifndef TURBOWASM_MODULE_H
#define TURBOWASM_MODULE_H

#include <turbowasm/status.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zero-initialize before first use.  A loaded module borrows the input bytes;
 * they must remain alive and immutable until turbowasm_module_destroy(). */
typedef struct turbowasm_module {
    void *impl;
} turbowasm_module;

turbowasm_status turbowasm_module_load_borrowed(turbowasm_module *module,
                                                const uint8_t *bytes,
                                                size_t size);
void turbowasm_module_destroy(turbowasm_module *module);

const uint8_t *turbowasm_module_bytes(const turbowasm_module *module);
size_t turbowasm_module_size(const turbowasm_module *module);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_MODULE_H */
