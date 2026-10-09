#include "command.h"
#include <tlog.h>
#include <string.h>

static bool number(const char *s, uint64_t maximum, uint64_t *out) {
    uint64_t value = 0;
    if (!s || !*s) return false;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || value > (maximum - (uint64_t)(*s - '0')) / 10)
            return false;
        value = value * 10 + (uint64_t)(*s - '0');
    }
    if (!value) return false;
    *out = value;
    return true;
}
static int usage(void) {
    puts("usage: turbowasm-run [--dir absolute-host-directory] [--env KEY=VALUE]"
         " [--memory-pages N] [--fuel N] [--] module.wasm [guest arguments...]\n"
         "Defaults: 256 memory pages, 100000000 instructions; no guest directory or environment.");
    return 2;
}
int main(int argc, char **argv) {
    const char *environment[64];
    tw_command_options options = {0};
    options.memory_bytes = 16u * 1024u * 1024u;
    options.fuel = UINT64_C(100000000);
    options.streams[0] = stdin; options.streams[1] = stdout; options.streams[2] = stderr;
    options.environment = environment;
    int i = 1;
    for (; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help")) { usage(); return 0; }
        if (!strcmp(arg, "--")) { ++i; break; }
        if (arg[0] != '-') break;
        if (i + 1 == argc) return usage();
        const char *value = argv[++i];
        if (!strcmp(arg, "--dir")) {
            if (options.directory) return usage();
            options.directory = value;
        } else if (!strcmp(arg, "--env")) {
            const char *equal = strchr(value, '=');
            if (!equal || equal == value || options.environment_count == 64) return usage();
            environment[options.environment_count++] = value;
        } else if (!strcmp(arg, "--fuel")) {
            if (!number(value, UINT64_MAX, &options.fuel)) return usage();
        } else if (!strcmp(arg, "--memory-pages")) {
            uint64_t pages;
            if (!number(value, 16384, &pages)) return usage();
            options.memory_bytes = (size_t)pages * 65536u;
        } else return usage();
    }
    if (i == argc) return usage();
    options.module_path = argv[i];
    options.args = (const char *const *)(argv + i);
    options.arg_count = (size_t)(argc - i);
    tlog_t *logger = tlog_create(NULL);
    if (!logger) return 125;
    cmeta_console_sink_opts_t sink_options = { .output = stderr, .use_colors = 0, .pattern = "{message}" };
    cmeta_log_sink_t *sink = cmeta_sink_console_create(&sink_options);
    if (!sink || tlog_add_sink(logger, sink)) {
        if (sink) cmeta_sink_destroy(sink);
        tlog_destroy(logger);
        return 125;
    }
    tw_command_result result = tw_command_run(&options);
    if (result.status != TURBOWASM_OK)
        SALTS_LOG_ERRORF(logger, "turbowasm-run", "{}: {} (trap: {})", result.phase,
            turbowasm_status_string(result.status), turbowasm_trap_string(result.trap));
    tlog_destroy(logger);
    if (result.status != TURBOWASM_OK) return 125;
    /* Process exit status is the portable low eight bits of Preview1's u32. */
    return result.exited ? (int)(result.exit_code & 255u) : 0;
}
