#include "wasi02_descriptor.h"

#include <string.h>

#define TW_WASI02_V028 {0u, 2u, 8u}

static const turbowasm_wasi02_type_desc type_bool = {
    TURBOWASM_WASI02_TYPE_BOOL, "bool", {{0}}
};
static const turbowasm_wasi02_type_desc type_u8 = {
    TURBOWASM_WASI02_TYPE_U8, "u8", {{0}}
};
static const turbowasm_wasi02_type_desc type_u32 = {
    TURBOWASM_WASI02_TYPE_U32, "u32", {{0}}
};
static const turbowasm_wasi02_type_desc type_u64 = {
    TURBOWASM_WASI02_TYPE_U64, "u64", {{0}}
};
static const turbowasm_wasi02_type_desc type_string = {
    TURBOWASM_WASI02_TYPE_STRING, "string", {{0}}
};

static const turbowasm_wasi02_record_field datetime_fields[] = {
    {"seconds", &type_u64},
    {"nanoseconds", &type_u32}
};
static const turbowasm_wasi02_type_desc type_datetime = {
    TURBOWASM_WASI02_TYPE_RECORD,
    "datetime",
    {.record = {datetime_fields, 2u}}
};

static const turbowasm_wasi02_type_desc type_instant = {
    TURBOWASM_WASI02_TYPE_ALIAS,
    "instant",
    {.alias = {&type_u64}}
};
static const turbowasm_wasi02_type_desc type_duration = {
    TURBOWASM_WASI02_TYPE_ALIAS,
    "duration",
    {.alias = {&type_u64}}
};

static const turbowasm_wasi02_type_desc type_pollable = {
    TURBOWASM_WASI02_TYPE_RESOURCE,
    "pollable",
    {.resource = {
        "wasi:io", "poll", TW_WASI02_V028, "pollable"
    }}
};

static const turbowasm_wasi02_type_desc type_list_u8 = {
    TURBOWASM_WASI02_TYPE_LIST,
    NULL,
    {.list = {&type_u8}}
};

static const turbowasm_wasi02_type_desc *const seed_elements[] = {
    &type_u64, &type_u64
};
static const turbowasm_wasi02_type_desc type_seed_tuple = {
    TURBOWASM_WASI02_TYPE_TUPLE,
    NULL,
    {.tuple = {seed_elements, 2u}}
};

static const turbowasm_wasi02_type_desc *const env_pair_elements[] = {
    &type_string, &type_string
};
static const turbowasm_wasi02_type_desc type_env_pair = {
    TURBOWASM_WASI02_TYPE_TUPLE,
    NULL,
    {.tuple = {env_pair_elements, 2u}}
};
static const turbowasm_wasi02_type_desc type_environment = {
    TURBOWASM_WASI02_TYPE_LIST,
    NULL,
    {.list = {&type_env_pair}}
};
static const turbowasm_wasi02_type_desc type_arguments = {
    TURBOWASM_WASI02_TYPE_LIST,
    NULL,
    {.list = {&type_string}}
};
static const turbowasm_wasi02_type_desc type_optional_string = {
    TURBOWASM_WASI02_TYPE_OPTION,
    NULL,
    {.option = {&type_string}}
};
static const turbowasm_wasi02_type_desc type_unit_result = {
    TURBOWASM_WASI02_TYPE_RESULT,
    NULL,
    {.result = {NULL, NULL}}
};

static const turbowasm_wasi02_function_desc wall_clock_functions[] = {
    {"now", NULL, 0u, &type_datetime},
    {"resolution", NULL, 0u, &type_datetime}
};

static const turbowasm_wasi02_param_desc instant_param[] = {
    {"when", &type_instant}
};
static const turbowasm_wasi02_param_desc duration_param[] = {
    {"when", &type_duration}
};
static const turbowasm_wasi02_function_desc monotonic_clock_functions[] = {
    {"now", NULL, 0u, &type_instant},
    {"resolution", NULL, 0u, &type_duration},
    {"subscribe-instant", instant_param, 1u, &type_pollable},
    {"subscribe-duration", duration_param, 1u, &type_pollable}
};

static const turbowasm_wasi02_param_desc random_len_param[] = {
    {"len", &type_u64}
};
static const turbowasm_wasi02_function_desc random_functions[] = {
    {"get-random-bytes", random_len_param, 1u, &type_list_u8},
    {"get-random-u64", NULL, 0u, &type_u64}
};
static const turbowasm_wasi02_function_desc insecure_functions[] = {
    {"get-insecure-random-bytes", random_len_param, 1u, &type_list_u8},
    {"get-insecure-random-u64", NULL, 0u, &type_u64}
};
static const turbowasm_wasi02_function_desc insecure_seed_functions[] = {
    {"insecure-seed", NULL, 0u, &type_seed_tuple}
};

static const turbowasm_wasi02_function_desc environment_functions[] = {
    {"get-environment", NULL, 0u, &type_environment},
    {"get-arguments", NULL, 0u, &type_arguments},
    {"initial-cwd", NULL, 0u, &type_optional_string}
};
static const turbowasm_wasi02_param_desc exit_params[] = {
    {"status", &type_unit_result}
};
static const turbowasm_wasi02_function_desc exit_functions[] = {
    {"exit", exit_params, 1u, NULL}
};

static const turbowasm_wasi02_interface_desc interfaces[] = {
    {
        "wasi:clocks", "wall-clock", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-clocks",
        "71e486b1b44a49687dbe17d5b979d6da6112c7f2",
        wall_clock_functions, 2u
    },
    {
        "wasi:clocks", "monotonic-clock", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-clocks",
        "71e486b1b44a49687dbe17d5b979d6da6112c7f2",
        monotonic_clock_functions, 4u
    },
    {
        "wasi:random", "random", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-random",
        "bd54965b22082b3e157b2fb4bc77987c33ae7d5f",
        random_functions, 2u
    },
    {
        "wasi:random", "insecure", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-random",
        "bd54965b22082b3e157b2fb4bc77987c33ae7d5f",
        insecure_functions, 2u
    },
    {
        "wasi:random", "insecure-seed", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-random",
        "bd54965b22082b3e157b2fb4bc77987c33ae7d5f",
        insecure_seed_functions, 1u
    },
    {
        "wasi:cli", "environment", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-cli",
        "e922fd7bd137cd284a5e6c4815a5a630d32fdd01",
        environment_functions, 3u
    },
    {
        "wasi:cli", "exit", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-cli",
        "e922fd7bd137cd284a5e6c4815a5a630d32fdd01",
        exit_functions, 1u
    }
};

size_t turbowasm_wasi02_interface_count(void) {
    return sizeof(interfaces) / sizeof(interfaces[0]);
}

const turbowasm_wasi02_interface_desc *
turbowasm_wasi02_interface_at(size_t index) {
    return index < turbowasm_wasi02_interface_count()
        ? &interfaces[index]
        : NULL;
}

const turbowasm_wasi02_interface_desc *
turbowasm_wasi02_find_interface(
    const char *package_name,
    const char *interface_name) {
    size_t i;

    if (package_name == NULL || interface_name == NULL)
        return NULL;

    for (i = 0u; i < turbowasm_wasi02_interface_count(); ++i) {
        const turbowasm_wasi02_interface_desc *desc = &interfaces[i];
        if (strcmp(desc->package_name, package_name) == 0 &&
            strcmp(desc->interface_name, interface_name) == 0)
            return desc;
    }
    return NULL;
}

const turbowasm_wasi02_function_desc *
turbowasm_wasi02_find_function(
    const turbowasm_wasi02_interface_desc *interface_desc,
    const char *name) {
    uint32_t i;

    if (interface_desc == NULL || name == NULL)
        return NULL;

    for (i = 0u; i < interface_desc->function_count; ++i) {
        const turbowasm_wasi02_function_desc *function =
            &interface_desc->functions[i];
        if (strcmp(function->name, name) == 0)
            return function;
    }
    return NULL;
}
