#include "tinytest.h"
#include "command.h"
#include <cmeta_fs.h>
#include <string.h>

static tw_command_options options;
static FILE *streams[3];
static char root[1024];
static const char *args[] = {"guest-c11", NULL, "argument"};
static const char *environment[] = {"C11_TEST=value"};

static tw_command_result run(const char *mode) {
    args[1] = mode;
    return tw_command_run(&options);
}
static void run_buffering(const char *mode) {
    options.module_path = GUEST_BUFFERING_PATH;
    options.directory = root;
    tw_command_result r = run(mode);
    if (r.status != TURBOWASM_OK || r.exit_code) {
        char message[1024]; rewind(streams[2]);
        size_t n = fread(message, 1, sizeof(message) - 1, streams[2]); message[n] = 0;
        info("buffer guest stderr: %s", message);
    }
    check_equal(r.status, TURBOWASM_OK); check_true(r.exited); check_equal(r.exit_code, 0u);
}
static void check_buffer_file(const char *expected) {
    char path[1200], text[32] = {0};
    check_equal(cmeta_fs_path_join(path, sizeof(path), root, "buffer"), 0);
    FILE *f = fopen(path, "rb"); check_not_null(f);
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    int error = ferror(f); fclose(f);
    check_equal(error, 0); check_equal(n, strlen(expected)); check_equal(text, expected);
}
static void output(char *buffer, size_t size) {
    rewind(streams[1]);
    size_t n = fread(buffer, 1, size - 1, streams[1]);
    buffer[n] = 0;
}
static void diagnostics(void) {
    char buffer[1024];
    rewind(streams[2]);
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, streams[2]);
    buffer[n] = 0;
    info("guest stderr: %s", buffer);
}

spec("local Metallic C11 command guests") {
    before_all() {
        char temp[768];
        check_equal(cmeta_fs_get_tmpdir(temp, sizeof(temp)), 0);
        check_equal(cmeta_fs_path_join(root, sizeof(root), temp, "turbowasm-metallic-c11"), 0);
        check_equal(cmeta_fs_mkdir(root, 0700), 0);
    }
    before_each() {
        for (int i = 0; i < 3; ++i) { streams[i] = tmpfile(); check_not_null(streams[i]); }
        options = (tw_command_options){0};
        options.module_path = GUEST_C11_PATH;
        options.args = args; options.arg_count = 3;
        options.environment = environment; options.environment_count = 1;
        options.memory_bytes = 16u * 1024u * 1024u; options.fuel = UINT64_C(100000000);
        memcpy(options.streams, streams, sizeof(streams));
    }
    after_each() {
        for (int i = 0; i < 3; ++i) { if (streams[i]) fclose(streams[i]); streams[i] = NULL; }
        const char *names[] = {"first", "second", "oom", "buffer", "buffer2"};
        char path[1200];
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (!cmeta_fs_path_join(path, sizeof(path), root, names[i]) &&
                !cmeta_fs_access(path, SALTS_FS_ACCESS_EXISTS))
                (void)cmeta_fs_unlink(path);
    }
    after_all() {
        /* Fails if tmpfile left any named entry behind. */
        check_equal(cmeta_fs_rmdir(root), 0);
    }
    it("returns through exit and runs atexit in reverse order") {
        tw_command_result r = run("exit"); char text[64]; output(text, sizeof(text));
        check_equal(r.status, TURBOWASM_OK); check_true(r.exited); check_equal(r.exit_code, 23u);
        check_equal(text, "BA");
    }
    it("quick_exit runs only quick handlers") {
        tw_command_result r = run("quick"); char text[64]; output(text, sizeof(text));
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 24u); check_equal(text, "DC");
    }
    it("_Exit bypasses all handlers") {
        tw_command_result r = run("immediate"); char text[64]; output(text, sizeof(text));
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 25u); check_equal(text, "");
    }
    it("keeps argv alive through exit and grants only supplied environment") {
        tw_command_result r = run("args"); diagnostics(); char text[64]; output(text, sizeof(text));
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u); check_equal(text, "args-live");
    }
    it("executes C11 allocation, conversion, formatting and time functions") {
        tw_command_result r = run("library"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("renames files, toggles append and closes anonymous temporary files") {
        options.directory = root;
        tw_command_result r = run("files"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("recovers from heap exhaustion without leaking fopen descriptors") {
        options.directory = root; options.memory_bytes = 1024u * 1024u;
        tw_command_result r = run("allocation"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("denies filesystem access without an admitted directory") {
        tw_command_result r = run("no-directory"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("passes stdio bytes and closes guest streams without freeing host streams") {
        check_equal(fwrite("hello", 1, 5, streams[0]), 5u); rewind(streams[0]);
        tw_command_result r = run("streams"); diagnostics(); char text[64]; output(text, sizeof(text));
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u); check_equal(text, "hello");
        check_equal(fseek(streams[1], 0, SEEK_SET), 0);
    }
    it("reports fuel exhaustion separately from proc_exit") {
        options.fuel = 1000;
        tw_command_result r = run("loop");
        check_equal(r.status, TURBOWASM_FUEL_EXHAUSTED); check_false(r.exited);
    }
    it("executes every 128-bit shift count against an independent bit oracle") {
        options.module_path = GUEST_NUMERIC_PATH;
        tw_command_result r = run("shifts"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("honors null-input semantics in restartable C11 character conversions") {
        tw_command_result r = run("unicode-null"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("preserves UTF-16 surrogate pairs and rejects invalid sequences") {
        tw_command_result r = run("unicode-surrogates"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("keeps implicit character state independent and resumes bounded strings") {
        tw_command_result r = run("unicode-restart"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("truncates binary128 to integers without intermediate double rounding") {
        options.module_path = GUEST_NUMERIC_PATH;
        tw_command_result r = run("conversions"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("rounds binary128 division including subnormal halfway values") {
        options.module_path = GUEST_NUMERIC_PATH;
        tw_command_result r = run("division"); diagnostics();
        check_equal(r.status, TURBOWASM_OK); check_equal(r.exit_code, 0u);
    }
    it("buffers complete blocks and flushes borrowed output on close") { run_buffering("full"); }
    it("flushes line endings and all pending streams") { run_buffering("line"); }
    it("tracks read-ahead, pushback, positions and EOF") { run_buffering("input"); }
    it("synchronizes update streams and releases buffers on reopen") { run_buffering("update"); }
    it("reports allocation and flush failures without retaining closed buffers") {
        options.memory_bytes = 1024u * 1024u;
        run_buffering("errors");
    }
    it("flushes a line-buffered prompt before unbuffered input") {
        check_equal(fwrite("x", 1, 1, streams[0]), 1u); rewind(streams[0]);
        run_buffering("prompt"); char text[32]; output(text, sizeof(text)); check_equal(text, "prompt");
    }
    it("flushes all files after normal exit handlers") {
        run_buffering("exit"); check_buffer_file("file!");
        char text[32]; output(text, sizeof(text)); check_equal(text, "exit");
    }
    it("quick_exit leaves pending buffers unflushed") { run_buffering("quick"); check_buffer_file(""); }
    it("_Exit leaves pending buffers unflushed") { run_buffering("immediate"); check_buffer_file(""); }
    it("retries only unwritten bytes after short writes and transient errors") {
        options.module_path = GUEST_STDIO_FAULTS_PATH;
        tw_command_result r = run("write");
        check_equal(r.status, TURBOWASM_OK); check_true(r.exited); check_equal(r.exit_code, 0u);
    }
    it("does not treat short reads as EOF") {
        options.module_path = GUEST_STDIO_FAULTS_PATH;
        tw_command_result r = run("read");
        check_equal(r.status, TURBOWASM_OK); check_true(r.exited); check_equal(r.exit_code, 0u);
    }
    it("reports partial fwrite progress when filling the buffer triggers an error") {
        options.module_path = GUEST_STDIO_FAULTS_PATH;
        tw_command_result r = run("write-fill");
        check_equal(r.status, TURBOWASM_OK); check_true(r.exited); check_equal(r.exit_code, 0u);
    }
    it("recovers from read errors without inventing EOF or skipping bytes") {
        options.module_path = GUEST_STDIO_FAULTS_PATH;
        tw_command_result r = run("read-error");
        check_equal(r.status, TURBOWASM_OK); check_true(r.exited); check_equal(r.exit_code, 0u);
    }
}
