#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "jit_memory_helper.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <string.h>

enum { FIRST_ATOMIC = 0x10, LAST_ATOMIC = 0x4e, MODULE_CAPACITY = 128,
       ATOMIC_ARGUMENTS = 3, INITIAL_BYTES = 8 };

/* Each body fits a single-byte section length; storage is fixed and test-owned. */
static size_t atomic_module(uint8_t *bytes, bool wide, bool shared,
    const turbowasm_atomic_descriptor *descriptor) {
    const uint8_t header[] = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
    uint8_t value = descriptor->value_type == TURBOWASM_ATOMIC_I32 ? 0x7f : 0x7e;
    uint8_t declarations[] = {
        1, 8, 1, 0x60, ATOMIC_ARGUMENTS, 0x7f, 0x7f, 0x7f, 1, 0x7f,
        3, 2, 1, 0,
        5, 4, 1, 1, 1, 1
    };
    size_t size = 0u, section_size, body_size;
    declarations[5] = wide ? 0x7e : 0x7f;
    declarations[6] = declarations[7] = declarations[9] = value;
    declarations[17] = (uint8_t)(1u | (shared ? 2u : 0u) | (wide ? 4u : 0u));
    memcpy(bytes, header, sizeof(header)); size += sizeof(header);
    memcpy(bytes + size, declarations, sizeof(declarations)); size += sizeof(declarations);
    bytes[size++] = 10;
    section_size = size++;
    bytes[size++] = 1;
    body_size = size++;
    bytes[size++] = 0;
    bytes[size++] = 0x20; bytes[size++] = 0;
    if (descriptor->kind != TURBOWASM_ATOMIC_LOAD) {
        bytes[size++] = 0x20; bytes[size++] = 1;
    }
    if (descriptor->kind == TURBOWASM_ATOMIC_CMPXCHG) {
        bytes[size++] = 0x20; bytes[size++] = 2;
    }
    bytes[size++] = 0xfe;
    bytes[size++] = (uint8_t)descriptor->subopcode;
    bytes[size++] = descriptor->alignment_log2;
    bytes[size++] = 0;
    if (descriptor->kind == TURBOWASM_ATOMIC_STORE) {
        bytes[size++] = 0x20; bytes[size++] = 1;
    }
    bytes[size++] = 0x0b;
    bytes[section_size] = (uint8_t)(size - section_size - 1u);
    bytes[body_size] = (uint8_t)(size - body_size - 1u);
    check_true(size <= MODULE_CAPACITY && size < 0x80u);
    return size;
}

static void compare_operation(const turbowasm_atomic_descriptor *descriptor,
    bool wide, bool shared) {
    const uint64_t addresses[] = {0u, 1u, UINT64_C(65536), UINT64_C(4294967296)};
    const uint8_t initial[INITIAL_BYTES] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t bytes[MODULE_CAPACITY], expected_memory[INITIAL_BYTES], actual_memory[INITIAL_BYTES];
    turbowasm_module module = {0};
    turbowasm_instance reference = {0}, target = {0};
    turbowasm_value args[ATOMIC_ARGUMENTS] = {{0}};
    size_t size = atomic_module(bytes, wide, shared, descriptor);
    size_t scenario;
    check_equal(turbowasm_module_load_borrowed(&module, bytes, size), TURBOWASM_OK);
    check_equal(turbowasm_instance_create(&reference, &module), TURBOWASM_OK);
    check_equal(turbowasm_instance_create(&target, &module), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
    {
        turbowasm_jit_backend backend = {0};
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(target.impl, &backend, 1u), TURBOWASM_OK);
    }
#endif
    args[0].kind = wide ? TURBOWASM_VALUE_I64 : TURBOWASM_VALUE_I32;
    args[1].kind = args[2].kind = descriptor->value_type == TURBOWASM_ATOMIC_I32
        ? TURBOWASM_VALUE_I32 : TURBOWASM_VALUE_I64;
    if (args[1].kind == TURBOWASM_VALUE_I32) {
        args[1].as.i32 = -1; args[2].as.i32 = 42;
    } else {
        args[1].as.i64 = -1; args[2].as.i64 = 42;
    }
    for (scenario = 0u; scenario < sizeof(addresses) / sizeof(addresses[0]); ++scenario) {
        turbowasm_value expected = {0}, actual = {0};
        turbowasm_trap expected_trap, actual_trap;
        turbowasm_status expected_status, actual_status;
        turbowasm_jit_invocation_context context = {0};
        size_t expected_count, actual_count;
        int64_t helper_result;
        uint64_t expected_bits;
        if (wide) args[0].as.i64 = (int64_t)addresses[scenario];
        else args[0].as.i32 = (int32_t)(uint32_t)addresses[scenario];
        check_equal(turbowasm_instance_memory_write_bytes(reference.impl, 0u, 0u, 0u,
            initial, sizeof(initial)), TURBOWASM_OK);
        check_equal(turbowasm_instance_memory_write_bytes(target.impl, 0u, 0u, 0u,
            initial, sizeof(initial)), TURBOWASM_OK);
        expected_status = turbowasm_instance_invoke(&reference, 0u, args, ATOMIC_ARGUMENTS,
            &expected, 1u, &expected_count, &expected_trap);
        actual_status = turbowasm_instance_invoke(&target, 0u, args, ATOMIC_ARGUMENTS,
            &actual, 1u, &actual_count, &actual_trap);
        check_equal(actual_status, expected_status);
        check_equal(actual_trap, expected_trap);
        check_equal(actual_count, expected_count);
        expected_bits = expected.kind == TURBOWASM_VALUE_I32
            ? (uint64_t)(uint32_t)expected.as.i32 : (uint64_t)expected.as.i64;
        if (actual_status == TURBOWASM_OK) {
            check_equal(actual.kind, expected.kind);
            check_equal(actual.kind == TURBOWASM_VALUE_I32
                ? (uint64_t)(uint32_t)actual.as.i32 : (uint64_t)actual.as.i64, expected_bits);
        }
        check_equal(turbowasm_instance_memory_read_bytes(reference.impl, 0u, 0u, 0u,
            expected_memory, sizeof(expected_memory)), TURBOWASM_OK);
        check_equal(turbowasm_instance_memory_read_bytes(target.impl, 0u, 0u, 0u,
            actual_memory, sizeof(actual_memory)), TURBOWASM_OK);
        check_equal(memcmp(actual_memory, expected_memory, sizeof(actual_memory)), 0);
#ifdef TURBOWASM_TEST_MIR
        check_equal(((turbowasm_instance_impl *)target.impl)->jit_functions[0].state,
            TURBOWASM_JIT_COMPILED);
#endif
        /* Exercise the same bridge even when this platform cannot build MIR. */
        check_equal(turbowasm_instance_memory_write_bytes(target.impl, 0u, 0u, 0u,
            initial, sizeof(initial)), TURBOWASM_OK);
        context.instance = target.impl;
        helper_result = turbowasm_jit_memory(&context,
            TURBOWASM_JIT_MEMORY_ATOMIC + descriptor->subopcode, 0, 0, 0,
            (int64_t)addresses[scenario], -1, 42);
        check_equal(context.call_status, expected_status);
        check_equal(context.call_trap, expected_trap);
        if (expected_status == TURBOWASM_OK && descriptor->kind != TURBOWASM_ATOMIC_STORE)
            check_equal((uint64_t)helper_result, expected_bits);
        check_equal(turbowasm_instance_memory_read_bytes(target.impl, 0u, 0u, 0u,
            actual_memory, sizeof(actual_memory)), TURBOWASM_OK);
        check_equal(memcmp(actual_memory, expected_memory, sizeof(actual_memory)), 0);
    }
    turbowasm_instance_destroy(&target);
    turbowasm_instance_destroy(&reference);
    turbowasm_module_destroy(&module);
}

spec("atomic lowering") {
    it("preserves every atomic descriptor on shared/unshared memory32 and memory64") {
        uint32_t opcode, mode;
        for (opcode = FIRST_ATOMIC; opcode <= LAST_ATOMIC; ++opcode) {
            const turbowasm_atomic_descriptor *descriptor = turbowasm_atomic_descriptor_find(opcode);
            check_not_null(descriptor);
            for (mode = 0u; mode < 4u; ++mode)
                compare_operation(descriptor, (mode & 1u) != 0u, (mode & 2u) != 0u);
        }
    }
}
