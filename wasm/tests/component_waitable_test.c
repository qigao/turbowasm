#include "component_waitable.h"
#include "runtime_alloc.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static turbowasm_component_resource_table table;
static turbowasm_component_waitable_set sets[2];
static turbowasm_component_waitable items[5];
static turbowasm_component_resource_handle loan_handle;
static unsigned releases;
static bool release_fails;
static turbowasm_runtime_config allocation_config;
static turbowasm_runtime_scope allocation_scope;
static struct { size_t live; bool fail; } allocations;

static void *allocate(void *context, size_t size) {
    void *pointer;
    (void)context;
    if (allocations.fail) return NULL;
    pointer = malloc(size);
    if (pointer != NULL) ++allocations.live;
    return pointer;
}

static void deallocate(void *context, void *pointer) {
    (void)context;
    if (pointer != NULL) --allocations.live;
    free(pointer);
}

static turbowasm_value rep(void) {
    turbowasm_value value = {0};
    value.kind = TURBOWASM_VALUE_I32;
    value.as.i32 = 42;
    return value;
}

static void ready_stream(turbowasm_component_waitable *item, uint32_t count) {
    check_true(turbowasm_component_endpoint_begin_copy(&item->state.endpoint));
    check_true(turbowasm_component_endpoint_notify(&item->state.endpoint, count));
}

static turbowasm_status release_loan(void *context) {
    turbowasm_component_waitable *item = context;
    turbowasm_component_event event;
    turbowasm_component_resource_handle extra[16];
    unsigned i;
    ++releases;
    check_equal(turbowasm_component_waitable_drop(&table, item->handle), TURBOWASM_TRAPPED);
    check_equal(turbowasm_component_waitable_join(&table, item->handle, 0u), TURBOWASM_TRAPPED);
    check_equal(turbowasm_component_waitable_take(&table, item->handle, &event), TURBOWASM_TRAPPED);
    check_equal(turbowasm_component_resource_lend_release(&table, loan_handle, 42u), TURBOWASM_OK);
    /* Grow the shared slot array during delivery: no borrowed entry pointer
     * may survive this hook. Resource reps and waitable owners remain stable. */
    for (i = 0u; i < 16u; ++i)
        check_equal(turbowasm_component_resource_new_owned(&table, 42u, rep(), &extra[i]), TURBOWASM_OK);
    for (i = 0u; i < 16u; ++i)
        check_equal(turbowasm_component_resource_drop(&table, extra[i], 42u, NULL, NULL), TURBOWASM_OK);
    return release_fails ? TURBOWASM_TRAPPED : TURBOWASM_OK;
}

spec("Component waitables and shared canonical handles") {
    before_each() {
        memset(sets, 0, sizeof(sets));
        memset(items, 0, sizeof(items));
        loan_handle = 0u;
        releases = 0u;
        release_fails = false;
        memset(&allocations, 0, sizeof(allocations));
        turbowasm_runtime_config_init(&allocation_config);
        allocation_config.allocator.allocate = allocate;
        allocation_config.allocator.deallocate = deallocate;
        allocation_scope = turbowasm_runtime_scope_enter(&allocation_config);
    }
    after_each() {
        /* On a failed assertion all fixture owners still outlive this table;
         * no callback/storage is owned by a table slot. */
        turbowasm_component_resource_table_destroy(&table);
        turbowasm_runtime_scope_leave(allocation_scope);
        check_equal(allocations.live, 0u);
    }

    it("shares quota and generations without confusing resource and async handles") {
        turbowasm_component_resource_handle resource, stale;
        turbowasm_value value = rep();
        void *object = &table;
        check_true(turbowasm_component_resource_table_init(&table, 2u));
        check_equal(turbowasm_component_resource_new_owned(&table, 42u, rep(), &resource), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
        check_not_equal(resource, sets[0].handle);
        check_equal(turbowasm_component_resource_rep(&table, sets[0].handle, 42u, &value), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_resource_lend_acquire(&table, sets[0].handle, 42u), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_resource_lend_release(&table, sets[0].handle, 42u), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_resource_take_owned(&table, sets[0].handle, 42u, &value), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_resource_drop(&table, sets[0].handle, 42u, NULL, NULL), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_set_drop(&table, resource), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_handle_remove(&table, resource,
            TURBOWASM_COMPONENT_HANDLE_WAITABLE_SET, &object), TURBOWASM_TRAPPED);
        check_true(object == &table);
        check_equal(turbowasm_component_waitable_register(&table,
            TURBOWASM_COMPONENT_HANDLE_STREAM_READ, &items[0]), TURBOWASM_OUT_OF_MEMORY);
        check_null(items[0].table);
        check_equal(items[0].handle, 0u);
        stale = sets[0].handle;
        check_equal(turbowasm_component_waitable_set_drop(&table, stale), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_register(&table,
            TURBOWASM_COMPONENT_HANDLE_STREAM_READ, &items[0]), TURBOWASM_OK);
        check_not_equal(items[0].handle, stale);
        check_equal(turbowasm_component_handle_kind_get(&table, stale), TURBOWASM_COMPONENT_HANDLE_INVALID);
        check_equal(turbowasm_component_resource_rep(&table, resource, 42u, &value), TURBOWASM_OK);
        check_equal(value.as.i32, 42);
        check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_resource_drop(&table, resource, 42u, NULL, NULL), TURBOWASM_OK);
        check_equal(table.live_count, 0u);
    }

    it("moves membership atomically and rejects stale or wrong-kind targets") {
        turbowasm_component_resource_handle old_set;
        check_true(turbowasm_component_resource_table_init(&table, 8u));
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[1]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_register(&table,
            TURBOWASM_COMPONENT_HANDLE_FUTURE_READ, &items[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&table, items[0].handle, sets[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_join(&table, items[0].handle, items[0].handle), TURBOWASM_TRAPPED);
        check_equal(items[0].set_handle, sets[0].handle);
        check_equal(turbowasm_component_waitable_join(&table, items[0].handle, sets[1].handle), TURBOWASM_OK);
        old_set = sets[0].handle;
        check_equal(turbowasm_component_waitable_set_drop(&table, old_set), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&table, items[0].handle, old_set), TURBOWASM_TRAPPED);
        check_equal(items[0].set_handle, sets[1].handle);
        check_equal(turbowasm_component_waitable_join(&table, items[0].handle, 0u), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[1].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_OK);
    }

    it("pins empty sets during waits and rejects counter overflow or underflow") {
        turbowasm_component_event event = {TURBOWASM_COMPONENT_EVENT_SUBTASK, 7u, 8u};
        check_true(turbowasm_component_resource_table_init(&table, 1u));
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
        check_equal(event.code, TURBOWASM_COMPONENT_EVENT_NONE);
        check_equal(event.handle, 0u);
        check_equal(event.payload, 0u);
        check_equal(turbowasm_component_waitable_set_wait_acquire(&table, sets[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_wait_acquire(&table, sets[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_set_wait_release(&table, sets[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_TRAPPED);
        sets[0].wait_count = UINT32_MAX;
        check_equal(turbowasm_component_waitable_set_wait_acquire(&table, sets[0].handle), TURBOWASM_TRAPPED);
        check_equal(sets[0].wait_count, UINT32_MAX);
        sets[0].wait_count = 1u;
        check_equal(turbowasm_component_waitable_set_wait_release(&table, sets[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_wait_release(&table, sets[0].handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_OK);
    }

    it("polls all five waitable kinds with exact canonical event codes") {
        unsigned i;
        turbowasm_component_event event;
        check_true(turbowasm_component_resource_table_init(&table, 8u));
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
        for (i = 0u; i < 5u; ++i) {
            check_equal(turbowasm_component_waitable_register(&table,
                (turbowasm_component_handle_kind)(TURBOWASM_COMPONENT_HANDLE_SUBTASK + i), &items[i]), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_join(&table, items[i].handle, sets[0].handle), TURBOWASM_OK);
            if (i == 0u) {
                check_true(turbowasm_component_subtask_start(&items[i].state.subtask));
                check_true(turbowasm_component_subtask_resolve(&items[i].state.subtask, false));
            } else {
                ready_stream(&items[i], 1u);
            }
        }
        for (i = 0u; i < 5u; ++i) {
            check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
            check_equal((unsigned)event.code, i + 1u);
            check_equal(event.handle, items[i].handle);
            check_equal(event.payload, i == 0u ? 2u : (i < 3u ? 16u : 0u));
            check_equal(turbowasm_component_waitable_drop(&table, items[i].handle), TURBOWASM_OK);
        }
        check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
        check_equal(event.code, TURBOWASM_COMPONENT_EVENT_NONE);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_OK);
    }

    it("rotates delivery so a repeatedly ready low slot cannot starve another member") {
        unsigned i;
        turbowasm_component_event event;
        check_true(turbowasm_component_resource_table_init(&table, 8u));
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
        for (i = 0u; i < 2u; ++i) {
            check_equal(turbowasm_component_waitable_register(&table,
                TURBOWASM_COMPONENT_HANDLE_STREAM_READ, &items[i]), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_join(&table, items[i].handle, sets[0].handle), TURBOWASM_OK);
            ready_stream(&items[i], 1u);
        }
        check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
        check_equal(event.handle, items[0].handle);
        ready_stream(&items[0], 2u);
        check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
        check_equal(event.handle, items[1].handle);
        check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
        check_equal(event.handle, items[0].handle);
        check_equal(event.payload, 32u);
        for (i = 0u; i < 2u; ++i)
            check_equal(turbowasm_component_waitable_drop(&table, items[i].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_OK);
    }

    it("prevents event theft and membership changes during an individual synchronous wait") {
        turbowasm_component_event event = {TURBOWASM_COMPONENT_EVENT_NONE, 7u, 8u};
        check_true(turbowasm_component_resource_table_init(&table, 2u));
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_register(&table,
            TURBOWASM_COMPONENT_HANDLE_STREAM_READ, &items[0]), TURBOWASM_OK);
        check_true(turbowasm_component_endpoint_begin_copy(&items[0].state.endpoint));
        check_equal(turbowasm_component_waitable_wait_begin(&table, items[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_end(&table, items[0].handle, &event), TURBOWASM_YIELDED);
        check_equal(event.handle, 7u);
        check_true(items[0].sync_waiter);
        check_equal(turbowasm_component_waitable_join(&table, items[0].handle, sets[0].handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_TRAPPED);
        check_true(turbowasm_component_endpoint_notify(&items[0].state.endpoint, 3u));
        check_equal(turbowasm_component_waitable_take(&table, items[0].handle, &event), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_wait_end(&table, items[0].handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 48u);
        check_false(items[0].sync_waiter);
        check_equal(turbowasm_component_waitable_join(&table, items[0].handle, sets[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_begin(&table, items[0].handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_OK);
    }

    it("cancels a wait pin without consuming a pending completion") {
        turbowasm_component_event event;
        check_true(turbowasm_component_resource_table_init(&table, 1u));
        check_equal(turbowasm_component_waitable_register(&table,
            TURBOWASM_COMPONENT_HANDLE_FUTURE_WRITE, &items[0]), TURBOWASM_OK);
        ready_stream(&items[0], 1u);
        check_equal(turbowasm_component_waitable_wait_begin(&table, items[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_cancel(&table, items[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_wait_cancel(&table, items[0].handle), TURBOWASM_TRAPPED);
        check_true(items[0].state.endpoint.pending_event);
        check_equal(turbowasm_component_waitable_take(&table, items[0].handle, &event), TURBOWASM_OK);
        check_equal(event.code, TURBOWASM_COMPONENT_EVENT_FUTURE_WRITE);
        check_equal(event.payload, TURBOWASM_COMPONENT_COPY_COMPLETED);
        check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_OK);
    }

    it("retains an in-flight endpoint through cancellation and failed event output admission") {
        turbowasm_component_event event;
        check_true(turbowasm_component_resource_table_init(&table, 2u));
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_register(&table,
            TURBOWASM_COMPONENT_HANDLE_FUTURE_READ, &items[0]), TURBOWASM_OK);
        check_true(turbowasm_component_endpoint_begin_copy(&items[0].state.endpoint));
        check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_join(&table, items[0].handle, sets[0].handle), TURBOWASM_OK);
        check_true(turbowasm_component_endpoint_request_cancel(&items[0].state.endpoint, true));
        check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_TRAPPED);
        check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
        check_equal(event.code, TURBOWASM_COMPONENT_EVENT_NONE);
        check_true(turbowasm_component_endpoint_notify(&items[0].state.endpoint, 0u));
        check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_equal(turbowasm_component_waitable_take(&table, items[0].handle, NULL), TURBOWASM_INVALID_ARGUMENT);
        check_true(items[0].state.endpoint.pending_event);
        check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
        check_equal(event.payload, TURBOWASM_COMPONENT_COPY_CANCELLED);
        check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_OK);
    }

    it("releases real resource loans once on terminal delivery despite table growth and hook failure") {
        unsigned failure;
        for (failure = 0u; failure < 2u; ++failure) {
            turbowasm_component_event event = {TURBOWASM_COMPONENT_EVENT_NONE, 7u, 8u};
            memset(sets, 0, sizeof(sets)); memset(items, 0, sizeof(items));
            releases = 0u; release_fails = failure != 0u;
            check_true(turbowasm_component_resource_table_init(&table, 32u));
            check_equal(turbowasm_component_resource_new_owned(&table, 42u, rep(), &loan_handle), TURBOWASM_OK);
            check_equal(turbowasm_component_resource_lend_acquire(&table, loan_handle, 42u), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
            items[0].release_pending = release_loan; items[0].release_context = &items[0];
            check_equal(turbowasm_component_waitable_register(&table,
                TURBOWASM_COMPONENT_HANDLE_SUBTASK, &items[0]), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_join(&table, items[0].handle, sets[0].handle), TURBOWASM_OK);
            check_true(turbowasm_component_subtask_start(&items[0].state.subtask));
            check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
            check_equal(event.payload, (uint32_t)TURBOWASM_COMPONENT_SUBTASK_STARTED);
            check_equal(releases, 0u);
            check_equal(turbowasm_component_resource_drop(&table, loan_handle, 42u, NULL, NULL), TURBOWASM_TRAPPED);
            check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_TRAPPED);
            check_true(turbowasm_component_subtask_resolve(&items[0].state.subtask, false));
            event.handle = 7u;
            check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event),
                failure ? TURBOWASM_TRAPPED : TURBOWASM_OK);
            check_equal(event.handle, failure ? 7u : items[0].handle);
            check_equal(releases, 1u);
            check_greater(table.capacity, 8u);
            check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
            check_equal(event.code, TURBOWASM_COMPONENT_EVENT_NONE);
            check_equal(releases, 1u);
            check_equal(turbowasm_component_resource_drop(&table, loan_handle, 42u, NULL, NULL), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_OK);
            check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_OK);
            turbowasm_component_resource_table_destroy(&table);
        }
    }

    it("preserves registrations and pending events when shared table growth fails") {
        turbowasm_runtime_config config;
        turbowasm_runtime_scope scope;
        turbowasm_component_resource_handle handles[7];
        turbowasm_component_event event;
        turbowasm_status status;
        unsigned i;
        check_true(turbowasm_component_resource_table_init(&table, 16u));
        config = allocation_config;
        config.limits.max_allocation_bytes = 1u;
        scope = turbowasm_runtime_scope_enter(&config);
        status = turbowasm_component_waitable_set_register(&table, &sets[0]);
        turbowasm_runtime_scope_leave(scope);
        check_equal(status, TURBOWASM_OUT_OF_MEMORY);
        check_null(sets[0].table); check_equal(sets[0].handle, 0u); check_equal(table.live_count, 0u);
        check_equal(turbowasm_component_waitable_register(&table,
            TURBOWASM_COMPONENT_HANDLE_STREAM_READ, &items[0]), TURBOWASM_OK);
        ready_stream(&items[0], 2u);
        for (i = 0u; i < 7u; ++i)
            check_equal(turbowasm_component_resource_new_owned(&table, 42u, rep(), &handles[i]), TURBOWASM_OK);
        /* Realloc retains the allocator/limit that created the storage. Inject
         * failure into that allocator, rather than an unrelated current scope. */
        allocations.fail = true;
        status = turbowasm_component_waitable_set_register(&table, &sets[0]);
        allocations.fail = false;
        check_equal(status, TURBOWASM_OUT_OF_MEMORY);
        check_equal(table.live_count, 8u);
        check_null(sets[0].table);
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_join(&table, items[0].handle, sets[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_poll(&table, sets[0].handle, &event), TURBOWASM_OK);
        check_equal(event.payload, 32u);
        for (i = 0u; i < 7u; ++i)
            check_equal(turbowasm_component_resource_drop(&table, handles[i], 42u, NULL, NULL), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_drop(&table, items[0].handle), TURBOWASM_OK);
        check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_OK);
    }

    it("retires exhausted generations across alternating resource and set allocations") {
        uint32_t generation;
        turbowasm_component_resource_handle first = 0u;
        check_true(turbowasm_component_resource_table_init(&table, 1u));
        for (generation = 1u; generation <= TURBOWASM_COMPONENT_RESOURCE_MAX_GENERATION; ++generation) {
            if (generation % 2u) {
                check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OK);
                if (first == 0u) first = sets[0].handle;
                check_equal(sets[0].handle >> 16u, generation);
                check_equal(turbowasm_component_waitable_set_drop(&table, sets[0].handle), TURBOWASM_OK);
            } else {
                turbowasm_component_resource_handle handle;
                check_equal(turbowasm_component_resource_new_owned(&table, 42u, rep(), &handle), TURBOWASM_OK);
                check_equal(handle >> 16u, generation);
                check_equal(turbowasm_component_resource_drop(&table, handle, 42u, NULL, NULL), TURBOWASM_OK);
            }
            check_equal(turbowasm_component_handle_kind_get(&table, first), TURBOWASM_COMPONENT_HANDLE_INVALID);
        }
        check_equal(turbowasm_component_waitable_set_register(&table, &sets[0]), TURBOWASM_OUT_OF_MEMORY);
        check_equal(table.live_count, 0u);
        check_true(table.entries[0].retired);
    }
}
