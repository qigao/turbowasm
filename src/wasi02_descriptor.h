#ifndef TURBOWASM_WASI02_DESCRIPTOR_H
#define TURBOWASM_WASI02_DESCRIPTOR_H

#include <stddef.h>
#include <stdint.h>

typedef struct turbowasm_wasi02_version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} turbowasm_wasi02_version;

typedef enum turbowasm_wasi02_type_kind {
    TURBOWASM_WASI02_TYPE_UNIT = 0,
    TURBOWASM_WASI02_TYPE_BOOL,
    TURBOWASM_WASI02_TYPE_U8,
    TURBOWASM_WASI02_TYPE_U32,
    TURBOWASM_WASI02_TYPE_U64,
    TURBOWASM_WASI02_TYPE_STRING,
    TURBOWASM_WASI02_TYPE_ALIAS,
    TURBOWASM_WASI02_TYPE_LIST,
    TURBOWASM_WASI02_TYPE_TUPLE,
    TURBOWASM_WASI02_TYPE_RECORD,
    TURBOWASM_WASI02_TYPE_OPTION,
    TURBOWASM_WASI02_TYPE_RESULT,
    TURBOWASM_WASI02_TYPE_ENUM,
    TURBOWASM_WASI02_TYPE_FLAGS,
    TURBOWASM_WASI02_TYPE_RESOURCE
} turbowasm_wasi02_type_kind;

typedef struct turbowasm_wasi02_type_desc
    turbowasm_wasi02_type_desc;

typedef struct turbowasm_wasi02_record_field {
    const char *name;
    const turbowasm_wasi02_type_desc *type;
} turbowasm_wasi02_record_field;

typedef struct turbowasm_wasi02_label_set {
    const char *const *labels;
    uint32_t count;
} turbowasm_wasi02_label_set;

struct turbowasm_wasi02_type_desc {
    turbowasm_wasi02_type_kind kind;
    const char *name;
    union {
        struct {
            const turbowasm_wasi02_type_desc *target;
        } alias;
        struct {
            const turbowasm_wasi02_type_desc *element;
        } list;
        struct {
            const turbowasm_wasi02_type_desc *const *elements;
            uint32_t count;
        } tuple;
        struct {
            const turbowasm_wasi02_record_field *fields;
            uint32_t count;
        } record;
        struct {
            const turbowasm_wasi02_type_desc *payload;
        } option;
        struct {
            /* NULL denotes the WIT unit arm. */
            const turbowasm_wasi02_type_desc *ok;
            const turbowasm_wasi02_type_desc *error;
        } result;
        turbowasm_wasi02_label_set enumeration;
        turbowasm_wasi02_label_set flags;
        struct {
            const char *package_name;
            const char *interface_name;
            turbowasm_wasi02_version version;
            const char *resource_name;
        } resource;
    } as;
};

typedef struct turbowasm_wasi02_param_desc {
    const char *name;
    const turbowasm_wasi02_type_desc *type;
} turbowasm_wasi02_param_desc;

typedef struct turbowasm_wasi02_function_desc {
    const char *name;
    const turbowasm_wasi02_param_desc *params;
    uint32_t param_count;
    /* NULL means no result. */
    const turbowasm_wasi02_type_desc *result;
} turbowasm_wasi02_function_desc;

typedef struct turbowasm_wasi02_interface_desc {
    const char *package_name;
    const char *interface_name;
    turbowasm_wasi02_version version;

    const char *source_repository;
    const char *source_commit;

    const turbowasm_wasi02_function_desc *functions;
    uint32_t function_count;
} turbowasm_wasi02_interface_desc;

size_t turbowasm_wasi02_interface_count(void);

const turbowasm_wasi02_interface_desc *
turbowasm_wasi02_interface_at(size_t index);

const turbowasm_wasi02_interface_desc *
turbowasm_wasi02_find_interface(
    const char *package_name,
    const char *interface_name);

const turbowasm_wasi02_function_desc *
turbowasm_wasi02_find_function(
    const turbowasm_wasi02_interface_desc *interface_desc,
    const char *name);

#endif /* TURBOWASM_WASI02_DESCRIPTOR_H */
