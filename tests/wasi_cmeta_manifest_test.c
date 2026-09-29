#include <turbowasm/wasi.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <string.h>

static void test_manifest_is_valid_and_unique(void) {
    size_t count = turbowasm_wasi_preview1_function_count();
    size_t i;
    size_t j;

    assert(count == 21u);
    for (i = 0u; i < count; ++i) {
        const cmeta_function_desc *function =
            turbowasm_wasi_preview1_function_at(i);
        assert(function != NULL);
        assert(cmeta_function_desc_valid(function));
        assert(function->name != NULL);
        assert(function->name[0] != '\0');

        for (j = i + 1u; j < count; ++j) {
            const cmeta_function_desc *other =
                turbowasm_wasi_preview1_function_at(j);
            assert(other != NULL);
            assert(strcmp(function->name, other->name) != 0);
        }

        assert(turbowasm_wasi_preview1_find_function(
                   function->name) == function);
    }

    assert(turbowasm_wasi_preview1_function_at(count) == NULL);
    assert(turbowasm_wasi_preview1_find_function(NULL) == NULL);
    assert(turbowasm_wasi_preview1_find_function("") == NULL);
    assert(turbowasm_wasi_preview1_find_function(
               "not_a_preview1_import") == NULL);
}

static void test_representative_signatures(void) {
    const cmeta_function_desc *clock =
        turbowasm_wasi_preview1_find_function("clock_time_get");
    const cmeta_function_desc *seek =
        turbowasm_wasi_preview1_find_function("fd_seek");
    const cmeta_function_desc *path_open =
        turbowasm_wasi_preview1_find_function("path_open");
    const cmeta_function_desc *proc_exit =
        turbowasm_wasi_preview1_find_function("proc_exit");

    assert(clock != NULL);
    assert(clock->param_count == 3u);
    assert(cmeta_type_equal(
        cmeta_function_param(clock, 0u)->type,
        &cmeta_type_uint32));
    assert(cmeta_type_equal(
        cmeta_function_param(clock, 1u)->type,
        &cmeta_type_uint64));
    assert(cmeta_type_equal(
        cmeta_function_param(clock, 2u)->type,
        &cmeta_type_uint32));
    assert(cmeta_type_equal(
        clock->return_type,
        &cmeta_type_uint32));

    assert(seek != NULL);
    assert(seek->param_count == 4u);
    assert(cmeta_type_equal(
        cmeta_function_param(seek, 1u)->type,
        &cmeta_type_uint64));

    assert(path_open != NULL);
    assert(path_open->param_count == 9u);
    assert(cmeta_type_equal(
        cmeta_function_param(path_open, 5u)->type,
        &cmeta_type_uint64));
    assert(cmeta_type_equal(
        cmeta_function_param(path_open, 6u)->type,
        &cmeta_type_uint64));

    assert(proc_exit != NULL);
    assert(proc_exit->param_count == 1u);
    assert(cmeta_type_equal(
        proc_exit->return_type,
        &cmeta_type_void));
}

int main(void) {
    test_manifest_is_valid_and_unique();
    test_representative_signatures();
    return 0;
}
