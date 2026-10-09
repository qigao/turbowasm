#ifndef TURBOWASM_COMMAND_H
#define TURBOWASM_COMMAND_H
#include <turbowasm/turbowasm.h>
#include <stdio.h>

/* Internal runner contract. All paths, arrays and streams are borrowed until
 * return. No ambient environment or directory is inherited by the guest. */
typedef struct tw_command_options {
    const char *module_path;
    const char *directory;
    const char *const *args;
    size_t arg_count;
    const char *const *environment;
    size_t environment_count;
    FILE *streams[3];
    size_t memory_bytes;
    uint64_t fuel;
} tw_command_options;

typedef struct tw_command_result {
    turbowasm_status status;
    turbowasm_trap trap;
    bool exited;
    uint32_t exit_code;
    const char *phase;
} tw_command_result;

tw_command_result tw_command_run(const tw_command_options *options);
#endif
