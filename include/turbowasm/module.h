#ifndef TURBOWASM_MODULE_H
#define TURBOWASM_MODULE_H

#include <turbowasm/status.h>

#include <stdbool.h>
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

typedef struct turbowasm_module_summary {
    uint32_t standard_section_mask;
    size_t custom_section_count;

    uint32_t type_count;

    uint32_t imported_function_count;
    uint32_t imported_table_count;
    uint32_t imported_memory_count;
    uint32_t imported_global_count;

    uint32_t function_count;
    uint32_t table_count;
    uint32_t memory_count;
    uint32_t global_count;
    uint32_t code_count;

    uint32_t export_count;
    bool has_start;
    uint32_t start_function_index;
    bool has_data_count;
    uint32_t data_count;
    uint32_t data_segment_count;
} turbowasm_module_summary;

turbowasm_status turbowasm_module_load_borrowed(turbowasm_module *module,
                                                const uint8_t *bytes,
                                                size_t size);
void turbowasm_module_destroy(turbowasm_module *module);

const uint8_t *turbowasm_module_bytes(const turbowasm_module *module);
size_t turbowasm_module_size(const turbowasm_module *module);
bool turbowasm_module_summary_get(const turbowasm_module *module,
                                  turbowasm_module_summary *out);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_MODULE_H */
