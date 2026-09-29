#ifndef TURBOWASM_COMPONENT_BINARY_H
#define TURBOWASM_COMPONENT_BINARY_H

#include <turbowasm/runtime.h>
#include <turbowasm/status.h>

#include "component_type_graph.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    TURBOWASM_COMPONENT_BINARY_VERSION = 0x000du,
    TURBOWASM_COMPONENT_BINARY_LAYER = 0x0001u
};

typedef struct turbowasm_component_section {
    uint8_t id;
    const uint8_t *payload;
    uint32_t size;
} turbowasm_component_section;

typedef struct turbowasm_component_core_module {
    const uint8_t *bytes;
    uint32_t size;
} turbowasm_component_core_module;

typedef struct turbowasm_component_core_instance_def {
    uint32_t module_index;
} turbowasm_component_core_instance_def;

typedef struct turbowasm_component_name {
    const uint8_t *bytes;
    uint32_t size;
} turbowasm_component_name;

typedef struct turbowasm_component_core_function_alias {
    uint32_t core_function_index;
    uint32_t instance_index;
    turbowasm_component_name name;
} turbowasm_component_core_function_alias;

typedef struct turbowasm_component_canon_lift {
    uint32_t component_function_index;
    uint32_t core_function_index;
    uint32_t type_index;
} turbowasm_component_canon_lift;

typedef enum turbowasm_component_external_kind {
    TURBOWASM_COMPONENT_EXTERN_CORE_MODULE = 0,
    TURBOWASM_COMPONENT_EXTERN_FUNCTION = 1,
    TURBOWASM_COMPONENT_EXTERN_VALUE = 2,
    TURBOWASM_COMPONENT_EXTERN_TYPE = 3,
    TURBOWASM_COMPONENT_EXTERN_COMPONENT = 4,
    TURBOWASM_COMPONENT_EXTERN_INSTANCE = 5
} turbowasm_component_external_kind;

typedef struct turbowasm_component_import {
    turbowasm_component_name name;
    turbowasm_component_external_kind kind;
    uint32_t type_index;
} turbowasm_component_import;

typedef struct turbowasm_component_export {
    turbowasm_component_name name;
    turbowasm_component_external_kind kind;
    uint32_t item_index;
    bool has_ascribed_type;
    uint32_t type_index;
} turbowasm_component_export;

typedef struct turbowasm_component_binary {
    const uint8_t *bytes;
    size_t size;

    turbowasm_component_section *sections;
    uint32_t section_count;
    uint32_t section_capacity;

    turbowasm_component_core_module *core_modules;
    uint32_t core_module_count;
    uint32_t core_module_capacity;

    turbowasm_component_core_instance_def *core_instances;
    uint32_t core_instance_count;
    uint32_t core_instance_capacity;

    turbowasm_component_core_function_alias *core_function_aliases;
    uint32_t core_function_alias_count;
    uint32_t core_function_alias_capacity;

    turbowasm_component_canon_lift *canon_lifts;
    uint32_t canon_lift_count;
    uint32_t canon_lift_capacity;

    turbowasm_component_type_graph type_graph;

    turbowasm_component_import *imports;
    uint32_t import_count;
    uint32_t import_capacity;

    turbowasm_component_export *exports;
    uint32_t export_count;
    uint32_t export_capacity;

    turbowasm_runtime_config config;
} turbowasm_component_binary;

turbowasm_status turbowasm_component_binary_load(
    turbowasm_component_binary *component,
    const uint8_t *bytes,
    size_t size);

turbowasm_status turbowasm_component_binary_load_with_config(
    turbowasm_component_binary *component,
    const uint8_t *bytes,
    size_t size,
    const turbowasm_runtime_config *config);

void turbowasm_component_binary_destroy(
    turbowasm_component_binary *component);

const turbowasm_component_section *
turbowasm_component_binary_section_at(
    const turbowasm_component_binary *component,
    uint32_t index);

const turbowasm_component_core_module *
turbowasm_component_binary_core_module_at(
    const turbowasm_component_binary *component,
    uint32_t index);

const turbowasm_component_core_instance_def *
turbowasm_component_binary_core_instance_at(
    const turbowasm_component_binary *component,
    uint32_t index);

const turbowasm_component_core_function_alias *
turbowasm_component_binary_core_function_alias_at(
    const turbowasm_component_binary *component,
    uint32_t index);

const turbowasm_component_canon_lift *
turbowasm_component_binary_canon_lift_at(
    const turbowasm_component_binary *component,
    uint32_t index);

const turbowasm_component_import *
turbowasm_component_binary_import_at(
    const turbowasm_component_binary *component,
    uint32_t index);

const turbowasm_component_export *
turbowasm_component_binary_export_at(
    const turbowasm_component_binary *component,
    uint32_t index);

#endif /* TURBOWASM_COMPONENT_BINARY_H */
