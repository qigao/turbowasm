#include <turbowasm/turbowasm.h>

#include "instance_internal.h"

#include <salts/thread.h>

#ifdef TURBOWASM_SPEC_ENABLE_MIR
#include "jit/mir_backend.h"
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct spec_slot {
    uint8_t *bytes;
    size_t size;
    turbowasm_module module;
    turbowasm_instance instance;
    bool loaded;
    bool ready;
    bool unsupported;
    bool borrowed;
} spec_slot;

typedef struct spec_state spec_state;

typedef struct spec_thread {
    char *name_hex;
    char *manifest_path;
    salts_thread_t handle;
    spec_state *child;
    int rc;
    bool joined;
    bool aggregated;
} spec_thread;

struct spec_state {
    spec_slot **slots;
    size_t slot_count;

    spec_thread **threads;
    size_t thread_count;
    size_t thread_capacity;

    /*
     * WebAssembly store allocations created by assertions that trap during
     * instantiation. Imported memory/table side effects remain observable,
     * and funcrefs written into imported tables may point back at these
     * otherwise unnamed instances.
     */
    spec_slot **retained_failures;
    size_t retained_failure_count;
    size_t retained_failure_capacity;

    turbowasm_linker linker;
    turbowasm_module spectest_module;
    turbowasm_instance spectest_instance;
    bool spectest_ready;
    int64_t current_slot;
    size_t passed;
    size_t failed;
    size_t unsupported;
    bool printed_first_failure;
    bool printed_first_unsupported;
    bool printed_first_runtime_unsupported;
};

static int spec_run_manifest(spec_state *state, const char *path);
static bool spec_state_init(spec_state *state);
static void spec_state_destroy(spec_state *state);

typedef enum spec_expected_pattern {
    SPEC_EXPECT_EXACT = 0,
    SPEC_EXPECT_NAN_CANONICAL,
    SPEC_EXPECT_NAN_ARITHMETIC,
    SPEC_EXPECT_FUNCREF_NONNULL
} spec_expected_pattern;

typedef struct spec_expected_value {
    turbowasm_value value;
    spec_expected_pattern pattern;

    /*
     * v128 expectations may carry a NaN pattern per floating-point lane.
     * Integer lanes and ordinary floating lanes remain exact bit patterns.
     */
    uint8_t v128_lane_bits;
    uint8_t v128_lane_count;
    spec_expected_pattern v128_lane_patterns[16];
} spec_expected_value;

/*
 * Canonical WebAssembly spec-test host module.
 *
 * This is a normal Wasm provider instance registered as "spectest" through
 * the public linker. The harness therefore exercises the same import/export
 * matching and shared state paths as ordinary linked modules instead of
 * adding test-only host extern APIs.
 */
static const uint8_t spec_spectest_module_bytes[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x1e, 0x07, 0x60,
    0x00, 0x00, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x01, 0x7e, 0x00, 0x60, 0x01,
    0x7d, 0x00, 0x60, 0x01, 0x7c, 0x00, 0x60, 0x02, 0x7f, 0x7d, 0x00, 0x60,
    0x02, 0x7c, 0x7c, 0x00, 0x03, 0x08, 0x07, 0x00, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x04, 0x05, 0x01, 0x70, 0x01, 0x0a, 0x14, 0x05, 0x04, 0x01,
    0x01, 0x01, 0x02, 0x06, 0x21, 0x04, 0x7f, 0x00, 0x41, 0x9a, 0x05, 0x0b,
    0x7e, 0x00, 0x42, 0x9a, 0x05, 0x0b, 0x7d, 0x00, 0x43, 0x66, 0xa6, 0x26,
    0x44, 0x0b, 0x7c, 0x00, 0x44, 0xcd, 0xcc, 0xcc, 0xcc, 0xcc, 0xd4, 0x84,
    0x40, 0x0b, 0x07, 0x9e, 0x01, 0x0d, 0x0a, 0x67, 0x6c, 0x6f, 0x62, 0x61,
    0x6c, 0x5f, 0x69, 0x33, 0x32, 0x03, 0x00, 0x0a, 0x67, 0x6c, 0x6f, 0x62,
    0x61, 0x6c, 0x5f, 0x69, 0x36, 0x34, 0x03, 0x01, 0x0a, 0x67, 0x6c, 0x6f,
    0x62, 0x61, 0x6c, 0x5f, 0x66, 0x33, 0x32, 0x03, 0x02, 0x0a, 0x67, 0x6c,
    0x6f, 0x62, 0x61, 0x6c, 0x5f, 0x66, 0x36, 0x34, 0x03, 0x03, 0x05, 0x74,
    0x61, 0x62, 0x6c, 0x65, 0x01, 0x00, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72,
    0x79, 0x02, 0x00, 0x05, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x00, 0x00, 0x09,
    0x70, 0x72, 0x69, 0x6e, 0x74, 0x5f, 0x69, 0x33, 0x32, 0x00, 0x01, 0x09,
    0x70, 0x72, 0x69, 0x6e, 0x74, 0x5f, 0x69, 0x36, 0x34, 0x00, 0x02, 0x09,
    0x70, 0x72, 0x69, 0x6e, 0x74, 0x5f, 0x66, 0x33, 0x32, 0x00, 0x03, 0x09,
    0x70, 0x72, 0x69, 0x6e, 0x74, 0x5f, 0x66, 0x36, 0x34, 0x00, 0x04, 0x0d,
    0x70, 0x72, 0x69, 0x6e, 0x74, 0x5f, 0x69, 0x33, 0x32, 0x5f, 0x66, 0x33,
    0x32, 0x00, 0x05, 0x0d, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x5f, 0x66, 0x36,
    0x34, 0x5f, 0x66, 0x36, 0x34, 0x00, 0x06, 0x0a, 0x16, 0x07, 0x02, 0x00,
    0x0b, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b, 0x02, 0x00,
    0x0b, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

#ifdef TURBOWASM_SPEC_ENABLE_MIR
static turbowasm_status spec_attach_mir(turbowasm_instance *instance) {
    turbowasm_jit_backend backend = {0};
    turbowasm_instance_impl *impl;
    turbowasm_status status;

    if (instance == NULL || instance->impl == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    status = turbowasm_mir_backend_create(&backend);
    if (status != TURBOWASM_OK)
        return status;

    impl = (turbowasm_instance_impl *)instance->impl;
    status = turbowasm_jit_instance_attach_backend(
        impl, &backend, 1u);
    if (status != TURBOWASM_OK && backend.context != NULL &&
        backend.destroy_backend != NULL) {
        backend.destroy_backend(backend.context);
        backend.context = NULL;
    }
    return status;
}

static void spec_collect_mir_stats(
    const spec_state *state,
    size_t *compiled,
    size_t *interpret_only,
    size_t *cold,
    uint64_t *calls) {
    size_t slot_index;

    if (state == NULL || compiled == NULL ||
        interpret_only == NULL || cold == NULL || calls == NULL)
        return;

    for (slot_index = 0u;
         slot_index < state->slot_count;
         ++slot_index) {
        const spec_slot *slot = state->slots[slot_index];
        const turbowasm_instance_impl *impl;
        uint32_t function_index;

        if (slot == NULL || !slot->ready ||
            slot->instance.impl == NULL)
            continue;

        impl = (const turbowasm_instance_impl *)slot->instance.impl;
        if (!impl->jit_backend_attached ||
            impl->jit_functions == NULL)
            continue;

        for (function_index = 0u;
             function_index < impl->jit_function_count;
             ++function_index) {
            const turbowasm_jit_function_state *entry =
                &impl->jit_functions[function_index];

            *calls += entry->call_count;
            switch (entry->state) {
                case TURBOWASM_JIT_COMPILED:
                    ++*compiled;
                    break;
                case TURBOWASM_JIT_INTERPRET_ONLY:
                    ++*interpret_only;
                    break;
                case TURBOWASM_JIT_INTERPRET:
                default:
                    ++*cold;
                    break;
            }
        }
    }
}
#endif

static bool spec_init_spectest(spec_state *state) {
    static const uint8_t name_bytes[] = {
        's', 'p', 'e', 'c', 't', 'e', 's', 't'
    };
    turbowasm_name name = {
        name_bytes,
        (uint32_t)sizeof(name_bytes)
    };
    turbowasm_status status;

    if (state == NULL)
        return false;

    status = turbowasm_module_load_borrowed(
        &state->spectest_module,
        spec_spectest_module_bytes,
        sizeof(spec_spectest_module_bytes));
    if (status != TURBOWASM_OK)
        return false;

    status = turbowasm_instance_create(
        &state->spectest_instance,
        &state->spectest_module);
    if (status != TURBOWASM_OK) {
        turbowasm_module_destroy(&state->spectest_module);
        return false;
    }

    status = turbowasm_linker_define_instance(
        &state->linker, name, &state->spectest_instance);
    if (status != TURBOWASM_OK) {
        turbowasm_instance_destroy(&state->spectest_instance);
        turbowasm_module_destroy(&state->spectest_module);
        return false;
    }

    state->spectest_ready = true;
    return true;
}

static void spec_note_failure(spec_state *state,
                              unsigned line,
                              const char *message) {
    ++state->failed;
    if (!state->printed_first_failure) {
        fprintf(stderr, "FIRST_FAIL line=%u %s\n", line, message);
        state->printed_first_failure = true;
    }
}

static void spec_note_unsupported(spec_state *state,
                                  unsigned line,
                                  const char *message) {
    ++state->unsupported;
    if (!state->printed_first_unsupported) {
        fprintf(stderr, "FIRST_UNSUPPORTED line=%u %s\n",
                line, message);
        state->printed_first_unsupported = true;
    }
}

static void spec_note_runtime_unsupported(spec_state *state,
                                          unsigned line,
                                          const char *message) {
    spec_note_unsupported(state, line, message);
    if (!state->printed_first_runtime_unsupported) {
        fprintf(stderr, "FIRST_RUNTIME_UNSUPPORTED line=%u %s\n",
                line, message);
        state->printed_first_runtime_unsupported = true;
    }
}

static void spec_note_pass(spec_state *state) {
    ++state->passed;
}

static unsigned char *spec_read_file(const char *path,
                                     size_t *size_out) {
    FILE *file;
    long length;
    unsigned char *bytes;

    if (path == NULL || size_out == NULL)
        return NULL;

    file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    bytes = (unsigned char *)malloc(
        length == 0 ? 1u : (size_t)length);
    if (bytes == NULL) {
        fclose(file);
        return NULL;
    }

    if (length != 0 &&
        fread(bytes, 1u, (size_t)length, file) !=
            (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }

    fclose(file);
    *size_out = (size_t)length;
    return bytes;
}

static void spec_slot_destroy(spec_slot *slot) {
    if (slot == NULL)
        return;
    if (!slot->borrowed) {
        turbowasm_instance_destroy(&slot->instance);
        turbowasm_module_destroy(&slot->module);
        free(slot->bytes);
    }
    memset(slot, 0, sizeof(*slot));
}

static bool spec_retain_failed_instantiation(
    spec_state *state,
    uint8_t **bytes,
    size_t size,
    turbowasm_module *module,
    turbowasm_instance *instance) {
    spec_slot **grown;
    spec_slot *slot;
    turbowasm_instance_impl *impl;
    size_t next;

    if (state == NULL || bytes == NULL || *bytes == NULL ||
        module == NULL || module->impl == NULL ||
        instance == NULL || instance->impl == NULL)
        return false;

    if (state->retained_failure_count ==
        state->retained_failure_capacity) {
        next = state->retained_failure_capacity == 0u
            ? 4u
            : state->retained_failure_capacity * 2u;
        if (next < state->retained_failure_capacity ||
            next > SIZE_MAX / sizeof(*grown))
            return false;

        grown = (spec_slot **)realloc(
            state->retained_failures,
            next * sizeof(*grown));
        if (grown == NULL)
            return false;

        memset(grown + state->retained_failure_capacity, 0,
               (next - state->retained_failure_capacity) *
                   sizeof(*grown));
        state->retained_failures = grown;
        state->retained_failure_capacity = next;
    }

    slot = (spec_slot *)calloc(1u, sizeof(*slot));
    if (slot == NULL)
        return false;

    slot->bytes = *bytes;
    slot->size = size;
    slot->module = *module;
    slot->instance = *instance;
    slot->loaded = true;

    /*
     * The instance was created against the stack-local module wrapper above.
     * Rebind that borrowed wrapper pointer to this stable heap slot before the
     * command returns.
     */
    impl = (turbowasm_instance_impl *)slot->instance.impl;
    impl->module = &slot->module;

    state->retained_failures[
        state->retained_failure_count++] = slot;

    *bytes = NULL;
    module->impl = NULL;
    instance->impl = NULL;
    return true;
}

static bool spec_ensure_slot(spec_state *state, size_t slot_index) {
    spec_slot **grown;
    size_t next;

    if (state == NULL)
        return false;

    if (slot_index >= state->slot_count) {
        next = state->slot_count == 0u ? 8u : state->slot_count;
        while (next <= slot_index) {
            if (next > SIZE_MAX / 2u)
                return false;
            next *= 2u;
        }

        if (next > SIZE_MAX / sizeof(*grown))
            return false;

        grown = (spec_slot **)realloc(
            state->slots, next * sizeof(*grown));
        if (grown == NULL)
            return false;

        memset(grown + state->slot_count, 0,
               (next - state->slot_count) * sizeof(*grown));
        state->slots = grown;
        state->slot_count = next;
    }

    if (state->slots[slot_index] == NULL) {
        state->slots[slot_index] =
            (spec_slot *)calloc(1u, sizeof(spec_slot));
        if (state->slots[slot_index] == NULL)
            return false;
    }

    return true;
}


static char *spec_strdup_text(const char *text) {
    size_t size;
    char *copy;

    if (text == NULL)
        return NULL;
    size = strlen(text) + 1u;
    copy = (char *)malloc(size);
    if (copy != NULL)
        memcpy(copy, text, size);
    return copy;
}

static bool spec_state_init(spec_state *state) {
    if (state == NULL)
        return false;

    memset(state, 0, sizeof(*state));
    state->current_slot = -1;

    if (turbowasm_linker_init(&state->linker) != TURBOWASM_OK)
        return false;
    if (!spec_init_spectest(state)) {
        turbowasm_linker_destroy(&state->linker);
        memset(state, 0, sizeof(*state));
        return false;
    }
    return true;
}

static spec_thread *spec_find_thread(
    spec_state *state,
    const char *name_hex) {
    size_t index;

    if (state == NULL || name_hex == NULL)
        return NULL;

    for (index = 0u; index < state->thread_count; ++index) {
        spec_thread *thread = state->threads[index];
        if (thread != NULL &&
            thread->name_hex != NULL &&
            strcmp(thread->name_hex, name_hex) == 0)
            return thread;
    }
    return NULL;
}

static bool spec_reserve_thread(spec_state *state) {
    spec_thread **grown;
    size_t next;

    if (state == NULL)
        return false;
    if (state->thread_count < state->thread_capacity)
        return true;

    next = state->thread_capacity == 0u
        ? 4u
        : state->thread_capacity * 2u;
    if (next < state->thread_capacity ||
        next > SIZE_MAX / sizeof(*grown))
        return false;

    grown = (spec_thread **)realloc(
        state->threads, next * sizeof(*grown));
    if (grown == NULL)
        return false;

    memset(
        grown + state->thread_capacity, 0,
        (next - state->thread_capacity) * sizeof(*grown));
    state->threads = grown;
    state->thread_capacity = next;
    return true;
}

static bool spec_parse_slot_index(
    const char *text,
    size_t *out) {
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out == NULL || *text == '\0')
        return false;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' ||
        value > (unsigned long long)SIZE_MAX)
        return false;

    *out = (size_t)value;
    return true;
}

static bool spec_bind_shared_slot(
    spec_state *parent,
    spec_state *child,
    size_t child_index,
    size_t parent_index) {
    const spec_slot *source;
    spec_slot *target;

    if (parent == NULL || child == NULL ||
        parent_index >= parent->slot_count ||
        parent->slots[parent_index] == NULL)
        return false;

    source = parent->slots[parent_index];
    if (!source->loaded || !source->ready || source->unsupported)
        return false;

    if (!spec_ensure_slot(child, child_index))
        return false;

    target = child->slots[child_index];
    spec_slot_destroy(target);
    target->size = source->size;
    target->module = source->module;
    target->instance = source->instance;
    target->loaded = source->loaded;
    target->ready = source->ready;
    target->unsupported = source->unsupported;
    target->borrowed = true;
    return true;
}

static bool spec_apply_shared_bindings(
    spec_state *parent,
    spec_state *child,
    const char *bindings) {
    char *copy;
    char *cursor;

    if (bindings == NULL)
        return false;
    if (strcmp(bindings, "-") == 0)
        return true;

    copy = spec_strdup_text(bindings);
    if (copy == NULL)
        return false;

    cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
        char *next = strchr(cursor, ',');
        char *colon;
        size_t child_index;
        size_t parent_index;

        if (next != NULL)
            *next = '\0';
        colon = strchr(cursor, ':');
        if (colon == NULL) {
            free(copy);
            return false;
        }
        *colon = '\0';

        if (!spec_parse_slot_index(cursor, &child_index) ||
            !spec_parse_slot_index(colon + 1u, &parent_index) ||
            !spec_bind_shared_slot(
                parent, child, child_index, parent_index)) {
            free(copy);
            return false;
        }

        cursor = next == NULL ? NULL : next + 1u;
    }

    free(copy);
    return true;
}

static void spec_thread_main(void *argument) {
    spec_thread *thread = (spec_thread *)argument;

    if (thread == NULL ||
        thread->child == NULL ||
        thread->manifest_path == NULL)
        return;

    thread->rc = spec_run_manifest(
        thread->child, thread->manifest_path);
}

static void spec_command_thread(
    spec_state *state,
    unsigned line,
    const char *name_hex,
    const char *bindings,
    const char *manifest_path) {
    spec_thread *thread;

    if (state == NULL || name_hex == NULL ||
        bindings == NULL || manifest_path == NULL) {
        spec_note_failure(state, line, "invalid thread command");
        return;
    }
    if (spec_find_thread(state, name_hex) != NULL) {
        spec_note_failure(state, line, "duplicate thread name");
        return;
    }
    if (!spec_reserve_thread(state)) {
        spec_note_failure(state, line, "thread registry allocation failed");
        return;
    }

    thread = (spec_thread *)calloc(1u, sizeof(*thread));
    if (thread == NULL) {
        spec_note_failure(state, line, "thread allocation failed");
        return;
    }

    thread->name_hex = spec_strdup_text(name_hex);
    thread->manifest_path = spec_strdup_text(manifest_path);
    thread->child = (spec_state *)calloc(1u, sizeof(*thread->child));
    if (thread->name_hex == NULL ||
        thread->manifest_path == NULL ||
        thread->child == NULL) {
        free(thread->name_hex);
        free(thread->manifest_path);
        free(thread->child);
        free(thread);
        spec_note_failure(state, line, "thread allocation failed");
        return;
    }

    if (!spec_state_init(thread->child)) {
        free(thread->name_hex);
        free(thread->manifest_path);
        free(thread->child);
        free(thread);
        spec_note_failure(state, line, "thread state init failed");
        return;
    }

    if (!spec_apply_shared_bindings(
            state, thread->child, bindings)) {
        spec_state_destroy(thread->child);
        free(thread->child);
        free(thread->name_hex);
        free(thread->manifest_path);
        free(thread);
        spec_note_failure(state, line, "thread shared binding failed");
        return;
    }

    if (salts_thread_create(
            &thread->handle,
            spec_thread_main,
            thread) != 0) {
        spec_state_destroy(thread->child);
        free(thread->child);
        free(thread->name_hex);
        free(thread->manifest_path);
        free(thread);
        spec_note_failure(state, line, "thread creation failed");
        return;
    }

    state->threads[state->thread_count++] = thread;
    spec_note_pass(state);
}

static void spec_command_wait(
    spec_state *state,
    unsigned line,
    const char *name_hex) {
    spec_thread *thread;

    if (state == NULL || name_hex == NULL) {
        spec_note_failure(state, line, "invalid wait command");
        return;
    }

    thread = spec_find_thread(state, name_hex);
    if (thread == NULL) {
        spec_note_failure(state, line, "wait target unavailable");
        return;
    }
    if (thread->joined) {
        spec_note_failure(state, line, "thread already joined");
        return;
    }

    if (salts_thread_join(&thread->handle) != 0) {
        spec_note_failure(state, line, "thread join failed");
        return;
    }
    thread->joined = true;

    if (thread->child != NULL && !thread->aggregated) {
        state->passed += thread->child->passed;
        state->failed += thread->child->failed;
        state->unsupported += thread->child->unsupported;
        if (thread->rc != 0 && thread->child->failed == 0u)
            spec_note_failure(state, line, "thread manifest failed");
        thread->aggregated = true;

        spec_state_destroy(thread->child);
        free(thread->child);
        thread->child = NULL;
    }

    spec_note_pass(state);
}

static void spec_state_destroy(spec_state *state) {
    size_t index;

    if (state == NULL)
        return;

    index = state->thread_count;
    while (index != 0u) {
        spec_thread *thread;
        --index;
        thread = state->threads[index];
        if (thread == NULL)
            continue;

        if (!thread->joined && thread->handle != NULL) {
            (void)salts_thread_join(&thread->handle);
            thread->joined = true;
        }
        if (thread->child != NULL) {
            spec_state_destroy(thread->child);
            free(thread->child);
        }
        free(thread->name_hex);
        free(thread->manifest_path);
        free(thread);
    }
    free(state->threads);

    index = state->slot_count;
    while (index != 0u) {
        --index;
        if (state->slots[index] != NULL) {
            spec_slot_destroy(state->slots[index]);
            free(state->slots[index]);
        }
    }
    free(state->slots);

    index = state->retained_failure_count;
    while (index != 0u) {
        --index;
        if (state->retained_failures[index] != NULL) {
            spec_slot_destroy(state->retained_failures[index]);
            free(state->retained_failures[index]);
        }
    }
    free(state->retained_failures);

    if (state->spectest_ready) {
        turbowasm_instance_destroy(&state->spectest_instance);
        turbowasm_module_destroy(&state->spectest_module);
    }
    turbowasm_linker_destroy(&state->linker);
    memset(state, 0, sizeof(*state));
}

static int spec_hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

static uint8_t *spec_decode_hex(const char *text,
                                size_t *size_out) {
    size_t length;
    size_t index;
    uint8_t *bytes;

    if (text == NULL || size_out == NULL)
        return NULL;

    length = strlen(text);
    if ((length & 1u) != 0u)
        return NULL;

    bytes = (uint8_t *)malloc(length == 0u ? 1u : length / 2u);
    if (bytes == NULL)
        return NULL;

    for (index = 0u; index < length; index += 2u) {
        int hi = spec_hex_nibble(text[index]);
        int lo = spec_hex_nibble(text[index + 1u]);
        if (hi < 0 || lo < 0) {
            free(bytes);
            return NULL;
        }
        bytes[index / 2u] = (uint8_t)((hi << 4) | lo);
    }

    *size_out = length / 2u;
    return bytes;
}

static bool spec_parse_u64(const char *text, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out == NULL || *text == '\0')
        return false;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0')
        return false;

    *out = (uint64_t)value;
    return true;
}

static bool spec_v128_layout(
    const char *lane_type,
    size_t lane_type_size,
    turbowasm_v128_shape *out_shape,
    uint8_t *out_lane_bits,
    uint8_t *out_lane_count,
    bool *out_float_lane) {
    if (lane_type == NULL || out_shape == NULL ||
        out_lane_bits == NULL || out_lane_count == NULL ||
        out_float_lane == NULL)
        return false;

    *out_float_lane = false;
    if (lane_type_size == 2u &&
        memcmp(lane_type, "i8", 2u) == 0) {
        *out_shape = TURBOWASM_V128_I8X16;
        *out_lane_bits = 8u;
        *out_lane_count = 16u;
        return true;
    }
    if (lane_type_size == 3u &&
        memcmp(lane_type, "i16", 3u) == 0) {
        *out_shape = TURBOWASM_V128_I16X8;
        *out_lane_bits = 16u;
        *out_lane_count = 8u;
        return true;
    }
    if (lane_type_size == 3u &&
        memcmp(lane_type, "i32", 3u) == 0) {
        *out_shape = TURBOWASM_V128_I32X4;
        *out_lane_bits = 32u;
        *out_lane_count = 4u;
        return true;
    }
    if (lane_type_size == 3u &&
        memcmp(lane_type, "i64", 3u) == 0) {
        *out_shape = TURBOWASM_V128_I64X2;
        *out_lane_bits = 64u;
        *out_lane_count = 2u;
        return true;
    }
    if (lane_type_size == 3u &&
        memcmp(lane_type, "f32", 3u) == 0) {
        *out_shape = TURBOWASM_V128_F32X4;
        *out_lane_bits = 32u;
        *out_lane_count = 4u;
        *out_float_lane = true;
        return true;
    }
    if (lane_type_size == 3u &&
        memcmp(lane_type, "f64", 3u) == 0) {
        *out_shape = TURBOWASM_V128_F64X2;
        *out_lane_bits = 64u;
        *out_lane_count = 2u;
        *out_float_lane = true;
        return true;
    }
    return false;
}

static bool spec_parse_v128_token(
    const char *token,
    bool allow_patterns,
    turbowasm_value *out,
    spec_expected_value *expected_out) {
    const char *lane_type;
    const char *separator;
    char *copy = NULL;
    char *cursor;
    turbowasm_v128_shape shape;
    uint8_t lane_bits;
    uint8_t lane_count;
    bool float_lane;
    uint8_t bytes[16] = {0};
    uint8_t lane_index = 0u;
    size_t lane_size;

    if (token == NULL || out == NULL ||
        strncmp(token, "v128:", 5u) != 0)
        return false;

    lane_type = token + 5u;
    separator = strchr(lane_type, ':');
    if (separator == NULL ||
        !spec_v128_layout(
            lane_type, (size_t)(separator - lane_type),
            &shape, &lane_bits, &lane_count, &float_lane))
        return false;

    copy = (char *)malloc(strlen(separator + 1u) + 1u);
    if (copy == NULL)
        return false;
    strcpy(copy, separator + 1u);
    cursor = copy;
    lane_size = (size_t)lane_bits / 8u;

    if (expected_out != NULL) {
        expected_out->v128_lane_bits = lane_bits;
        expected_out->v128_lane_count = lane_count;
    }

    while (cursor != NULL && *cursor != '\0') {
        char *next = strchr(cursor, ';');
        uint64_t bits = 0u;
        spec_expected_pattern pattern = SPEC_EXPECT_EXACT;
        size_t byte_index;

        if (next != NULL)
            *next = '\0';
        if (lane_index >= lane_count) {
            free(copy);
            return false;
        }

        if (allow_patterns && float_lane &&
            strcmp(cursor, "nan:canonical") == 0) {
            pattern = SPEC_EXPECT_NAN_CANONICAL;
        } else if (allow_patterns && float_lane &&
                   strcmp(cursor, "nan:arithmetic") == 0) {
            pattern = SPEC_EXPECT_NAN_ARITHMETIC;
        } else {
            if (!spec_parse_u64(cursor, &bits) ||
                (lane_bits < 64u &&
                 bits >= (UINT64_C(1) << lane_bits))) {
                free(copy);
                return false;
            }
        }

        for (byte_index = 0u;
             byte_index < lane_size;
             ++byte_index) {
            bytes[(size_t)lane_index * lane_size + byte_index] =
                (uint8_t)(bits >> (8u * byte_index));
        }
        if (expected_out != NULL)
            expected_out->v128_lane_patterns[lane_index] = pattern;

        ++lane_index;
        cursor = next == NULL ? NULL : next + 1u;
    }

    free(copy);
    if (lane_index != lane_count)
        return false;

    memset(out, 0, sizeof(*out));
    out->kind = TURBOWASM_VALUE_V128;
    return turbowasm_v128_load(
               &out->as.v128, shape, bytes) == TURBOWASM_OK;
}

static uint64_t spec_read_lane_le(
    const uint8_t *bytes,
    size_t lane_size) {
    uint64_t result = 0u;
    size_t index;

    for (index = 0u; index < lane_size; ++index)
        result |= (uint64_t)bytes[index] << (8u * index);
    return result;
}

static bool spec_parse_value(const char *token,
                             turbowasm_value *out) {
    const char *colon;
    size_t type_size;
    uint64_t bits;

    if (token == NULL || out == NULL)
        return false;

    if (strncmp(token, "v128:", 5u) == 0)
        return spec_parse_v128_token(
            token, false, out, NULL);

    memset(out, 0, sizeof(*out));
    colon = strchr(token, ':');
    if (colon == NULL)
        return false;
    type_size = (size_t)(colon - token);

    if (type_size == 3u &&
        memcmp(token, "i32", 3u) == 0) {
        if (!spec_parse_u64(colon + 1u, &bits))
            return false;
        out->kind = TURBOWASM_VALUE_I32;
        out->as.i32 = (int32_t)(uint32_t)bits;
        return true;
    }
    if (type_size == 3u &&
        memcmp(token, "i64", 3u) == 0) {
        if (!spec_parse_u64(colon + 1u, &bits))
            return false;
        out->kind = TURBOWASM_VALUE_I64;
        out->as.i64 = (int64_t)bits;
        return true;
    }
    if (type_size == 3u &&
        memcmp(token, "f32", 3u) == 0) {
        uint32_t raw;
        if (!spec_parse_u64(colon + 1u, &bits) ||
            bits > UINT32_MAX)
            return false;
        raw = (uint32_t)bits;
        out->kind = TURBOWASM_VALUE_F32;
        memcpy(&out->as.f32, &raw, sizeof(raw));
        return true;
    }
    if (type_size == 3u &&
        memcmp(token, "f64", 3u) == 0) {
        if (!spec_parse_u64(colon + 1u, &bits))
            return false;
        out->kind = TURBOWASM_VALUE_F64;
        memcpy(&out->as.f64, &bits, sizeof(bits));
        return true;
    }
    if (type_size == 7u &&
        memcmp(token, "funcref", 7u) == 0 &&
        strcmp(colon + 1u, "null") == 0) {
        out->kind = TURBOWASM_VALUE_FUNCREF;
        out->as.funcref.is_null = true;
        out->as.funcref.function_index = UINT32_MAX;
        out->as.funcref.owner = NULL;
        return true;
    }
    if (type_size == 9u &&
        memcmp(token, "externref", 9u) == 0) {
        out->kind = TURBOWASM_VALUE_EXTERNREF;
        if (strcmp(colon + 1u, "null") == 0) {
            out->as.externref.is_null = true;
            out->as.externref.token = 0u;
            return true;
        }
        if (!spec_parse_u64(colon + 1u, &bits) ||
            bits > (uint64_t)UINTPTR_MAX)
            return false;
        out->as.externref.is_null = false;
        out->as.externref.token = (uintptr_t)bits;
        return true;
    }

    return false;
}

static bool spec_values_equal(const turbowasm_value *actual,
                              const turbowasm_value *expected) {
    uint32_t f32_actual;
    uint32_t f32_expected;
    uint64_t f64_actual;
    uint64_t f64_expected;

    if (actual == NULL || expected == NULL ||
        actual->kind != expected->kind)
        return false;

    switch (actual->kind) {
        case TURBOWASM_VALUE_I32:
            return actual->as.i32 == expected->as.i32;
        case TURBOWASM_VALUE_I64:
            return actual->as.i64 == expected->as.i64;
        case TURBOWASM_VALUE_F32:
            memcpy(&f32_actual, &actual->as.f32, sizeof(f32_actual));
            memcpy(&f32_expected, &expected->as.f32,
                   sizeof(f32_expected));
            return f32_actual == f32_expected;
        case TURBOWASM_VALUE_F64:
            memcpy(&f64_actual, &actual->as.f64, sizeof(f64_actual));
            memcpy(&f64_expected, &expected->as.f64,
                   sizeof(f64_expected));
            return f64_actual == f64_expected;
        case TURBOWASM_VALUE_FUNCREF:
            return actual->as.funcref.is_null &&
                   expected->as.funcref.is_null;
        case TURBOWASM_VALUE_EXTERNREF:
            if (actual->as.externref.is_null ||
                expected->as.externref.is_null)
                return actual->as.externref.is_null &&
                       expected->as.externref.is_null;
            return actual->as.externref.token ==
                   expected->as.externref.token;
        case TURBOWASM_VALUE_V128: {
            uint8_t actual_bytes[16];
            uint8_t expected_bytes[16];
            if (turbowasm_v128_store(
                    actual_bytes, &actual->as.v128) != TURBOWASM_OK ||
                turbowasm_v128_store(
                    expected_bytes, &expected->as.v128) != TURBOWASM_OK)
                return false;
            return memcmp(
                       actual_bytes, expected_bytes,
                       sizeof(actual_bytes)) == 0;
        }
        default:
            return false;
    }
}

static size_t spec_count_tokens(const char *text) {
    size_t count = 1u;
    const char *cursor;

    if (text == NULL || strcmp(text, "-") == 0 ||
        *text == '\0')
        return 0u;

    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == ',')
            ++count;
    }
    return count;
}

static bool spec_parse_values(const char *text,
                              turbowasm_value **out_values,
                              size_t *out_count) {
    size_t count;
    size_t index = 0u;
    char *copy = NULL;
    char *cursor;
    turbowasm_value *values = NULL;

    if (out_values == NULL || out_count == NULL)
        return false;
    *out_values = NULL;
    *out_count = 0u;

    count = spec_count_tokens(text);
    if (count == 0u)
        return true;

    values = (turbowasm_value *)calloc(count, sizeof(*values));
    if (values == NULL)
        return false;

    copy = (char *)malloc(strlen(text) + 1u);
    if (copy == NULL) {
        free(values);
        return false;
    }
    strcpy(copy, text);

    cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
        char *comma = strchr(cursor, ',');
        if (comma != NULL)
            *comma = '\0';

        if (index >= count ||
            !spec_parse_value(cursor, &values[index])) {
            free(copy);
            free(values);
            return false;
        }
        ++index;
        cursor = comma == NULL ? NULL : comma + 1u;
    }

    free(copy);
    if (index != count) {
        free(values);
        return false;
    }

    *out_values = values;
    *out_count = count;
    return true;
}

static bool spec_parse_expected_value(
    const char *token,
    spec_expected_value *out) {
    if (token == NULL || out == NULL)
        return false;

    memset(out, 0, sizeof(*out));

    if (strncmp(token, "v128:", 5u) == 0) {
        out->pattern = SPEC_EXPECT_EXACT;
        return spec_parse_v128_token(
            token, true, &out->value, out);
    }

    if (strcmp(token, "f32:nan:canonical") == 0) {
        out->value.kind = TURBOWASM_VALUE_F32;
        out->pattern = SPEC_EXPECT_NAN_CANONICAL;
        return true;
    }
    if (strcmp(token, "f32:nan:arithmetic") == 0) {
        out->value.kind = TURBOWASM_VALUE_F32;
        out->pattern = SPEC_EXPECT_NAN_ARITHMETIC;
        return true;
    }
    if (strcmp(token, "f64:nan:canonical") == 0) {
        out->value.kind = TURBOWASM_VALUE_F64;
        out->pattern = SPEC_EXPECT_NAN_CANONICAL;
        return true;
    }
    if (strcmp(token, "f64:nan:arithmetic") == 0) {
        out->value.kind = TURBOWASM_VALUE_F64;
        out->pattern = SPEC_EXPECT_NAN_ARITHMETIC;
        return true;
    }
    if (strcmp(token, "funcref:nonnull") == 0) {
        out->value.kind = TURBOWASM_VALUE_FUNCREF;
        out->pattern = SPEC_EXPECT_FUNCREF_NONNULL;
        return true;
    }

    out->pattern = SPEC_EXPECT_EXACT;
    return spec_parse_value(token, &out->value);
}

static bool spec_parse_expected_values(
    const char *text,
    spec_expected_value **out_values,
    size_t *out_count) {
    size_t count;
    size_t index = 0u;
    char *copy = NULL;
    char *cursor;
    spec_expected_value *values = NULL;

    if (out_values == NULL || out_count == NULL)
        return false;
    *out_values = NULL;
    *out_count = 0u;

    count = spec_count_tokens(text);
    if (count == 0u)
        return true;

    values = (spec_expected_value *)calloc(count, sizeof(*values));
    if (values == NULL)
        return false;

    copy = (char *)malloc(strlen(text) + 1u);
    if (copy == NULL) {
        free(values);
        return false;
    }
    strcpy(copy, text);

    cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
        char *comma = strchr(cursor, ',');
        if (comma != NULL)
            *comma = '\0';

        if (index >= count ||
            !spec_parse_expected_value(cursor, &values[index])) {
            free(copy);
            free(values);
            return false;
        }
        ++index;
        cursor = comma == NULL ? NULL : comma + 1u;
    }

    free(copy);
    if (index != count) {
        free(values);
        return false;
    }

    *out_values = values;
    *out_count = count;
    return true;
}

static bool spec_expected_matches(
    const turbowasm_value *actual,
    const spec_expected_value *expected) {
    uint32_t f32_bits;
    uint64_t f64_bits;

    if (actual == NULL || expected == NULL)
        return false;

    if (expected->value.kind == TURBOWASM_VALUE_V128 &&
        expected->v128_lane_count != 0u) {
        uint8_t actual_bytes[16];
        uint8_t expected_bytes[16];
        size_t lane_size =
            (size_t)expected->v128_lane_bits / 8u;
        uint8_t lane;

        if (actual->kind != TURBOWASM_VALUE_V128 ||
            lane_size == 0u ||
            (size_t)expected->v128_lane_count * lane_size != 16u ||
            turbowasm_v128_store(
                actual_bytes, &actual->as.v128) != TURBOWASM_OK ||
            turbowasm_v128_store(
                expected_bytes,
                &expected->value.as.v128) != TURBOWASM_OK)
            return false;

        for (lane = 0u;
             lane < expected->v128_lane_count;
             ++lane) {
            const uint8_t *a =
                actual_bytes + (size_t)lane * lane_size;
            const uint8_t *e =
                expected_bytes + (size_t)lane * lane_size;
            spec_expected_pattern lane_pattern =
                expected->v128_lane_patterns[lane];

            if (lane_pattern == SPEC_EXPECT_EXACT) {
                if (memcmp(a, e, lane_size) != 0)
                    return false;
                continue;
            }

            if (expected->v128_lane_bits == 32u) {
                uint32_t bits = (uint32_t)spec_read_lane_le(
                    a, lane_size);
                if (lane_pattern == SPEC_EXPECT_NAN_CANONICAL) {
                    if ((bits & UINT32_C(0x7fffffff)) !=
                        UINT32_C(0x7fc00000))
                        return false;
                } else if ((bits & UINT32_C(0x7fc00000)) !=
                           UINT32_C(0x7fc00000)) {
                    return false;
                }
                continue;
            }

            if (expected->v128_lane_bits == 64u) {
                uint64_t bits = spec_read_lane_le(a, lane_size);
                if (lane_pattern == SPEC_EXPECT_NAN_CANONICAL) {
                    if ((bits & UINT64_C(0x7fffffffffffffff)) !=
                        UINT64_C(0x7ff8000000000000))
                        return false;
                } else if ((bits & UINT64_C(0x7ff8000000000000)) !=
                           UINT64_C(0x7ff8000000000000)) {
                    return false;
                }
                continue;
            }

            return false;
        }
        return true;
    }

    if (expected->pattern == SPEC_EXPECT_EXACT)
        return spec_values_equal(actual, &expected->value);

    if (actual->kind != expected->value.kind)
        return false;

    if (expected->pattern == SPEC_EXPECT_FUNCREF_NONNULL)
        return actual->kind == TURBOWASM_VALUE_FUNCREF &&
               !actual->as.funcref.is_null;

    if (actual->kind == TURBOWASM_VALUE_F32) {
        memcpy(&f32_bits, &actual->as.f32, sizeof(f32_bits));
        if (expected->pattern == SPEC_EXPECT_NAN_CANONICAL)
            return (f32_bits & UINT32_C(0x7fffffff)) ==
                   UINT32_C(0x7fc00000);
        return (f32_bits & UINT32_C(0x7fc00000)) ==
               UINT32_C(0x7fc00000);
    }

    if (actual->kind == TURBOWASM_VALUE_F64) {
        memcpy(&f64_bits, &actual->as.f64, sizeof(f64_bits));
        if (expected->pattern == SPEC_EXPECT_NAN_CANONICAL)
            return (f64_bits & UINT64_C(0x7fffffffffffffff)) ==
                   UINT64_C(0x7ff8000000000000);
        return (f64_bits & UINT64_C(0x7ff8000000000000)) ==
               UINT64_C(0x7ff8000000000000);
    }

    return false;
}

static int64_t spec_resolve_slot(const spec_state *state,
                                 const char *text) {
    char *end = NULL;
    long long parsed;

    if (strcmp(text, "-") == 0)
        return state->current_slot;

    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || parsed < 0)
        return -1;
    return (int64_t)parsed;
}

static bool spec_export_function(const spec_slot *slot,
                                 const uint8_t *name,
                                 size_t name_size,
                                 uint32_t *out_index) {
    size_t index;
    size_t count;

    if (slot == NULL || !slot->loaded || out_index == NULL)
        return false;

    count = turbowasm_module_export_count(&slot->module);
    for (index = 0u; index < count; ++index) {
        const turbowasm_export_desc *desc =
            turbowasm_module_export_at(&slot->module, index);
        if (desc == NULL ||
            desc->kind != TURBOWASM_EXTERN_FUNCTION ||
            desc->name.size != name_size)
            continue;
        if (name_size == 0u ||
            memcmp(desc->name.bytes, name, name_size) == 0) {
            *out_index = desc->item_index;
            return true;
        }
    }
    return false;
}

static turbowasm_status spec_invoke(spec_state *state,
                                    int64_t slot_index,
                                    const char *field_hex,
                                    const char *args_text,
                                    turbowasm_value **out_results,
                                    size_t *out_result_count,
                                    turbowasm_trap *out_trap,
                                    bool *out_unsupported) {
    spec_slot *slot;
    uint8_t *field = NULL;
    size_t field_size = 0u;
    uint32_t function_index;
    turbowasm_function_signature signature;
    turbowasm_value *arguments = NULL;
    size_t argument_count = 0u;
    turbowasm_value *results = NULL;
    size_t result_count = 0u;
    turbowasm_status status;

    *out_results = NULL;
    *out_result_count = 0u;
    *out_trap = TURBOWASM_TRAP_NONE;
    *out_unsupported = false;

    if (slot_index < 0 ||
        (size_t)slot_index >= state->slot_count)
        return TURBOWASM_INVALID_ARGUMENT;

    slot = state->slots[(size_t)slot_index];
    if (slot == NULL || slot->unsupported || !slot->ready) {
        *out_unsupported = true;
        return TURBOWASM_UNSUPPORTED;
    }

    field = spec_decode_hex(field_hex, &field_size);
    if (field == NULL)
        return TURBOWASM_INVALID_ARGUMENT;

    if (!spec_export_function(
            slot, field, field_size, &function_index)) {
        free(field);
        return TURBOWASM_INVALID_ARGUMENT;
    }
    free(field);

    if (!turbowasm_module_function_signature_get(
            &slot->module, function_index, &signature))
        return TURBOWASM_INVALID_ARGUMENT;

    if (!spec_parse_values(
            args_text, &arguments, &argument_count))
        return TURBOWASM_INVALID_ARGUMENT;
    if (argument_count != signature.param_count) {
        free(arguments);
        return TURBOWASM_INVALID_ARGUMENT;
    }

    if (signature.result_count != 0u) {
        results = (turbowasm_value *)calloc(
            signature.result_count, sizeof(*results));
        if (results == NULL) {
            free(arguments);
            return TURBOWASM_OUT_OF_MEMORY;
        }
    }

    status = turbowasm_instance_invoke(
        &slot->instance,
        function_index,
        arguments,
        argument_count,
        results,
        signature.result_count,
        &result_count,
        out_trap);

    free(arguments);

    if (status == TURBOWASM_UNSUPPORTED) {
        *out_unsupported = true;
        free(results);
        return status;
    }
    if (status != TURBOWASM_OK && status != TURBOWASM_TRAPPED) {
        free(results);
        return status;
    }

    *out_results = results;
    *out_result_count = result_count;
    return status;
}

static void spec_command_module_definition(spec_state *state,
                                           unsigned line,
                                           const char *path) {
    size_t size = 0u;
    uint8_t *bytes = spec_read_file(path, &size);
    turbowasm_module module = {0};
    turbowasm_status status;

    if (bytes == NULL) {
        spec_note_failure(state, line, "cannot read module definition");
        return;
    }

    status = turbowasm_module_load_borrowed(&module, bytes, size);
    if (status == TURBOWASM_UNSUPPORTED) {
        spec_note_runtime_unsupported(state, line,
                                      "module definition validation unsupported");
    } else if (status != TURBOWASM_OK) {
        spec_note_failure(state, line, "valid module definition rejected");
    } else {
        spec_note_pass(state);
    }

    turbowasm_module_destroy(&module);
    free(bytes);
}

static void spec_command_module(spec_state *state,
                                unsigned line,
                                size_t slot_index,
                                const char *path) {
    spec_slot *slot;
    turbowasm_status status;

    if (!spec_ensure_slot(state, slot_index)) {
        spec_note_failure(state, line, "slot allocation failed");
        return;
    }

    slot = state->slots[slot_index];
    spec_slot_destroy(slot);
    slot->bytes = spec_read_file(path, &slot->size);
    if (slot->bytes == NULL) {
        spec_note_failure(state, line, "cannot read module file");
        return;
    }

    status = turbowasm_module_load_borrowed(
        &slot->module, slot->bytes, slot->size);
    if (status == TURBOWASM_UNSUPPORTED) {
        slot->unsupported = true;
        state->current_slot = (int64_t)slot_index;
        spec_note_runtime_unsupported(state, line, "module validation unsupported");
        return;
    }
    if (status != TURBOWASM_OK) {
        spec_note_failure(state, line, "valid module rejected");
        return;
    }

    slot->loaded = true;
    status = turbowasm_instance_create_linked(
        &slot->instance, &slot->module, &state->linker);
    if (status == TURBOWASM_UNSUPPORTED ||
        status == TURBOWASM_LINK_ERROR) {
        slot->unsupported = true;
        state->current_slot = (int64_t)slot_index;
        spec_note_runtime_unsupported(state, line, "module host/link feature unsupported");
        return;
    }
    if (status != TURBOWASM_OK) {
        spec_note_failure(state, line, "valid module failed instantiation");
        return;
    }

#ifdef TURBOWASM_SPEC_ENABLE_MIR
    status = spec_attach_mir(&slot->instance);
    if (status != TURBOWASM_OK) {
        spec_note_failure(state, line, "failed to attach MIR replay backend");
        return;
    }
#endif

    slot->ready = true;
    state->current_slot = (int64_t)slot_index;
    spec_note_pass(state);
}

static void spec_command_register(spec_state *state,
                                  unsigned line,
                                  int64_t slot_index,
                                  const char *name_hex) {
    size_t name_size = 0u;
    uint8_t *name_bytes;
    turbowasm_name name;
    turbowasm_status status;

    if (slot_index < 0 ||
        (size_t)slot_index >= state->slot_count ||
        state->slots[(size_t)slot_index] == NULL ||
        state->slots[(size_t)slot_index]->unsupported ||
        !state->slots[(size_t)slot_index]->ready) {
        spec_note_runtime_unsupported(state, line, "register target unavailable");
        return;
    }

    name_bytes = spec_decode_hex(name_hex, &name_size);
    if (name_bytes == NULL) {
        spec_note_failure(state, line, "invalid register name");
        return;
    }
    name.bytes = name_bytes;
    name.size = (uint32_t)name_size;

    status = turbowasm_linker_define_instance(
        &state->linker, name,
        &state->slots[(size_t)slot_index]->instance);
    free(name_bytes);

    if (status != TURBOWASM_OK) {
        spec_note_failure(state, line, "register failed");
        return;
    }
    spec_note_pass(state);
}

static void spec_command_negative_module(spec_state *state,
                                         unsigned line,
                                         const char *kind,
                                         const char *path) {
    size_t size = 0u;
    uint8_t *bytes = spec_read_file(path, &size);
    turbowasm_module module = {0};
    turbowasm_instance instance = {0};
    turbowasm_status status;

    if (bytes == NULL) {
        spec_note_failure(state, line, "cannot read assertion module");
        return;
    }

    status = turbowasm_module_load_borrowed(&module, bytes, size);

    if (strcmp(kind, "assert_invalid") == 0 ||
        strcmp(kind, "assert_malformed") == 0) {
        if (status == TURBOWASM_MALFORMED_MODULE) {
            spec_note_pass(state);
        } else if (status == TURBOWASM_UNSUPPORTED) {
            spec_note_runtime_unsupported(state, line,
                                          "negative module feature unsupported");
        } else {
            spec_note_failure(state, line,
                              "negative module unexpectedly admitted");
        }
        turbowasm_module_destroy(&module);
        free(bytes);
        return;
    }

    if (status == TURBOWASM_UNSUPPORTED) {
        spec_note_runtime_unsupported(state, line,
                                      "assertion module validation unsupported");
        free(bytes);
        return;
    }
    if (status != TURBOWASM_OK) {
        spec_note_failure(state, line,
                          "assertion module failed before expected phase");
        free(bytes);
        return;
    }

    if (strcmp(kind, "assert_uninstantiable") == 0) {
        status = turbowasm_instance_create_linked_preserve_failure(
            &instance, &module, &state->linker);
    } else {
        status = turbowasm_instance_create_linked(
            &instance, &module, &state->linker);
    }

    if (strcmp(kind, "assert_unlinkable") == 0) {
        if (status == TURBOWASM_LINK_ERROR ||
            status == TURBOWASM_TYPE_MISMATCH) {
            spec_note_pass(state);
        } else if (status == TURBOWASM_UNSUPPORTED) {
            spec_note_runtime_unsupported(state, line,
                                          "unlinkable feature unsupported");
        } else {
            spec_note_failure(state, line,
                              "module did not fail linking");
        }
    } else if (strcmp(kind, "assert_uninstantiable") == 0) {
        if (status == TURBOWASM_TRAPPED) {
            if (instance.impl != NULL &&
                !spec_retain_failed_instantiation(
                    state, &bytes, size,
                    &module, &instance)) {
                spec_note_failure(
                    state, line,
                    "failed to retain trapped store allocation");
            } else {
                spec_note_pass(state);
            }
        } else if (status == TURBOWASM_UNSUPPORTED ||
                   status == TURBOWASM_LINK_ERROR) {
            spec_note_runtime_unsupported(state, line,
                                          "uninstantiable precondition unsupported");
        } else {
            spec_note_failure(state, line,
                              "module did not trap during instantiation");
        }
    } else {
        spec_note_failure(state, line, "unknown negative module command");
    }

    turbowasm_instance_destroy(&instance);
    turbowasm_module_destroy(&module);
    free(bytes);
}

static void spec_command_action(spec_state *state,
                                unsigned line,
                                int64_t slot_index,
                                const char *field_hex,
                                const char *args_text) {
    turbowasm_value *results = NULL;
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    bool unsupported = false;
    turbowasm_status status = spec_invoke(
        state, slot_index, field_hex, args_text,
        &results, &result_count, &trap, &unsupported);

    free(results);
    if (unsupported) {
        spec_note_runtime_unsupported(state, line, "invoke unsupported");
    } else if (status != TURBOWASM_OK) {
        spec_note_failure(state, line, "action did not complete");
    } else {
        spec_note_pass(state);
    }
}

static void spec_command_assert_return(spec_state *state,
                                       unsigned line,
                                       int64_t slot_index,
                                       const char *field_hex,
                                       const char *args_text,
                                       const char *expected_text) {
    turbowasm_value *results = NULL;
    spec_expected_value *expected = NULL;
    size_t result_count = 0u;
    size_t expected_count = 0u;
    size_t index;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    bool unsupported = false;
    turbowasm_status status;

    if (!spec_parse_expected_values(
            expected_text, &expected, &expected_count)) {
        spec_note_failure(state, line, "invalid expected value encoding");
        return;
    }

    status = spec_invoke(
        state, slot_index, field_hex, args_text,
        &results, &result_count, &trap, &unsupported);

    if (unsupported) {
        spec_note_runtime_unsupported(state, line, "assert_return invoke unsupported");
        goto done;
    }
    if (status != TURBOWASM_OK) {
        spec_note_failure(state, line, "assert_return did not complete");
        goto done;
    }
    if (result_count != expected_count) {
        spec_note_failure(state, line, "assert_return result count mismatch");
        goto done;
    }

    for (index = 0u; index < result_count; ++index) {
        if (!spec_expected_matches(&results[index], &expected[index])) {
            spec_note_failure(state, line, "assert_return value mismatch");
            goto done;
        }
    }

    spec_note_pass(state);

done:
    free(results);
    free(expected);
}

static void spec_command_assert_return_either(
    spec_state *state,
    unsigned line,
    int64_t slot_index,
    const char *field_hex,
    const char *args_text,
    const char *alternatives_text) {
    turbowasm_value *results = NULL;
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    bool unsupported = false;
    turbowasm_status status;
    char *copy = NULL;
    char *cursor;
    bool matched = false;
    bool parsed_any = false;

    if (alternatives_text == NULL || *alternatives_text == '\0') {
        spec_note_failure(state, line, "empty permitted result set");
        return;
    }

    status = spec_invoke(
        state, slot_index, field_hex, args_text,
        &results, &result_count, &trap, &unsupported);

    if (unsupported) {
        spec_note_runtime_unsupported(
            state, line, "assert_return_either invoke unsupported");
        goto done;
    }
    if (status != TURBOWASM_OK) {
        spec_note_failure(
            state, line, "assert_return_either did not complete");
        goto done;
    }
    if (result_count != 1u) {
        spec_note_failure(
            state, line, "assert_return_either requires one result");
        goto done;
    }

    copy = (char *)malloc(strlen(alternatives_text) + 1u);
    if (copy == NULL) {
        spec_note_failure(
            state, line, "permitted result allocation failed");
        goto done;
    }
    strcpy(copy, alternatives_text);

    cursor = copy;
    while (cursor != NULL && *cursor != '\0') {
        char *next = strchr(cursor, '|');
        spec_expected_value expected;

        if (next != NULL)
            *next = '\0';
        if (!spec_parse_expected_value(cursor, &expected)) {
            spec_note_failure(
                state, line, "invalid permitted result encoding");
            goto done;
        }

        parsed_any = true;
        if (spec_expected_matches(&results[0], &expected)) {
            matched = true;
            break;
        }
        cursor = next == NULL ? NULL : next + 1u;
    }

    if (!parsed_any) {
        spec_note_failure(state, line, "empty permitted result set");
    } else if (!matched) {
        spec_note_failure(
            state, line, "result outside permitted result set");
    } else {
        spec_note_pass(state);
    }

done:
    free(copy);
    free(results);
}

static void spec_command_assert_trap(spec_state *state,
                                     unsigned line,
                                     int64_t slot_index,
                                     const char *field_hex,
                                     const char *args_text,
                                     int expected_trap) {
    turbowasm_value *results = NULL;
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    bool unsupported = false;
    turbowasm_status status = spec_invoke(
        state, slot_index, field_hex, args_text,
        &results, &result_count, &trap, &unsupported);

    free(results);

    if (unsupported) {
        spec_note_runtime_unsupported(state, line, "assert_trap invoke unsupported");
        return;
    }
    if (status != TURBOWASM_TRAPPED) {
        spec_note_failure(state, line, "expected Wasm trap");
        return;
    }
    if ((int)trap != expected_trap) {
        spec_note_failure(state, line, "trap kind mismatch");
        return;
    }
    spec_note_pass(state);
}

static size_t spec_split_tabs(char *line,
                              char **fields,
                              size_t capacity) {
    size_t count = 0u;
    char *cursor = line;

    while (cursor != NULL && count < capacity) {
        char *tab = strchr(cursor, '\t');
        fields[count++] = cursor;
        if (tab == NULL)
            break;
        *tab = '\0';
        cursor = tab + 1u;
    }
    return count;
}

static bool spec_parse_unsigned(const char *text, unsigned *out) {
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' ||
        value > UINT32_MAX)
        return false;
    *out = (unsigned)value;
    return true;
}

static void spec_trace_command_result(
    const spec_state *state,
    unsigned line,
    const char *command,
    size_t passed_before,
    size_t failed_before,
    size_t unsupported_before) {
    const char *outcome = "mixed";

    if (state == NULL || command == NULL ||
        getenv("TURBOWASM_SPEC_RESULT_TRACE") == NULL)
        return;

    if (state->passed == passed_before + 1u &&
        state->failed == failed_before &&
        state->unsupported == unsupported_before) {
        outcome = "pass";
    } else if (state->passed == passed_before &&
               state->failed == failed_before + 1u &&
               state->unsupported == unsupported_before) {
        outcome = "fail";
    } else if (state->passed == passed_before &&
               state->failed == failed_before &&
               state->unsupported == unsupported_before + 1u) {
        outcome = "unsupported";
    } else if (state->passed == passed_before &&
               state->failed == failed_before &&
               state->unsupported == unsupported_before) {
        outcome = "none";
    }

    printf("RESULT line=%u command=%s outcome=%s\n",
           line, command, outcome);
    fflush(stdout);
}

static int spec_run_manifest(spec_state *state, const char *path) {
    FILE *file = fopen(path, "rb");
    char buffer[65536];
    bool first = true;

    if (file == NULL) {
        fprintf(stderr, "cannot open manifest: %s\n", path);
        return 2;
    }

    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        char *fields[8] = {0};
        size_t field_count;
        size_t length = strlen(buffer);
        unsigned line = 0u;

        while (length != 0u &&
               (buffer[length - 1u] == '\n' ||
                buffer[length - 1u] == '\r')) {
            buffer[--length] = '\0';
        }
        if (length == 0u)
            continue;

        if (first) {
            first = false;
            if (strcmp(buffer, "TWCF1") != 0) {
                fprintf(stderr, "unsupported manifest version\n");
                fclose(file);
                return 2;
            }
            continue;
        }

        field_count = spec_split_tabs(
            buffer, fields, sizeof(fields) / sizeof(fields[0]));
        if (field_count < 3u ||
            !spec_parse_unsigned(fields[1], &line)) {
            spec_note_failure(state, 0u, "malformed manifest line");
            continue;
        }

        {
            const size_t passed_before = state->passed;
            const size_t failed_before = state->failed;
            const size_t unsupported_before = state->unsupported;

        if (getenv("TURBOWASM_SPEC_TRACE") != NULL) {
            fprintf(stderr, "TRACE line=%u command=%s\n",
                    line, fields[0]);
            fflush(stderr);
        }

        if (strcmp(fields[0], "unsupported") == 0) {
            spec_note_unsupported(state, line, fields[2]);
            goto command_done;
        }

        if (strcmp(fields[0], "module_definition") == 0 &&
            field_count == 3u) {
            spec_command_module_definition(state, line, fields[2]);
            goto command_done;
        }

        if (strcmp(fields[0], "module") == 0 && field_count == 4u) {
            unsigned slot = 0u;
            if (!spec_parse_unsigned(fields[2], &slot)) {
                spec_note_failure(state, line, "bad module slot");
                goto command_done;
            }
            spec_command_module(state, line, (size_t)slot, fields[3]);
            goto command_done;
        }

        if (strcmp(fields[0], "register") == 0 && field_count == 4u) {
            int64_t slot = spec_resolve_slot(state, fields[2]);
            spec_command_register(state, line, slot, fields[3]);
            goto command_done;
        }

        if ((strcmp(fields[0], "assert_invalid") == 0 ||
             strcmp(fields[0], "assert_malformed") == 0 ||
             strcmp(fields[0], "assert_unlinkable") == 0 ||
             strcmp(fields[0], "assert_uninstantiable") == 0) &&
            field_count == 3u) {
            spec_command_negative_module(
                state, line, fields[0], fields[2]);
            goto command_done;
        }

        if (strcmp(fields[0], "action") == 0 && field_count == 5u) {
            int64_t slot = spec_resolve_slot(state, fields[2]);
            spec_command_action(
                state, line, slot, fields[3], fields[4]);
            goto command_done;
        }

        if (strcmp(fields[0], "assert_return") == 0 &&
            field_count == 6u) {
            int64_t slot = spec_resolve_slot(state, fields[2]);
            spec_command_assert_return(
                state, line, slot, fields[3],
                fields[4], fields[5]);
            goto command_done;
        }

        if (strcmp(fields[0], "assert_return_either") == 0 &&
            field_count == 6u) {
            int64_t slot = spec_resolve_slot(state, fields[2]);
            spec_command_assert_return_either(
                state, line, slot, fields[3],
                fields[4], fields[5]);
            goto command_done;
        }

        if (strcmp(fields[0], "assert_trap") == 0 &&
            field_count == 6u) {
            char *end = NULL;
            long trap_value;
            int64_t slot = spec_resolve_slot(state, fields[2]);
            errno = 0;
            trap_value = strtol(fields[5], &end, 10);
            if (errno != 0 || end == NULL || *end != '\0') {
                spec_note_failure(state, line, "bad trap encoding");
                goto command_done;
            }
            spec_command_assert_trap(
                state, line, slot, fields[3],
                fields[4], (int)trap_value);
            goto command_done;
        }

        spec_note_failure(state, line, "unknown manifest command");

command_done:
        spec_trace_command_result(
            state, line, fields[0],
            passed_before, failed_before, unsupported_before);
        }
    }

    fclose(file);
    return 0;
}

int main(int argc, char **argv) {
    spec_state state;
    size_t index;
    int rc;
#ifdef TURBOWASM_SPEC_ENABLE_MIR
    size_t mir_compiled = 0u;
    size_t mir_interpret_only = 0u;
    size_t mir_cold = 0u;
    uint64_t mir_calls = 0u;
#endif

    if (argc != 2) {
        fprintf(stderr, "usage: turbowasm_spec_runner <manifest>\n");
        return 2;
    }

    memset(&state, 0, sizeof(state));
    state.current_slot = -1;

    if (turbowasm_linker_init(&state.linker) != TURBOWASM_OK) {
        fprintf(stderr, "failed to initialize linker\n");
        return 2;
    }
    if (!spec_init_spectest(&state)) {
        fprintf(stderr, "failed to initialize spectest provider\n");
        turbowasm_linker_destroy(&state.linker);
        return 2;
    }

    rc = spec_run_manifest(&state, argv[1]);

    printf("CONFORMANCE pass=%zu fail=%zu unsupported=%zu total=%zu\n",
           state.passed,
           state.failed,
           state.unsupported,
           state.passed + state.failed + state.unsupported);

#ifdef TURBOWASM_SPEC_ENABLE_MIR
    spec_collect_mir_stats(
        &state,
        &mir_compiled,
        &mir_interpret_only,
        &mir_cold,
        &mir_calls);
    printf("MIR_REPLAY compiled=%zu interpret_only=%zu cold=%zu calls=%" PRIu64 "\n",
           mir_compiled,
           mir_interpret_only,
           mir_cold,
           mir_calls);
#endif

    index = state.slot_count;
    while (index != 0u) {
        --index;
        if (state.slots[index] != NULL) {
            spec_slot_destroy(state.slots[index]);
            free(state.slots[index]);
        }
    }
    free(state.slots);

    index = state.retained_failure_count;
    while (index != 0u) {
        --index;
        if (state.retained_failures[index] != NULL) {
            spec_slot_destroy(state.retained_failures[index]);
            free(state.retained_failures[index]);
        }
    }
    free(state.retained_failures);

    if (state.spectest_ready) {
        turbowasm_instance_destroy(&state.spectest_instance);
        turbowasm_module_destroy(&state.spectest_module);
    }
    turbowasm_linker_destroy(&state.linker);

    if (rc != 0)
        return rc;
    return state.failed == 0u ? 0 : 1;
}
