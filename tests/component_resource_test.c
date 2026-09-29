#include "../src/component_resource.h"
#include "../src/runtime_alloc.h"

#include <turbowasm/turbowasm.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

typedef struct destructor_probe {
    uint32_t calls;
    uint64_t identity;
    turbowasm_value rep;
} destructor_probe;

static turbowasm_status probe_destructor(
    void *context,
    uint64_t resource_identity,
    turbowasm_value rep) {
    destructor_probe *probe = (destructor_probe *)context;
    assert(probe != NULL);
    ++probe->calls;
    probe->identity = resource_identity;
    probe->rep = rep;
    return TURBOWASM_OK;
}

static turbowasm_value i32_rep(int32_t value) {
    turbowasm_value rep = {0};
    rep.kind = TURBOWASM_VALUE_I32;
    rep.as.i32 = value;
    return rep;
}

static uint32_t generation_of(
    turbowasm_component_resource_handle handle) {
    return handle >> 16u;
}

static void test_new_rep_lend_drop(void) {
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_handle handle;
    turbowasm_component_resource_handle replacement;
    turbowasm_value rep = {0};
    destructor_probe probe = {0};

    assert(turbowasm_component_resource_table_init(&table, 4u));
    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(0x1001), i32_rep(42),
               &handle) == TURBOWASM_OK);
    assert(handle != 0u);
    assert(handle <= TURBOWASM_COMPONENT_RESOURCE_MAX_HANDLE);
    assert(table.live_count == 1u);

    assert(turbowasm_component_resource_rep(
               &table, handle, UINT64_C(0x1001),
               &rep) == TURBOWASM_OK);
    assert(rep.kind == TURBOWASM_VALUE_I32);
    assert(rep.as.i32 == 42);
    assert(turbowasm_component_resource_rep(
               &table, handle, UINT64_C(0x1002),
               &rep) == TURBOWASM_TRAPPED);

    assert(turbowasm_component_resource_lend_acquire(
               &table, handle, UINT64_C(0x1001)) == TURBOWASM_OK);
    assert(turbowasm_component_resource_drop(
               &table, handle, UINT64_C(0x1001),
               probe_destructor, &probe) == TURBOWASM_TRAPPED);
    assert(probe.calls == 0u);
    assert(turbowasm_component_resource_lend_release(
               &table, handle, UINT64_C(0x1001)) == TURBOWASM_OK);

    assert(turbowasm_component_resource_drop(
               &table, handle, UINT64_C(0x1001),
               probe_destructor, &probe) == TURBOWASM_OK);
    assert(probe.calls == 1u);
    assert(probe.identity == UINT64_C(0x1001));
    assert(probe.rep.kind == TURBOWASM_VALUE_I32);
    assert(probe.rep.as.i32 == 42);
    assert(table.live_count == 0u);
    assert(turbowasm_component_resource_rep(
               &table, handle, UINT64_C(0x1001),
               &rep) == TURBOWASM_TRAPPED);

    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(0x1001), i32_rep(43),
               &replacement) == TURBOWASM_OK);
    assert(replacement != handle);
    assert(generation_of(replacement) ==
           generation_of(handle) + 1u);
    assert(turbowasm_component_resource_rep(
               &table, handle, UINT64_C(0x1001),
               &rep) == TURBOWASM_TRAPPED);
    assert(turbowasm_component_resource_rep(
               &table, replacement, UINT64_C(0x1001),
               &rep) == TURBOWASM_OK);
    assert(rep.as.i32 == 43);

    assert(turbowasm_component_resource_drop(
               &table, replacement, UINT64_C(0x1001),
               NULL, NULL) == TURBOWASM_OK);
    turbowasm_component_resource_table_destroy(&table);
}

static void test_owned_take_and_borrowed_drop(void) {
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_handle owned;
    turbowasm_component_resource_handle borrowed;
    turbowasm_value rep = {0};
    destructor_probe probe = {0};

    assert(turbowasm_component_resource_table_init(&table, 4u));

    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(0x2001), i32_rep(77),
               &owned) == TURBOWASM_OK);
    assert(turbowasm_component_resource_take_owned(
               &table, owned, UINT64_C(0x2001),
               &rep) == TURBOWASM_OK);
    assert(rep.kind == TURBOWASM_VALUE_I32);
    assert(rep.as.i32 == 77);
    assert(table.live_count == 0u);
    assert(turbowasm_component_resource_rep(
               &table, owned, UINT64_C(0x2001),
               &rep) == TURBOWASM_TRAPPED);

    assert(turbowasm_component_resource_new_borrowed(
               &table, UINT64_C(0x2001), i32_rep(88),
               &borrowed) == TURBOWASM_OK);
    assert(turbowasm_component_resource_rep(
               &table, borrowed, UINT64_C(0x2001),
               &rep) == TURBOWASM_OK);
    assert(rep.as.i32 == 88);

    assert(turbowasm_component_resource_take_owned(
               &table, borrowed, UINT64_C(0x2001),
               &rep) == TURBOWASM_TRAPPED);

    assert(turbowasm_component_resource_drop(
               &table, borrowed, UINT64_C(0x2001),
               probe_destructor, &probe) == TURBOWASM_OK);
    assert(probe.calls == 0u);
    assert(table.live_count == 0u);

    turbowasm_component_resource_table_destroy(&table);
}

static void test_take_owned_rejects_active_lend(void) {
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_handle handle;
    turbowasm_value rep = {0};

    assert(turbowasm_component_resource_table_init(&table, 2u));
    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(0x3001), i32_rep(99),
               &handle) == TURBOWASM_OK);
    assert(turbowasm_component_resource_lend_acquire(
               &table, handle, UINT64_C(0x3001)) == TURBOWASM_OK);
    assert(turbowasm_component_resource_take_owned(
               &table, handle, UINT64_C(0x3001),
               &rep) == TURBOWASM_TRAPPED);
    assert(turbowasm_component_resource_lend_release(
               &table, handle, UINT64_C(0x3001)) == TURBOWASM_OK);
    assert(turbowasm_component_resource_take_owned(
               &table, handle, UINT64_C(0x3001),
               &rep) == TURBOWASM_OK);
    assert(rep.as.i32 == 99);

    turbowasm_component_resource_table_destroy(&table);
}

static void test_generation_never_wraps(void) {
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_handle first = 0u;
    turbowasm_component_resource_handle handle = 0u;
    turbowasm_value rep = {0};
    uint32_t generation;

    assert(turbowasm_component_resource_table_init(&table, 1u));

    for (generation = 1u;
         generation <= TURBOWASM_COMPONENT_RESOURCE_MAX_GENERATION;
         ++generation) {
        assert(turbowasm_component_resource_new_owned(
                   &table, UINT64_C(9),
                   i32_rep((int32_t)generation),
                   &handle) == TURBOWASM_OK);
        assert(generation_of(handle) == generation);
        if (generation == 1u)
            first = handle;
        assert(turbowasm_component_resource_rep(
                   &table, first, UINT64_C(9), &rep) ==
               (generation == 1u
                    ? TURBOWASM_OK
                    : TURBOWASM_TRAPPED));
        assert(turbowasm_component_resource_drop(
                   &table, handle, UINT64_C(9),
                   NULL, NULL) == TURBOWASM_OK);
    }

    assert(table.live_count == 0u);
    assert(table.entries[0].retired);
    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(9), i32_rep(1),
               &handle) == TURBOWASM_OUT_OF_MEMORY);
    assert(turbowasm_component_resource_rep(
               &table, first, UINT64_C(9), &rep) ==
           TURBOWASM_TRAPPED);

    turbowasm_component_resource_table_destroy(&table);
}

static void test_capacity_is_bounded(void) {
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_handle a;
    turbowasm_component_resource_handle b;
    turbowasm_component_resource_handle c;

    assert(turbowasm_component_resource_table_init(&table, 2u));
    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(1), i32_rep(1), &a) ==
           TURBOWASM_OK);
    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(2), i32_rep(2), &b) ==
           TURBOWASM_OK);
    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(3), i32_rep(3), &c) ==
           TURBOWASM_OUT_OF_MEMORY);

    assert(turbowasm_component_resource_drop(
               &table, a, UINT64_C(1), NULL, NULL) ==
           TURBOWASM_OK);
    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(3), i32_rep(3), &c) ==
           TURBOWASM_OK);
    assert(c != a);

    assert(turbowasm_component_resource_drop(
               &table, b, UINT64_C(2), NULL, NULL) ==
           TURBOWASM_OK);
    assert(turbowasm_component_resource_drop(
               &table, c, UINT64_C(3), NULL, NULL) ==
           TURBOWASM_OK);
    turbowasm_component_resource_table_destroy(&table);
}

static void test_runtime_allocation_limit(void) {
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_handle handle = 0u;
    turbowasm_runtime_config config;
    turbowasm_runtime_scope scope;

    turbowasm_runtime_config_init(&config);
    config.limits.max_allocation_bytes = 1u;

    scope = turbowasm_runtime_scope_enter(&config);
    assert(turbowasm_component_resource_table_init(&table, 8u));
    assert(turbowasm_component_resource_new_owned(
               &table, UINT64_C(1), i32_rep(1),
               &handle) == TURBOWASM_OUT_OF_MEMORY);
    turbowasm_runtime_scope_leave(scope);

    assert(table.entries == NULL);
    assert(table.live_count == 0u);
    turbowasm_component_resource_table_destroy(&table);
}

static void test_invalid_inputs(void) {
    turbowasm_component_resource_table table = {0};
    turbowasm_component_resource_handle handle = 0u;
    turbowasm_value rep = {0};

    assert(!turbowasm_component_resource_table_init(
        &table, TURBOWASM_COMPONENT_RESOURCE_MAX_SLOTS + 1u));
    assert(turbowasm_component_resource_table_init(&table, 1u));

    assert(turbowasm_component_resource_new_owned(
               &table, 0u, i32_rep(0), &handle) ==
           TURBOWASM_INVALID_ARGUMENT);
    assert(turbowasm_component_resource_rep(
               &table, 0u, UINT64_C(1), &rep) ==
           TURBOWASM_TRAPPED);
    assert(turbowasm_component_resource_lend_release(
               &table, 1u, UINT64_C(1)) ==
           TURBOWASM_TRAPPED);

    turbowasm_component_resource_table_destroy(&table);
}

int main(void) {
    test_new_rep_lend_drop();
    test_owned_take_and_borrowed_drop();
    test_take_owned_rejects_active_lend();
    test_generation_never_wraps();
    test_capacity_is_bounded();
    test_runtime_allocation_limit();
    test_invalid_inputs();
    return 0;
}
