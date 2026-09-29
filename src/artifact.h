#ifndef TURBOWASM_ARTIFACT_H
#define TURBOWASM_ARTIFACT_H

#include <turbowasm/module.h>

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TURBOWASM_ARTIFACT_SCHEMA_VERSION = 1,
    TURBOWASM_ARTIFACT_FLAG_COMPLETE_METADATA = 1u
};

enum {
    TURBOWASM_ARTIFACT_SECTION_SUMMARY = 1u,
    TURBOWASM_ARTIFACT_SECTION_CORE_METADATA = 2u
};

typedef struct turbowasm_artifact_info {
    uint32_t schema_version;
    uint32_t flags;
    uint64_t feature_fingerprint;
    uint64_t source_size;
    uint8_t source_sha256[TURBOWASM_SHA256_DIGEST_SIZE];
    uint32_t section_count;
    turbowasm_module_summary summary;

    bool has_core_metadata;
    uint32_t metadata_type_count;
    uint32_t metadata_function_count;
    uint32_t metadata_import_count;
    uint32_t metadata_export_count;
    uint32_t metadata_memory_count;
} turbowasm_artifact_info;

uint64_t turbowasm_artifact_current_feature_fingerprint(void);

turbowasm_status turbowasm_artifact_measure(
    const turbowasm_module *module,
    size_t *out_size);

turbowasm_status turbowasm_artifact_write(
    const turbowasm_module *module,
    uint8_t *output,
    size_t capacity,
    size_t *out_size);

turbowasm_status turbowasm_artifact_inspect(
    const uint8_t *artifact,
    size_t artifact_size,
    const uint8_t *source,
    size_t source_size,
    turbowasm_artifact_info *out);

#endif
