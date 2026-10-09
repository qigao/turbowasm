#include "tinytest.h"
#include <turbowasm/wasi_host_fs.h>
#include <cmeta_fs.h>
#include <salts/thread.h>
#include <tstr.h>
#include <string.h>

enum { WORKERS = 8, RECORDS = 128, RECORD_SIZE = 8, OPEN_ROUNDS = 32 };
typedef struct barrier {
    cmeta_mutex_t mutex;
    cmeta_cond_t cond;
    unsigned arrivals, phase;
} barrier;
typedef struct task {
    unsigned id, mode;
    uint32_t error;
    turbowasm_wasi_fs_file file;
    uint8_t bytes[RECORDS * RECORD_SIZE];
    turbowasm_wasi_fs_dirent first;
} task;
static barrier gate;
static task tasks[WORKERS];
static cmeta_thread_t threads[WORKERS];
static turbowasm_wasi_host_fs adapter;
static turbowasm_wasi_fs_provider provider;
static turbowasm_wasi_fs_file root, data_file;
static tstr directory;

static void rendezvous(void) {
    cmeta_mutex_lock(&gate.mutex);
    unsigned phase = gate.phase;
    if (++gate.arrivals == WORKERS + 1) {
        gate.arrivals = 0; ++gate.phase; cmeta_cond_broadcast(&gate.cond);
    } else while (phase == gate.phase) cmeta_cond_wait(&gate.cond, &gate.mutex);
    cmeta_mutex_unlock(&gate.mutex);
}
static uint32_t open_path(const char *path, uint32_t oflags, turbowasm_wasi_fs_file *out) {
    return provider.path_open(provider.context, root, 0, (const uint8_t *)path,
        strlen(path), oflags, TURBOWASM_WASI_RIGHT_FD_READ | TURBOWASM_WASI_RIGHT_FD_WRITE, 0, 0, out);
}
static void run(void *context) {
    task *t = context;
    if (t->mode == 4) {
        rendezvous();
        t->error = open_path("child", 2, &t->file);
        rendezvous(); rendezvous();
        if (!t->error) t->error = provider.close(provider.context, t->file);
        return;
    }
    if (t->mode == 3) {
        for (unsigned round = 0; round < OPEN_ROUNDS; ++round) {
            rendezvous();
            t->error = open_path("data", 0, &t->file);
            rendezvous(); /* all identities held while controller checks capacity */
            rendezvous();
            if (!t->error) t->error = provider.close(provider.context, t->file);
            rendezvous();
        }
        return;
    }
    rendezvous();
    for (unsigned n = 0; n < RECORDS && !t->error; ++n) {
        uint32_t count = 0;
        uint8_t *bytes = t->bytes + n * RECORD_SIZE;
        if (t->mode == 0) {
            for (unsigned j = 0; j < 4; ++j) bytes[j] = bytes[j + 4] = (uint8_t)(j % 2 ? n : t->id);
            turbowasm_wasi_const_buffer buffers[] = {{bytes, 4}, {bytes + 4, 4}};
            t->error = provider.write(provider.context, data_file, buffers, 2, &count);
            if (!t->error && count != RECORD_SIZE) t->error = TURBOWASM_WASI_ERRNO_IO;
        } else if (t->mode == 1) {
            turbowasm_wasi_buffer buffers[] = {{bytes, 4}, {bytes + 4, 4}};
            t->error = provider.read(provider.context, data_file, buffers, 2, &count);
            if (!t->error && count != RECORD_SIZE) t->error = TURBOWASM_WASI_ERRNO_IO;
        } else {
            turbowasm_wasi_fs_dirent entry = {0};
            bool has_entry = false;
            t->error = provider.readdir(provider.context, root, 0, &entry, &has_entry);
            if (!t->error && (!has_entry || !entry.name_length)) t->error = TURBOWASM_WASI_ERRNO_IO;
            if (!n) t->first = entry;
            else if (!t->error && (entry.name_length != t->first.name_length ||
                entry.next_cookie != t->first.next_cookie ||
                memcmp(entry.name, t->first.name, entry.name_length))) t->error = TURBOWASM_WASI_ERRNO_IO;
        }
    }
}
static void start_tasks(unsigned mode) {
    for (unsigned i = 0; i < WORKERS; ++i) {
        tasks[i] = (task){.id = i + 1, .mode = mode};
        check_equal(cmeta_thread_create(&threads[i], run, &tasks[i]), SALTS_OK);
    }
}
static void join_tasks(void) {
    for (unsigned i = 0; i < WORKERS; ++i) {
        check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
        cmeta_thread_destroy(&threads[i]);
        check_equal(tasks[i].error, 0u);
    }
}

spec("HostFS concurrent native identity ownership") {
    before_each() {
        gate = (barrier){0}; adapter = (turbowasm_wasi_host_fs){0};
        data_file = (turbowasm_wasi_fs_file){0};
        cmeta_mutex_init(&gate.mutex); cmeta_cond_init(&gate.cond);
        check(gate.mutex != NULL); check(gate.cond != NULL);
        tstr temp = tstr_new_len(NULL, 2047);
        directory = tstr_new_len(NULL, 2047);
        check(temp != NULL); check(directory != NULL);
        check_equal(cmeta_fs_get_tmpdir(temp, 2048), 0);
        check_equal(cmeta_fs_path_join(directory, 2048, temp, "turbowasm_host_concurrency"), 0);
        tstr_free(temp);
        check_equal(cmeta_fs_mkdir(directory, 0755), 0);
        turbowasm_wasi_host_fs_config config = {.host_root = directory, .file_capacity = 4, .path_capacity = 256};
        check_equal(turbowasm_wasi_host_fs_init(&adapter, &config), TURBOWASM_OK);
        check(turbowasm_wasi_host_fs_provider(&adapter, &provider, &root));
        check_equal(open_path("data", 1, &data_file), 0u);
    }
    after_each() {
        if (data_file.generation) check_equal(provider.close(provider.context, data_file), 0u);
        check_equal(provider.path_unlink_file(provider.context, root, (const uint8_t *)"data", 4), 0u);
        check_equal(provider.close(provider.context, root), 0u);
        check_equal(turbowasm_wasi_host_fs_destroy(&adapter), TURBOWASM_OK);
        check_equal(cmeta_fs_rmdir(directory), 0);
        tstr_free(directory);
        cmeta_cond_destroy(&gate.cond); cmeta_mutex_destroy(&gate.mutex);
    }
    it("serializes whole read and append write vectors on a shared file position") {
        check(provider.set_flags != NULL);
        check_equal(provider.set_flags(provider.context, data_file, 1), 0u);
        start_tasks(0); rendezvous(); join_tasks();
        uint64_t position;
        check_equal(provider.tell(provider.context, data_file, &position), 0u);
        check_equal(position, (uint64_t)(WORKERS * RECORDS * RECORD_SIZE));
        check_equal(provider.set_flags(provider.context, data_file, 0), 0u);
        check_equal(provider.seek(provider.context, data_file, 0, TURBOWASM_WASI_WHENCE_SET, &position), 0u);
        start_tasks(1); rendezvous(); join_tasks();
        unsigned seen[WORKERS][RECORDS] = {{0}};
        for (unsigned i = 0; i < WORKERS; ++i) for (unsigned n = 0; n < RECORDS; ++n) {
            const uint8_t *bytes = tasks[i].bytes + n * RECORD_SIZE;
            check_equal(bytes, bytes + 4, 4);
            check_equal(bytes[0], bytes[2]); check_equal(bytes[1], bytes[3]);
            check(bytes[0] >= 1 && bytes[0] <= WORKERS); check(bytes[1] < RECORDS);
            if (bytes[0] >= 1 && bytes[0] <= WORKERS && bytes[1] < RECORDS) ++seen[bytes[0] - 1][bytes[1]];
        }
        for (unsigned i = 0; i < WORKERS; ++i) for (unsigned n = 0; n < RECORDS; ++n) check_equal(seen[i][n], 1u);
    }
    it("initializes the root cursor once and serializes repeated cookie repositioning") {
        start_tasks(2); rendezvous(); join_tasks();
        for (unsigned i = 1; i < WORKERS; ++i) {
            check_equal(tasks[i].first.name_length, tasks[0].first.name_length);
            check_equal(tasks[i].first.name, tasks[0].first.name, tasks[0].first.name_length);
        }
    }
    it("reserves bounded native slots atomically and rejects stale identities after reuse") {
        turbowasm_wasi_fs_file stale = data_file;
        check_equal(provider.close(provider.context, data_file), 0u);
        data_file = (turbowasm_wasi_fs_file){0};
        start_tasks(3);
        for (unsigned round = 0; round < OPEN_ROUNDS; ++round) {
            rendezvous(); rendezvous();
            unsigned admitted = 0;
            for (unsigned i = 0; i < WORKERS; ++i) {
                if (!tasks[i].error) {
                    ++admitted;
                    for (unsigned j = 0; j < i; ++j)
                        if (!tasks[j].error) check_not_equal(tasks[i].file.object, tasks[j].file.object);
                } else check_equal(tasks[i].error, TURBOWASM_WASI_ERRNO_MFILE);
            }
            check_equal(admitted, 4u);
            check_equal(provider.close(provider.context, stale), TURBOWASM_WASI_ERRNO_BADF);
            rendezvous(); rendezvous();
            for (unsigned i = 0; i < WORKERS; ++i) {
                check(tasks[i].error == 0 || tasks[i].error == TURBOWASM_WASI_ERRNO_MFILE);
            }
        }
        for (unsigned i = 0; i < WORKERS; ++i) {
            check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
            cmeta_thread_destroy(&threads[i]);
        }
    }
    it("keeps rename admission closed until concurrent child directory owners release") {
        check(provider.path_rename != NULL);
        check_equal(provider.path_create_directory(provider.context, root, (const uint8_t *)"child", 5), 0u);
        start_tasks(4); rendezvous(); rendezvous();
        unsigned admitted = 0;
        for (unsigned i = 0; i < WORKERS; ++i) {
            if (!tasks[i].error) ++admitted;
            else check_equal(tasks[i].error, TURBOWASM_WASI_ERRNO_MFILE);
        }
        check_equal(admitted, 3u);
        check_equal(provider.path_rename(provider.context, root, (const uint8_t *)"data", 4,
            root, (const uint8_t *)"renamed", 7), TURBOWASM_WASI_ERRNO_BUSY);
        rendezvous();
        for (unsigned i = 0; i < WORKERS; ++i) {
            check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
            cmeta_thread_destroy(&threads[i]);
            check(tasks[i].error == 0 || tasks[i].error == TURBOWASM_WASI_ERRNO_MFILE);
        }
        check_equal(provider.path_rename(provider.context, root, (const uint8_t *)"data", 4,
            root, (const uint8_t *)"renamed", 7), 0u);
        check_equal(provider.path_rename(provider.context, root, (const uint8_t *)"renamed", 7,
            root, (const uint8_t *)"data", 4), 0u);
        check_equal(provider.path_remove_directory(provider.context, root, (const uint8_t *)"child", 5), 0u);
    }
}
