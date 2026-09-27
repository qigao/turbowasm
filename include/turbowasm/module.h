#ifndef TURBOWASM_MODULE_H
#define TURBOWASM_MODULE_H

#include <turbowasm/status.h>

#include <cmeta/cmeta.h>

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

/*
 * A Wasm function may have zero, one, or multiple results, so TurboWasm does
 * not force it into CMeta's single-return cmeta_function_desc. Instead the
 * module exposes an allocation-free reflected signature view whose individual
 * parameter/result types are canonical cmeta_type_desc values.
 */
typedef struct turbowasm_function_signature {
    uint32_t param_count;
    uint32_t result_count;
} turbowasm_function_signature;

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
    uint32_t element_count;
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

bool turbowasm_module_function_signature_get(
    const turbowasm_module *module,
    uint32_t function_index,
    turbowasm_function_signature *out);

const cmeta_type_desc *turbowasm_module_function_param_type(
    const turbowasm_module *module,
    uint32_t function_index,
    uint32_t param_index);

const cmeta_type_desc *turbowasm_module_function_result_type(
    const turbowasm_module *module,
    uint32_t function_index,
    uint32_t result_index);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_MODULE_H */
