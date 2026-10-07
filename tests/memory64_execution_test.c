#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "jit_memory_helper.h"
#include "jit_simd_helper.h"
#include "fixtures/memory64_native.h"
#include "fixtures/memory64_shared.h"
#include "fixtures/memory64_shared_import.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <string.h>

enum { N_SIZE, N_GROW, N_LOAD, N_STORE, N_HIGH, N_OVERFLOW, N_FILL,
       N_COPY, N_INIT, N_DROP, N_MIXED, N_F32, N_F64, N_SIMD,
       N_SIMD_HIGH, N_BLOCK, N_SIGNED, N_FILL_HIGH,
       N_LOAD_F32_ARG, N_STORE_F32_ARG, N_STORE_F64_MIXED,
       N_SUM_FOUR, N_MIXED_I32 };
enum { S_LOAD, S_STORE, S_ADD, S_CMPXCHG, S_NARROW, S_HIGH, S_OVERFLOW,
       S_WAIT32, S_WAIT64, S_NOTIFY, S_GROW, S_SIZE, S_WAIT_BLOCKING,
       S_PLAIN, S_BULK, S_SIMD, S_FENCE };

static turbowasm_module module;
static turbowasm_instance instance;
static turbowasm_module imported_module;
static turbowasm_instance workers[2];
static cmeta_thread_t threads[2];

enum { WORKER_ITERATIONS = 1000, WAITER_POLL_LIMIT = 1000, ARTIFACT_CAPACITY = 32768 };
typedef struct worker_call {
    turbowasm_instance *instance;
    uint32_t function;
    uint32_t iterations;
    turbowasm_status status;
    turbowasm_trap trap;
    turbowasm_value result;
} worker_call;
static worker_call calls[2];

static bool interrupt_wait(void *context) {
    unsigned *checks = context;
    /* Four instruction checkpoints precede the wait's own interrupt check. */
    return ++*checks >= 5u;
}

static void attach_backend(turbowasm_instance *target) {
#ifdef TURBOWASM_TEST_MIR
    turbowasm_jit_backend backend = {0};
    check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
    check_equal(turbowasm_jit_instance_attach_backend(target->impl, &backend, 1u), TURBOWASM_OK);
#else
    (void)target;
#endif
}

static void run_worker(void *argument) {
    worker_call *call = argument;
    uint32_t index;
    for (index = 0u; index < call->iterations; ++index) {
        size_t count = 0u;
        call->status = turbowasm_instance_invoke(call->instance,
            call->function, NULL, 0u, &call->result, 1u, &count, &call->trap);
#ifdef TURBOWASM_TEST_MIR
        if (((turbowasm_instance_impl *)call->instance->impl)->jit_functions[
                call->function].state != TURBOWASM_JIT_COMPILED) {
            call->status = TURBOWASM_UNSUPPORTED;
            break;
        }
#endif
        if (call->status != TURBOWASM_OK)
            break;
        if (count != 1u) {
            call->status = TURBOWASM_TYPE_MISMATCH;
            break;
        }
    }
}

static void create_workers(void) {
    turbowasm_linker linker = {0};
    turbowasm_status status;
    check_equal(turbowasm_module_load_borrowed(&imported_module,
        memory64_shared_import_bytes, sizeof(memory64_shared_import_bytes)), TURBOWASM_OK);
    check_equal(turbowasm_linker_init(&linker), TURBOWASM_OK);
    status = turbowasm_linker_define_instance(&linker,
        (turbowasm_name){(const uint8_t *)"p", 1u}, &instance);
    if (status == TURBOWASM_OK)
        status = turbowasm_instance_create_linked(&workers[0], &imported_module, &linker);
    if (status == TURBOWASM_OK)
        status = turbowasm_instance_create_linked(&workers[1], &imported_module, &linker);
    turbowasm_linker_destroy(&linker);
    check_equal(status, TURBOWASM_OK);
    attach_backend(&workers[0]);
    attach_backend(&workers[1]);
}

static void load_fixture(bool shared) {
    const uint8_t *bytes = shared ? memory64_shared_bytes : memory64_native_bytes;
    size_t size = shared ? sizeof(memory64_shared_bytes) : sizeof(memory64_native_bytes);
    check_equal(turbowasm_module_load_borrowed(&module, bytes, size), TURBOWASM_OK);
    check_equal(turbowasm_instance_create(&instance, &module), TURBOWASM_OK);
    attach_backend(&instance);
}

static turbowasm_value invoke_values(uint32_t function, size_t count,
    const turbowasm_value *args,
    turbowasm_status expected_status, turbowasm_trap expected_trap) {
    turbowasm_value result = {0};
    size_t result_count = 0u;
    turbowasm_trap trap = TURBOWASM_TRAP_NONE;
    check_equal(turbowasm_instance_invoke(&instance, function, args, count,
        &result, 1u, &result_count, &trap), expected_status);
    check_equal(trap, expected_trap);
    check_equal(result_count, expected_status == TURBOWASM_OK ? (size_t)1 : (size_t)0);
#ifdef TURBOWASM_TEST_MIR
    {
        turbowasm_instance_impl *impl = instance.impl;
        check(impl->jit_functions[function].state == TURBOWASM_JIT_COMPILED,
            "function %u must compile; tier state is %d", function,
            (int)impl->jit_functions[function].state);
        check_not_null(impl->jit_functions[function].compiled.impl);
    }
#endif
    return result;
}

static turbowasm_value invoke(uint32_t function, size_t count,
    int64_t a, int64_t b, turbowasm_status expected_status, turbowasm_trap expected_trap) {
    turbowasm_value args[2] = {{0}};
    args[0].kind = args[1].kind = TURBOWASM_VALUE_I64;
    args[0].as.i64 = a;
    args[1].as.i64 = b;
    return invoke_values(function, count, args, expected_status, expected_trap);
}

static int64_t integer(uint32_t function, size_t count, int64_t a, int64_t b) {
    turbowasm_value value = invoke(function, count, a, b,
        TURBOWASM_OK, TURBOWASM_TRAP_NONE);
    check_true(value.kind == TURBOWASM_VALUE_I32 || value.kind == TURBOWASM_VALUE_I64);
    return value.kind == TURBOWASM_VALUE_I32 ? value.as.i32 : value.as.i64;
}

spec("memory64 execution") {
    before_each() {
        memset(&module, 0, sizeof(module));
        memset(&instance, 0, sizeof(instance));
        memset(&imported_module, 0, sizeof(imported_module));
        memset(workers, 0, sizeof(workers));
        memset(threads, 0, sizeof(threads));
        memset(calls, 0, sizeof(calls));
    }
    after_each() {
        unsigned index;
        for (index = 0u; index < 2u; ++index) {
            if (threads[index] != NULL)
                check_equal(cmeta_thread_join(&threads[index]), 0);
            turbowasm_instance_destroy(&workers[index]);
        }
        turbowasm_module_destroy(&imported_module);
        turbowasm_instance_destroy(&instance);
        turbowasm_module_destroy(&module);
    }
    it("passes mixed scalar arguments and preserves float payload bits") {
        const uint32_t bits32[] = {UINT32_C(0x80000000), UINT32_C(0x7fc12345)};
        const uint64_t bits64[] = {UINT64_C(0x8000000000000000), UINT64_C(0x7ff8123456789abc)};
        turbowasm_value args[4] = {{0}};
        turbowasm_value saved[4];
        turbowasm_value result;
        size_t i;
        load_fixture(false);
        args[0].kind = TURBOWASM_VALUE_I64;
        args[0].as.i64 = 80;
        for (i = 0u; i < sizeof(bits32) / sizeof(bits32[0]); ++i) {
            uint32_t actual;
            args[1].kind = TURBOWASM_VALUE_F32;
            memcpy(&args[1].as.f32, &bits32[i], sizeof(actual));
            memcpy(saved, args, sizeof(args));
            result = invoke_values(N_STORE_F32_ARG, 2u, args,
                TURBOWASM_OK, TURBOWASM_TRAP_NONE);
            check_equal(result.kind, TURBOWASM_VALUE_F32);
            memcpy(&actual, &result.as.f32, sizeof(actual));
            check_equal(actual, bits32[i]);
            result = invoke_values(N_LOAD_F32_ARG, 1u, args,
                TURBOWASM_OK, TURBOWASM_TRAP_NONE);
            memcpy(&actual, &result.as.f32, sizeof(actual));
            check_equal(actual, bits32[i]);
            check_equal(memcmp(saved, args, sizeof(args)), 0);
        }
        args[2].kind = TURBOWASM_VALUE_I32;
        args[2].as.i32 = -1;
        args[3].kind = TURBOWASM_VALUE_I64;
        args[3].as.i64 = INT64_MIN;
        for (i = 0u; i < sizeof(bits64) / sizeof(bits64[0]); ++i) {
            uint64_t actual;
            args[1].kind = TURBOWASM_VALUE_F64;
            memcpy(&args[1].as.f64, &bits64[i], sizeof(actual));
            memcpy(saved, args, sizeof(args));
            result = invoke_values(N_STORE_F64_MIXED, 4u, args,
                TURBOWASM_OK, TURBOWASM_TRAP_NONE);
            check_equal(result.kind, TURBOWASM_VALUE_F64);
            memcpy(&actual, &result.as.f64, sizeof(actual));
            check_equal(actual, bits64[i]);
            check_equal(memcmp(saved, args, sizeof(args)), 0);
        }
        args[0].as.i64 = INT64_C(4294967296);
        invoke_values(N_STORE_F64_MIXED, 4u, args,
            TURBOWASM_TRAPPED, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
    }
    it("accepts more than two scalar parameters without truncating i32 values") {
        turbowasm_value args[4] = {{0}};
        turbowasm_value result;
        unsigned i;
        load_fixture(false);
        for (i = 0u; i < 4u; ++i) {
            args[i].kind = TURBOWASM_VALUE_I64;
            args[i].as.i64 = (int64_t)i + 1;
        }
        result = invoke_values(N_SUM_FOUR, 4u, args,
            TURBOWASM_OK, TURBOWASM_TRAP_NONE);
        check_equal(result.as.i64, INT64_C(10));
        args[0].kind = TURBOWASM_VALUE_F64;
        args[0].as.f64 = 1.0;
        args[2].kind = TURBOWASM_VALUE_F32;
        args[2].as.f32 = 2.0f;
        args[3].kind = TURBOWASM_VALUE_I32;
        args[3].as.i32 = INT32_MIN;
        result = invoke_values(N_MIXED_I32, 4u, args,
            TURBOWASM_OK, TURBOWASM_TRAP_NONE);
        check_equal(result.kind, TURBOWASM_VALUE_I32);
        check_equal(result.as.i32, INT32_MIN);
    }
    it("restores shared memory64 artifacts with the same execution semantics") {
        uint8_t artifact[ARTIFACT_CAPACITY];
        size_t artifact_size = 0u;
        load_fixture(true);
        check_equal(turbowasm_module_artifact_write(&module, artifact,
            sizeof(artifact), &artifact_size), TURBOWASM_OK);
        turbowasm_instance_destroy(&instance);
        turbowasm_module_destroy(&module);
        check_equal(turbowasm_module_load_borrowed_from_artifact(&module,
            memory64_shared_bytes, sizeof(memory64_shared_bytes),
            artifact, artifact_size), TURBOWASM_OK);
        check_equal(turbowasm_instance_create(&instance, &module), TURBOWASM_OK);
        attach_backend(&instance);
        check_equal(integer(S_STORE, 2, 0, 42), INT64_C(42));
        check_equal(integer(S_CMPXCHG, 1, 0, 0), INT64_C(42));
        check_equal(integer(S_LOAD, 1, 0, 0), INT64_C(99));
    }
    it("serializes concurrent imported atomic increments without lost updates") {
        unsigned index;
        load_fixture(true);
        create_workers();
        for (index = 0u; index < 2u; ++index) {
            calls[index].instance = &workers[index];
            calls[index].function = 0u;
            calls[index].iterations = WORKER_ITERATIONS;
            check_equal(cmeta_thread_create(&threads[index], run_worker, &calls[index]), 0);
        }
        for (index = 0u; index < 2u; ++index) {
            check_equal(cmeta_thread_join(&threads[index]), 0);
            check_equal(calls[index].status, TURBOWASM_OK);
            check_equal(calls[index].trap, TURBOWASM_TRAP_NONE);
        }
        check_equal(integer(S_LOAD, 1, 0, 0), (int64_t)(2 * WORKER_ITERATIONS));
    }
    it("preserves imported waiters across memory growth and notify") {
        turbowasm_instance_memory *memory;
        unsigned attempt;
        uint32_t waiter_count = 0u;
        load_fixture(true);
        create_workers();
        memory = &((turbowasm_instance_impl *)instance.impl)->memories[0];
        calls[0].instance = &workers[0];
        calls[0].function = 2u;
        calls[0].iterations = 1u;
        check_equal(cmeta_thread_create(&threads[0], run_worker, &calls[0]), 0);
        for (attempt = 0u; attempt < WAITER_POLL_LIMIT; ++attempt) {
            cmeta_mutex_lock(&memory->waiter_mutex);
            waiter_count = memory->waiter_count;
            cmeta_mutex_unlock(&memory->waiter_mutex);
            if (waiter_count == 1u)
                break;
            cmeta_sleep_ms(1u);
        }
        check_equal(waiter_count, 1u);
        calls[1].instance = &workers[1];
        calls[1].function = 4u;
        calls[1].iterations = 1u;
        run_worker(&calls[1]);
        check_equal(calls[1].status, TURBOWASM_OK);
        check_equal(calls[1].result.as.i64, INT64_C(1));
        calls[1].function = 3u;
        run_worker(&calls[1]);
        check_equal(calls[1].status, TURBOWASM_OK);
        check_equal(calls[1].result.as.i32, 1);
        check_equal(cmeta_thread_join(&threads[0]), 0);
        check_equal(calls[0].status, TURBOWASM_OK);
        check_equal(calls[0].result.as.i32, 0);
        check_equal(integer(S_SIZE, 0, 0, 0), INT64_C(2));
    }
    it("preserves full-width scalar addresses and growth failure") {
        load_fixture(false);
        check_equal(integer(N_SIZE, 0, 0, 0), INT64_C(1));
        check_equal(integer(N_STORE, 2, 8, INT64_MIN), INT64_MIN);
        check_equal(integer(N_BLOCK, 1, 8, 0), INT64_MIN);
        check_equal(integer(N_SIGNED, 0, 0, 0), INT64_C(-1));
        check_equal(integer(N_GROW, 1, INT64_C(4294967296), 0), INT64_C(-1));
        check_equal(integer(N_SIZE, 0, 0, 0), INT64_C(1));
        check_equal(integer(N_GROW, 1, 1, 0), INT64_C(1));
        check_equal(integer(N_LOAD, 1, 65536, 0), INT64_C(0));
        check_equal(integer(N_SIZE, 0, 0, 0), INT64_C(2));
        check_equal(integer(N_GROW, 1, 1, 0), INT64_C(-1));
    }
    it("runs bulk, mixed-width memories, floating-point and SIMD memory operations") {
        turbowasm_value value;
        uint32_t f32_bits;
        uint64_t f64_bits;
        load_fixture(false);
        check_equal(integer(N_FILL, 0, 0, 0), INT64_C(42));
        check_equal(integer(N_COPY, 0, 0, 0), INT64_C(42));
        check_equal(integer(N_MIXED, 0, 0, 0), INT64_C(42));
        check_equal(integer(N_INIT, 0, 0, 0), INT64_C(99));
        check_equal(integer(N_DROP, 0, 0, 0), INT64_C(7));
        invoke(N_INIT, 0, 0, 0, TURBOWASM_TRAPPED, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        value = invoke(N_F32, 0, 0, 0, TURBOWASM_OK, TURBOWASM_TRAP_NONE);
        memcpy(&f32_bits, &value.as.f32, sizeof(f32_bits));
        check_equal(f32_bits, UINT32_C(0x80000000));
        value = invoke(N_F64, 0, 0, 0, TURBOWASM_OK, TURBOWASM_TRAP_NONE);
        memcpy(&f64_bits, &value.as.f64, sizeof(f64_bits));
        check_equal(f64_bits, UINT64_C(0x8000000000000000));
        check_equal(integer(N_SIMD, 0, 0, 0), INT64_C(5));
    }
    it("traps high addresses and offsets without changing low memory") {
        load_fixture(false);
        check_equal(integer(N_STORE, 2, 0, 42), INT64_C(42));
        invoke(N_LOAD, 1, INT64_C(4294967296), 0, TURBOWASM_TRAPPED,
            TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        invoke(N_STORE, 2, INT64_C(4294967296), 99, TURBOWASM_TRAPPED,
            TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        invoke(N_HIGH, 0, 0, 0, TURBOWASM_TRAPPED, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        invoke(N_OVERFLOW, 0, 0, 0, TURBOWASM_TRAPPED, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        invoke(N_SIMD_HIGH, 0, 0, 0, TURBOWASM_TRAPPED, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        invoke(N_FILL_HIGH, 0, 0, 0, TURBOWASM_TRAPPED, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        check_equal(integer(N_LOAD, 1, 0, 0), INT64_C(42));
    }
    it("stops before memory side effects when fuel is exhausted") {
        turbowasm_execution_options options = {0};
        turbowasm_value args[2] = {{0}};
        turbowasm_value result = {0};
        turbowasm_trap trap = TURBOWASM_TRAP_NONE;
        size_t count = 0u;
        load_fixture(false);
        check_equal(integer(N_STORE, 2, 8, 42), INT64_C(42));
        args[0].kind = args[1].kind = TURBOWASM_VALUE_I64;
        args[0].as.i64 = 8;
        args[1].as.i64 = 99;
        options.has_fuel_limit = true;
        options.fuel = 2u;
        check_equal(turbowasm_instance_invoke_with_options(&instance, N_STORE,
            args, 2u, &result, 1u, &count, &trap, &options), TURBOWASM_FUEL_EXHAUSTED);
        check_equal(count, (size_t)0);
        check_equal(integer(N_LOAD, 1, 8, 0), INT64_C(42));
    }
    it("uses shared backing for ordinary, SIMD and atomic accesses") {
        load_fixture(true);
        check_equal(integer(S_FENCE, 0, 0, 0), INT64_C(7));
        check_equal(integer(S_STORE, 2, 0, 41), INT64_C(41));
        check_equal(integer(S_ADD, 2, 0, 1), INT64_C(41));
        check_equal(integer(S_CMPXCHG, 1, 0, 0), INT64_C(42));
        check_equal(integer(S_LOAD, 1, 0, 0), INT64_C(99));
        check_equal(integer(S_NARROW, 1, 8, 0), INT64_C(255));
        check_equal(integer(S_PLAIN, 0, 0, 0), INT64_C(123));
        check_equal(integer(S_BULK, 0, 0, 0), INT64_C(0x2a2a2a2a));
        check_equal(integer(S_SIMD, 0, 0, 0), INT64_C(5));
        check_equal(integer(S_GROW, 1, 1, 0), INT64_C(1));
        check_equal(integer(S_SIZE, 0, 0, 0), INT64_C(2));
        check_equal(integer(S_LOAD, 1, 65536, 0), INT64_C(0));
        check_equal(integer(S_GROW, 1, INT64_C(4294967296), 0), INT64_C(-1));
        check_equal(integer(S_SIZE, 0, 0, 0), INT64_C(2));
    }
    it("interrupts a native wait and stops before an atomic side effect on exhausted fuel") {
        turbowasm_execution_options options = {0};
        turbowasm_value args[2] = {{0}}, result = {0};
        turbowasm_trap trap;
        size_t count;
        unsigned checks = 0u;
        load_fixture(true);
        args[0].kind = args[1].kind = TURBOWASM_VALUE_I64;
        args[0].as.i64 = 8;
        options.should_interrupt = interrupt_wait;
        options.interrupt_context = &checks;
        check_equal(turbowasm_instance_invoke_with_options(&instance, S_WAIT_BLOCKING,
            args, 1u, &result, 1u, &count, &trap, &options), TURBOWASM_INTERRUPTED);
        check_equal(count, (size_t)0);
        check_true(checks >= 5u);
        check_equal(((turbowasm_instance_impl *)instance.impl)->memories[0].waiter_count, 0u);
        options = (turbowasm_execution_options){0};
        options.has_fuel_limit = true;
        options.fuel = 2u;
        args[1].as.i64 = 99;
        check_equal(turbowasm_instance_invoke_with_options(&instance, S_STORE,
            args, 2u, &result, 1u, &count, &trap, &options), TURBOWASM_FUEL_EXHAUSTED);
        check_equal(count, (size_t)0);
        check_equal(integer(S_LOAD, 1, 8, 0), INT64_C(0));
#ifdef TURBOWASM_TEST_MIR
        check_equal(((turbowasm_instance_impl *)instance.impl)->jit_functions[
            S_WAIT_BLOCKING].state, TURBOWASM_JIT_COMPILED);
        check_equal(((turbowasm_instance_impl *)instance.impl)->jit_functions[
            S_STORE].state, TURBOWASM_JIT_COMPILED);
#endif
    }
    it("keeps wait/notify address width, timeout and alignment semantics") {
        uint32_t index;
        load_fixture(true);
        check_equal(integer(S_WAIT32, 1, 0, 0), INT64_C(2));
        check_equal(integer(S_WAIT64, 1, 0, 0), INT64_C(2));
        check_equal(integer(S_NOTIFY, 1, 0, 0), INT64_C(0));
        check_equal(integer(S_STORE, 2, 0, 1), INT64_C(1));
        check_equal(integer(S_WAIT32, 1, 0, 0), INT64_C(1));
        check_equal(integer(S_WAIT64, 1, 0, 0), INT64_C(1));
        for (index = S_WAIT32; index <= S_NOTIFY; ++index) {
            invoke(index, 1, INT64_C(4294967296), 0, TURBOWASM_TRAPPED,
                TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
            invoke(index, 1, 1, 0, TURBOWASM_TRAPPED, TURBOWASM_TRAP_UNALIGNED_ATOMIC);
        }
        invoke(S_LOAD, 1, INT64_C(4294967296), 0, TURBOWASM_TRAPPED,
            TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        invoke(S_HIGH, 0, 0, 0, TURBOWASM_TRAPPED, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        invoke(S_OVERFLOW, 0, 0, 0, TURBOWASM_TRAPPED, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
    }
    it("checks the native helper ABI on platforms without MIR") {
        turbowasm_jit_invocation_context context = {0};
        cmeta_v128 slot;
        uint32_t expected_f32 = UINT32_C(0x7fc01234), actual_f32;
        uint64_t expected_f64 = UINT64_C(0x7ff8000000001234), actual_f64;
        float f32;
        double f64;
        load_fixture(false);
        context.instance = instance.impl;
        context.simd_slots = &slot;
        context.simd_slot_count = 1u;
        turbowasm_jit_memory(&context, 0x37, 0, 0, 0, 0, INT64_MIN, 0);
        check_equal(context.call_status, TURBOWASM_OK);
        check_equal(turbowasm_jit_memory(&context, 0x29, 0, 0, 0, 0, 0, 0), INT64_MIN);
        turbowasm_jit_memory(&context, 0x29, 0, 0, -1, 1, 0, 0);
        check_equal(context.call_status, TURBOWASM_TRAPPED);
        check_equal(context.call_trap, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        check_equal(turbowasm_jit_simd_memory(&context, 0, 0, 0,
            INT64_C(4294967296), 0), (int64_t)TURBOWASM_TRAPPED);
        check_equal(turbowasm_jit_simd_memory(&context, 0, 0, 0, 1, -1),
            (int64_t)TURBOWASM_TRAPPED);
        turbowasm_jit_memory(&context, TURBOWASM_JIT_MEMORY_BULK + 11,
            0, 0, 0, 8, 42, 3);
        check_equal(context.call_status, TURBOWASM_OK);
        check_equal(turbowasm_jit_memory(&context, 0x2d, 0, 0, 0, 10, 0, 0), INT64_C(42));
        check_equal(turbowasm_jit_memory(&context, 0x40, 0, 0, 0,
            INT64_C(4294967296), 0, 0), INT64_C(-1));
        check_equal(context.call_status, TURBOWASM_OK);
        check_equal(turbowasm_jit_memory(&context, 0x3f, 0, 0, 0, 0, 0, 0), INT64_C(1));
        memcpy(&f32, &expected_f32, sizeof(f32));
        memcpy(&f64, &expected_f64, sizeof(f64));
        turbowasm_jit_memory_store_f32(&context, 0, 0, 32, f32);
        check_equal(context.call_status, TURBOWASM_OK);
        f32 = turbowasm_jit_memory_load_f32(&context, 0, 0, 32);
        check_equal(context.call_status, TURBOWASM_OK);
        memcpy(&actual_f32, &f32, sizeof(actual_f32));
        check_equal(actual_f32, expected_f32);
        turbowasm_jit_memory_store_f64(&context, 0, 0, 40, f64);
        check_equal(context.call_status, TURBOWASM_OK);
        f64 = turbowasm_jit_memory_load_f64(&context, 0, 0, 40);
        check_equal(context.call_status, TURBOWASM_OK);
        memcpy(&actual_f64, &f64, sizeof(actual_f64));
        check_equal(actual_f64, expected_f64);
    }
}
