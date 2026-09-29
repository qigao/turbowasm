#ifndef TURBOWASM_MODULE_H
#define TURBOWASM_MODULE_H

#include <turbowasm/status.h>
#include <turbowasm/runtime.h>

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

/*
 * UTF-8 names borrow immutable bytes from the loaded module. They are not
 * NUL-terminated and remain valid only while the module and its source bytes
 * remain alive.
 */
typedef struct turbowasm_name {
    const uint8_t *bytes;
    uint32_t size;
} turbowasm_name;

typedef enum turbowasm_external_kind {
    TURBOWASM_EXTERN_FUNCTION = 0,
    TURBOWASM_EXTERN_TABLE = 1,
    TURBOWASM_EXTERN_MEMORY = 2,
    TURBOWASM_EXTERN_GLOBAL = 3,
    TURBOWASM_EXTERN_TAG = 4
} turbowasm_external_kind;

/*
 * item_index is the index in the corresponding Wasm function/table/memory/
 * global/tag index space. Function and tag imports retain their Wasm type
 * index. Other imports use UINT32_MAX.
 */
typedef struct turbowasm_import_desc {
    turbowasm_name module_name;
    turbowasm_name name;
    turbowasm_external_kind kind;
    uint32_t item_index;
    uint32_t type_index;
} turbowasm_import_desc;

typedef struct turbowasm_export_desc {
    turbowasm_name name;
    turbowasm_external_kind kind;
    uint32_t item_index;
} turbowasm_export_desc;

typedef struct turbowasm_memory_desc {
    /*
     * Legacy memory32 mirrors. For memory64 these saturate at UINT32_MAX;
     * use minimum64/maximum64 when memory64 is true.
     */
    uint32_t minimum;
    uint32_t maximum;
    uint32_t page_size;
    bool has_maximum;
    bool shared;
    bool imported;

    bool memory64;
    uint64_t minimum64;
    uint64_t maximum64;
} turbowasm_memory_desc;

typedef struct turbowasm_module_summary {
    uint32_t standard_section_mask;
    size_t custom_section_count;

    uint32_t type_count;

    uint32_t imported_function_count;
    uint32_t imported_table_count;
    uint32_t imported_memory_count;
    uint32_t imported_global_count;
    uint32_t imported_tag_count;

    uint32_t function_count;
    uint32_t table_count;
    uint32_t memory_count;
    uint32_t global_count;
    uint32_t tag_count;
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

/* Load with caller-supplied allocation/resource policy. The config is copied;
 * any allocator context it references must remain valid until all objects
 * derived from the module are destroyed. Zero limits mean unlimited. */
turbowasm_status turbowasm_module_load_borrowed_with_config(
    turbowasm_module *module,
    const uint8_t *bytes,
    size_t size,
    const turbowasm_runtime_config *config);

/*
 * Restore a module from a COMPLETE_METADATA artifact produced for the exact
 * borrowed source bytes. The artifact is structurally verified, matched to the
 * source SHA-256 and current Runtime feature fingerprint, then retained
 * validation metadata is reconstructed without rerunning Wasm validation.
 */
turbowasm_status turbowasm_module_load_borrowed_from_artifact(
    turbowasm_module *module,
    const uint8_t *bytes,
    size_t size,
    const uint8_t *artifact,
    size_t artifact_size);

turbowasm_status turbowasm_module_load_borrowed_from_artifact_with_config(
    turbowasm_module *module,
    const uint8_t *bytes,
    size_t size,
    const uint8_t *artifact,
    size_t artifact_size,
    const turbowasm_runtime_config *config);
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

size_t turbowasm_module_import_count(const turbowasm_module *module);
const turbowasm_import_desc *turbowasm_module_import_at(
    const turbowasm_module *module,
    size_t index);

size_t turbowasm_module_export_count(const turbowasm_module *module);
const turbowasm_export_desc *turbowasm_module_export_at(
    const turbowasm_module *module,
    size_t index);

size_t turbowasm_module_memory_count(
    const turbowasm_module *module);

bool turbowasm_module_memory_at(
    const turbowasm_module *module,
    size_t index,
    turbowasm_memory_desc *out);

#ifdef __cplusplus
}
#endif

#endif /* TURBOWASM_MODULE_H */
