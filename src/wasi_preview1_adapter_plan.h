/* generated static adapter plan; do not edit */
#ifndef turbowasm_wasi_preview1_adapter_plan_generated_h
#define turbowasm_wasi_preview1_adapter_plan_generated_h

#include <stddef.h>
#include <stdint.h>

typedef enum turbowasm_wasi_preview1_adapter_carrier {
  turbowasm_wasi_preview1_adapter_carrier_void = 0,
  turbowasm_wasi_preview1_adapter_carrier_u32 = 1,
  turbowasm_wasi_preview1_adapter_carrier_u64
} turbowasm_wasi_preview1_adapter_carrier;

typedef struct turbowasm_wasi_preview1_adapter_param_plan {
  const char *name;
  uint32_t flags;
  turbowasm_wasi_preview1_adapter_carrier carrier;
} turbowasm_wasi_preview1_adapter_param_plan;

typedef struct turbowasm_wasi_preview1_adapter_function_plan {
  size_t source_ordinal;
  const char *function_name;
  const turbowasm_wasi_preview1_adapter_param_plan *params;
  size_t param_count;
  turbowasm_wasi_preview1_adapter_carrier return_carrier;
  uint32_t effects;
  uint32_t properties;
} turbowasm_wasi_preview1_adapter_function_plan;

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_0[] = {
  {"argc", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"argv_buf_size", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_1[] = {
  {"argv", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"argv_buf", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_2[] = {
  {"environ_count", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"environ_buf_size", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_3[] = {
  {"environ", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"environ_buf", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_4[] = {
  {"clock_id", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"precision", 1u, turbowasm_wasi_preview1_adapter_carrier_u64},
  {"timestamp", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_5[] = {
  {"buffer", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"buffer_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_6[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"iovs", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"iovs_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"written", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_7[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"iovs", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"iovs_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"read_count", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_8[] = {
  {"exit_code", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_9[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_10[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"prestat", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_11[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_12[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_13[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_14[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_15[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"offset", 1u, turbowasm_wasi_preview1_adapter_carrier_u64},
  {"whence", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"new_offset", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_16[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"buffer", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"buffer_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"cookie", 1u, turbowasm_wasi_preview1_adapter_carrier_u64},
  {"bufused", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_17[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"offset", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_18[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"filestat", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_19[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"lookup_flags", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"filestat", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_param_plan turbowasm_wasi_preview1_adapter_params_20[] = {
  {"fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"dirflags", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"path_length", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"oflags", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"rights_base", 1u, turbowasm_wasi_preview1_adapter_carrier_u64},
  {"rights_inheriting", 1u, turbowasm_wasi_preview1_adapter_carrier_u64},
  {"fdflags", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
  {"opened_fd", 1u, turbowasm_wasi_preview1_adapter_carrier_u32},
};

static const turbowasm_wasi_preview1_adapter_function_plan turbowasm_wasi_preview1_adapter_functions[] = {
  {0u, "args_sizes_get", turbowasm_wasi_preview1_adapter_params_0, 2u, turbowasm_wasi_preview1_adapter_carrier_u32, 9u, 0u},
  {1u, "args_get", turbowasm_wasi_preview1_adapter_params_1, 2u, turbowasm_wasi_preview1_adapter_carrier_u32, 9u, 0u},
  {2u, "environ_sizes_get", turbowasm_wasi_preview1_adapter_params_2, 2u, turbowasm_wasi_preview1_adapter_carrier_u32, 9u, 0u},
  {3u, "environ_get", turbowasm_wasi_preview1_adapter_params_3, 2u, turbowasm_wasi_preview1_adapter_carrier_u32, 9u, 0u},
  {4u, "clock_time_get", turbowasm_wasi_preview1_adapter_params_4, 3u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {5u, "random_get", turbowasm_wasi_preview1_adapter_params_5, 2u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {6u, "fd_write", turbowasm_wasi_preview1_adapter_params_6, 4u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {7u, "fd_read", turbowasm_wasi_preview1_adapter_params_7, 4u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {8u, "proc_exit", turbowasm_wasi_preview1_adapter_params_8, 1u, turbowasm_wasi_preview1_adapter_carrier_void, 9u, 0u},
  {9u, "fd_close", turbowasm_wasi_preview1_adapter_params_9, 1u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {10u, "fd_prestat_get", turbowasm_wasi_preview1_adapter_params_10, 2u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {11u, "fd_prestat_dir_name", turbowasm_wasi_preview1_adapter_params_11, 3u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {12u, "path_create_directory", turbowasm_wasi_preview1_adapter_params_12, 3u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {13u, "path_remove_directory", turbowasm_wasi_preview1_adapter_params_13, 3u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {14u, "path_unlink_file", turbowasm_wasi_preview1_adapter_params_14, 3u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {15u, "fd_seek", turbowasm_wasi_preview1_adapter_params_15, 4u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {16u, "fd_readdir", turbowasm_wasi_preview1_adapter_params_16, 5u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {17u, "fd_tell", turbowasm_wasi_preview1_adapter_params_17, 2u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {18u, "fd_filestat_get", turbowasm_wasi_preview1_adapter_params_18, 2u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {19u, "path_filestat_get", turbowasm_wasi_preview1_adapter_params_19, 5u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
  {20u, "path_open", turbowasm_wasi_preview1_adapter_params_20, 9u, turbowasm_wasi_preview1_adapter_carrier_u32, 12u, 0u},
};

static const size_t turbowasm_wasi_preview1_adapter_function_count = 21u;

#endif /* turbowasm_wasi_preview1_adapter_plan_generated_h */
