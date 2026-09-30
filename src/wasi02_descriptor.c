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

static const turbowasm_wasi02_type_desc type_io_error = {
    TURBOWASM_WASI02_TYPE_RESOURCE,
    "error",
    {.resource = {
        "wasi:io", "error", TW_WASI02_V028, "error"
    }}
};

static const turbowasm_wasi02_type_desc type_input_stream = {
    TURBOWASM_WASI02_TYPE_RESOURCE,
    "input-stream",
    {.resource = {
        "wasi:io", "streams", TW_WASI02_V028, "input-stream"
    }}
};

static const turbowasm_wasi02_type_desc type_output_stream = {
    TURBOWASM_WASI02_TYPE_RESOURCE,
    "output-stream",
    {.resource = {
        "wasi:io", "streams", TW_WASI02_V028, "output-stream"
    }}
};

static const turbowasm_wasi02_variant_case stream_error_cases[] = {
    {"last-operation-failed", &type_io_error},
    {"closed", NULL}
};
static const turbowasm_wasi02_type_desc type_stream_error = {
    TURBOWASM_WASI02_TYPE_VARIANT,
    "stream-error",
    {.variant = {stream_error_cases, 2u}}
};

static const turbowasm_wasi02_type_desc type_stream_result_bytes = {
    TURBOWASM_WASI02_TYPE_RESULT,
    NULL,
    {.result = {&type_list_u8, &type_stream_error}}
};
static const turbowasm_wasi02_type_desc type_stream_result_u64 = {
    TURBOWASM_WASI02_TYPE_RESULT,
    NULL,
    {.result = {&type_u64, &type_stream_error}}
};
static const turbowasm_wasi02_type_desc type_stream_result_unit = {
    TURBOWASM_WASI02_TYPE_RESULT,
    NULL,
    {.result = {NULL, &type_stream_error}}
};

static const turbowasm_wasi02_type_desc type_fs_descriptor = {
    TURBOWASM_WASI02_TYPE_RESOURCE,
    "descriptor",
    {.resource = {
        "wasi:filesystem", "types", TW_WASI02_V028, "descriptor"
    }}
};

static const char *const fs_descriptor_type_labels[] = {
    "unknown", "block-device", "character-device", "directory",
    "fifo", "symbolic-link", "regular-file", "socket"
};
static const turbowasm_wasi02_type_desc type_fs_descriptor_type = {
    TURBOWASM_WASI02_TYPE_ENUM,
    "descriptor-type",
    {.enumeration = {fs_descriptor_type_labels, 8u}}
};

static const char *const fs_descriptor_flags_labels[] = {
    "read", "write", "file-integrity-sync", "data-integrity-sync",
    "requested-write-sync", "mutate-directory"
};
static const turbowasm_wasi02_type_desc type_fs_descriptor_flags = {
    TURBOWASM_WASI02_TYPE_FLAGS,
    "descriptor-flags",
    {.flags = {fs_descriptor_flags_labels, 6u}}
};

static const char *const fs_path_flags_labels[] = {
    "symlink-follow"
};
static const turbowasm_wasi02_type_desc type_fs_path_flags = {
    TURBOWASM_WASI02_TYPE_FLAGS,
    "path-flags",
    {.flags = {fs_path_flags_labels, 1u}}
};

static const char *const fs_open_flags_labels[] = {
    "create", "directory", "exclusive", "truncate"
};
static const turbowasm_wasi02_type_desc type_fs_open_flags = {
    TURBOWASM_WASI02_TYPE_FLAGS,
    "open-flags",
    {.flags = {fs_open_flags_labels, 4u}}
};

static const char *const fs_error_code_labels[] = {
    "access", "would-block", "already", "bad-descriptor", "busy",
    "deadlock", "quota", "exist", "file-too-large",
    "illegal-byte-sequence", "in-progress", "interrupted", "invalid",
    "io", "is-directory", "loop", "too-many-links", "message-size",
    "name-too-long", "no-device", "no-entry", "no-lock",
    "insufficient-memory", "insufficient-space", "not-directory",
    "not-empty", "not-recoverable", "unsupported", "no-tty",
    "no-such-device", "overflow", "not-permitted", "pipe", "read-only",
    "invalid-seek", "text-file-busy", "cross-device"
};
static const turbowasm_wasi02_type_desc type_fs_error_code = {
    TURBOWASM_WASI02_TYPE_ENUM,
    "error-code",
    {.enumeration = {fs_error_code_labels, 37u}}
};

static const turbowasm_wasi02_type_desc type_fs_filesize = {
    TURBOWASM_WASI02_TYPE_ALIAS,
    "filesize",
    {.alias = {&type_u64}}
};
static const turbowasm_wasi02_type_desc type_fs_link_count = {
    TURBOWASM_WASI02_TYPE_ALIAS,
    "link-count",
    {.alias = {&type_u64}}
};
static const turbowasm_wasi02_type_desc type_optional_datetime = {
    TURBOWASM_WASI02_TYPE_OPTION,
    NULL,
    {.option = {&type_datetime}}
};
static const turbowasm_wasi02_record_field fs_descriptor_stat_fields[] = {
    {"type", &type_fs_descriptor_type},
    {"link-count", &type_fs_link_count},
    {"size", &type_fs_filesize},
    {"data-access-timestamp", &type_optional_datetime},
    {"data-modification-timestamp", &type_optional_datetime},
    {"status-change-timestamp", &type_optional_datetime}
};
static const turbowasm_wasi02_type_desc type_fs_descriptor_stat = {
    TURBOWASM_WASI02_TYPE_RECORD,
    "descriptor-stat",
    {.record = {fs_descriptor_stat_fields, 6u}}
};

static const turbowasm_wasi02_type_desc type_fs_result_unit = {
    TURBOWASM_WASI02_TYPE_RESULT,
    NULL,
    {.result = {NULL, &type_fs_error_code}}
};
static const turbowasm_wasi02_type_desc type_fs_result_stat = {
    TURBOWASM_WASI02_TYPE_RESULT,
    NULL,
    {.result = {&type_fs_descriptor_stat, &type_fs_error_code}}
};
static const turbowasm_wasi02_type_desc type_fs_result_descriptor = {
    TURBOWASM_WASI02_TYPE_RESULT,
    NULL,
    {.result = {&type_fs_descriptor, &type_fs_error_code}}
};

static const turbowasm_wasi02_type_desc type_list_u32 = {
    TURBOWASM_WASI02_TYPE_LIST,
    NULL,
    {.list = {&type_u32}}
};
static const turbowasm_wasi02_type_desc type_list_pollable = {
    TURBOWASM_WASI02_TYPE_LIST,
    NULL,
    {.list = {&type_pollable}}
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

static const turbowasm_wasi02_type_desc *const preopen_pair_elements[] = {
    &type_fs_descriptor, &type_string
};
static const turbowasm_wasi02_type_desc type_preopen_pair = {
    TURBOWASM_WASI02_TYPE_TUPLE,
    NULL,
    {.tuple = {preopen_pair_elements, 2u}}
};
static const turbowasm_wasi02_type_desc type_preopen_list = {
    TURBOWASM_WASI02_TYPE_LIST,
    NULL,
    {.list = {&type_preopen_pair}}
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

static const turbowasm_wasi02_param_desc fs_create_dir_params[] = {
    {"self", &type_fs_descriptor},
    {"path", &type_string}
};
static const turbowasm_wasi02_param_desc fs_stat_params[] = {
    {"self", &type_fs_descriptor}
};
static const turbowasm_wasi02_param_desc fs_stat_at_params[] = {
    {"self", &type_fs_descriptor},
    {"path-flags", &type_fs_path_flags},
    {"path", &type_string}
};
static const turbowasm_wasi02_param_desc fs_open_at_params[] = {
    {"self", &type_fs_descriptor},
    {"path-flags", &type_fs_path_flags},
    {"path", &type_string},
    {"open-flags", &type_fs_open_flags},
    {"flags", &type_fs_descriptor_flags}
};
static const turbowasm_wasi02_param_desc fs_path_only_params[] = {
    {"self", &type_fs_descriptor},
    {"path", &type_string}
};
static const turbowasm_wasi02_function_desc filesystem_types_functions[] = {
    {"[method]descriptor.create-directory-at",
     fs_create_dir_params, 2u, &type_fs_result_unit},
    {"[method]descriptor.stat",
     fs_stat_params, 1u, &type_fs_result_stat},
    {"[method]descriptor.stat-at",
     fs_stat_at_params, 3u, &type_fs_result_stat},
    {"[method]descriptor.open-at",
     fs_open_at_params, 5u, &type_fs_result_descriptor},
    {"[method]descriptor.remove-directory-at",
     fs_path_only_params, 2u, &type_fs_result_unit},
    {"[method]descriptor.unlink-file-at",
     fs_path_only_params, 2u, &type_fs_result_unit}
};

static const turbowasm_wasi02_function_desc preopens_functions[] = {
    {"get-directories", NULL, 0u, &type_preopen_list}
};

static const turbowasm_wasi02_param_desc pollable_self_param[] = {
    {"self", &type_pollable}
};
static const turbowasm_wasi02_param_desc poll_list_param[] = {
    {"in", &type_list_pollable}
};
static const turbowasm_wasi02_function_desc poll_functions[] = {
    {"[method]pollable.ready",
     pollable_self_param, 1u, &type_bool},
    {"[method]pollable.block",
     pollable_self_param, 1u, NULL},
    {"poll",
     poll_list_param, 1u, &type_list_u32}
};

static const turbowasm_wasi02_param_desc io_error_self_param[] = {
    {"self", &type_io_error}
};
static const turbowasm_wasi02_function_desc io_error_functions[] = {
    {"[method]error.to-debug-string",
     io_error_self_param, 1u, &type_string}
};

static const turbowasm_wasi02_param_desc input_len_params[] = {
    {"self", &type_input_stream},
    {"len", &type_u64}
};
static const turbowasm_wasi02_param_desc input_self_param[] = {
    {"self", &type_input_stream}
};
static const turbowasm_wasi02_param_desc output_self_param[] = {
    {"self", &type_output_stream}
};
static const turbowasm_wasi02_param_desc output_contents_params[] = {
    {"self", &type_output_stream},
    {"contents", &type_list_u8}
};
static const turbowasm_wasi02_param_desc output_len_params[] = {
    {"self", &type_output_stream},
    {"len", &type_u64}
};
static const turbowasm_wasi02_param_desc output_splice_params[] = {
    {"self", &type_output_stream},
    {"src", &type_input_stream},
    {"len", &type_u64}
};

static const turbowasm_wasi02_function_desc streams_functions[] = {
    {"[method]input-stream.read",
     input_len_params, 2u, &type_stream_result_bytes},
    {"[method]input-stream.blocking-read",
     input_len_params, 2u, &type_stream_result_bytes},
    {"[method]input-stream.skip",
     input_len_params, 2u, &type_stream_result_u64},
    {"[method]input-stream.blocking-skip",
     input_len_params, 2u, &type_stream_result_u64},
    {"[method]input-stream.subscribe",
     input_self_param, 1u, &type_pollable},
    {"[method]output-stream.check-write",
     output_self_param, 1u, &type_stream_result_u64},
    {"[method]output-stream.write",
     output_contents_params, 2u, &type_stream_result_unit},
    {"[method]output-stream.blocking-write-and-flush",
     output_contents_params, 2u, &type_stream_result_unit},
    {"[method]output-stream.flush",
     output_self_param, 1u, &type_stream_result_unit},
    {"[method]output-stream.blocking-flush",
     output_self_param, 1u, &type_stream_result_unit},
    {"[method]output-stream.subscribe",
     output_self_param, 1u, &type_pollable},
    {"[method]output-stream.write-zeroes",
     output_len_params, 2u, &type_stream_result_unit},
    {"[method]output-stream.blocking-write-zeroes-and-flush",
     output_len_params, 2u, &type_stream_result_unit},
    {"[method]output-stream.splice",
     output_splice_params, 3u, &type_stream_result_u64},
    {"[method]output-stream.blocking-splice",
     output_splice_params, 3u, &type_stream_result_u64}
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
    },
    {
        "wasi:filesystem", "types", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-filesystem",
        "971b11617b50e7496bea85f36e60141bda172964",
        filesystem_types_functions, 6u
    },
    {
        "wasi:filesystem", "preopens", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-filesystem",
        "971b11617b50e7496bea85f36e60141bda172964",
        preopens_functions, 1u
    },
    {
        "wasi:io", "poll", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-io",
        "3983fe1feab6b3a3b4e5c47c8b13daaf22266f00",
        poll_functions, 3u
    },
    {
        "wasi:io", "error", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-io",
        "3983fe1feab6b3a3b4e5c47c8b13daaf22266f00",
        io_error_functions, 1u
    },
    {
        "wasi:io", "streams", TW_WASI02_V028,
        "https://github.com/WebAssembly/wasi-io",
        "3983fe1feab6b3a3b4e5c47c8b13daaf22266f00",
        streams_functions, 15u
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
