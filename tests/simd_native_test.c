#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#include "simd_exec_table.h"
#include "relaxed_simd.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <string.h>

enum { MODULE_CAPACITY = 128, PAGE_SIZE = 65536, SAMPLE_BYTES = 64, MEMORY_INDEX = 1 };
typedef struct signature {
    uint8_t inputs[3], count, result;
    bool memory, lane;
} signature;
typedef struct fixture {
    uint8_t bytes[MODULE_CAPACITY];
    turbowasm_module module;
    turbowasm_instance native, reference;
} fixture;
typedef struct outcome {
    turbowasm_status status;
    turbowasm_trap trap;
    size_t count;
    turbowasm_value value;
} outcome;

static uint8_t scalar_type(const turbowasm_simd_exec_descriptor *d) {
    switch (d->vector_desc->lane_kind) {
        case CMETA_VECTOR_I64: case CMETA_VECTOR_U64: return 0x7e;
        case CMETA_VECTOR_F32: return 0x7d;
        case CMETA_VECTOR_F64: return 0x7c;
        default: return 0x7f;
    }
}
static signature signature_for(const turbowasm_simd_exec_descriptor *d, bool wide) {
    signature s = {{0x7b, 0x7b, 0x7b}, 2, 0x7b, false, false};
    switch (d->kind) {
        case TURBOWASM_SIMD_EXEC_SPLAT: s.count = 1; s.inputs[0] = scalar_type(d); break;
        case TURBOWASM_SIMD_EXEC_UNARY: case TURBOWASM_SIMD_EXEC_EXTEND_HALF:
        case TURBOWASM_SIMD_EXEC_EXTADD_PAIRWISE: case TURBOWASM_SIMD_EXEC_CONVERT:
            s.count = 1; break;
        case TURBOWASM_SIMD_EXEC_REDUCE: s.count = 1; s.result = 0x7f; break;
        case TURBOWASM_SIMD_EXEC_SHIFT: s.inputs[1] = 0x7f; break;
        case TURBOWASM_SIMD_EXEC_SELECT: s.count = 3; break;
        case TURBOWASM_SIMD_EXEC_RELAXED: s.count = turbowasm_relaxed_simd_arity(d->opcode); break;
        case TURBOWASM_SIMD_EXEC_LANE_EXTRACT:
            s.count = 1; s.result = scalar_type(d); s.lane = true; break;
        case TURBOWASM_SIMD_EXEC_LANE_REPLACE:
            s.inputs[1] = scalar_type(d); s.lane = true; break;
        case TURBOWASM_SIMD_EXEC_MEMORY_EXTEND: case TURBOWASM_SIMD_EXEC_MEMORY_SPLAT:
        case TURBOWASM_SIMD_EXEC_MEMORY_ZERO: case TURBOWASM_SIMD_EXEC_MEMORY_LOAD_LANE:
        case TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE:
            s.memory = true; s.inputs[0] = wide ? 0x7e : 0x7f;
            s.lane = d->kind == TURBOWASM_SIMD_EXEC_MEMORY_LOAD_LANE || d->kind == TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE;
            s.count = s.lane ? 2 : 1;
            if (d->kind == TURBOWASM_SIMD_EXEC_MEMORY_STORE_LANE) s.result = 0;
            break;
        default: break;
    }
    return s;
}
static void uleb(uint8_t *bytes, size_t *size, uint64_t value) {
    do {
        uint8_t byte = (uint8_t)(value & 0x7fu);
        value >>= 7u;
        bytes[(*size)++] = value ? byte | 0x80u : byte;
    } while (value);
}
static void create(fixture *f, const turbowasm_simd_exec_descriptor *d,
    signature s, unsigned mode, unsigned lane, uint64_t offset) {
    static const uint8_t header[] = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
    size_t size = sizeof(header), section, body;
    unsigned i;
    memset(f, 0, sizeof(*f)); memcpy(f->bytes, header, sizeof(header));
    f->bytes[size++] = 1; f->bytes[size++] = (uint8_t)(s.count + 4u + (s.result != 0));
    f->bytes[size++] = 1; f->bytes[size++] = 0x60; f->bytes[size++] = s.count;
    for (i = 0; i < s.count; ++i) f->bytes[size++] = s.inputs[i];
    f->bytes[size++] = s.result != 0;
    if (s.result) f->bytes[size++] = s.result;
    f->bytes[size++] = 3; f->bytes[size++] = 2; f->bytes[size++] = 1; f->bytes[size++] = 0;
    if (s.memory) {
        f->bytes[size++] = 5; f->bytes[size++] = 7; f->bytes[size++] = 2;
        for (i = 0; i < 2; ++i) {
            f->bytes[size++] = (uint8_t)(1u | ((mode & 1u) ? 4u : 0u) | ((mode & 2u) ? 2u : 0u));
            f->bytes[size++] = 1; f->bytes[size++] = 1;
        }
    }
    f->bytes[size++] = 10; section = size++; f->bytes[size++] = 1; body = size++;
    f->bytes[size++] = 0;
    for (i = 0; i < s.count; ++i) { f->bytes[size++] = 0x20; f->bytes[size++] = (uint8_t)i; }
    f->bytes[size++] = 0xfd; uleb(f->bytes, &size, d->opcode);
    if (s.memory) {
        f->bytes[size++] = 0x40; f->bytes[size++] = MEMORY_INDEX;
        uleb(f->bytes, &size, offset);
    }
    if (s.lane) f->bytes[size++] = (uint8_t)lane;
    if (d->kind == TURBOWASM_SIMD_EXEC_SHUFFLE)
        for (i = 0; i < 16; ++i) f->bytes[size++] = (uint8_t)(lane ? 31u - i : (i * 7u) % 32u);
    f->bytes[size++] = 0x0b;
    f->bytes[body] = (uint8_t)(size - body - 1u);
    f->bytes[section] = (uint8_t)(size - section - 1u);
    check_true(size <= MODULE_CAPACITY);
    check(turbowasm_module_load_borrowed(&f->module, f->bytes, size) == TURBOWASM_OK,
        "SIMD fixture must validate: opcode=%x mode=%u lane=%u", d->opcode, mode, lane);
    check_equal(turbowasm_instance_create(&f->native, &f->module), TURBOWASM_OK);
    check_equal(turbowasm_instance_create(&f->reference, &f->module), TURBOWASM_OK);
#ifdef TURBOWASM_TEST_MIR
    {
        turbowasm_jit_backend backend = {0};
        check_equal(turbowasm_mir_backend_create(&backend), TURBOWASM_OK);
        check_equal(turbowasm_jit_instance_attach_backend(f->native.impl, &backend, 1u), TURBOWASM_OK);
    }
#endif
}
static void destroy(fixture *f) {
    turbowasm_instance_destroy(&f->native); turbowasm_instance_destroy(&f->reference);
    turbowasm_module_destroy(&f->module);
}
static turbowasm_value value(uint8_t type, unsigned seed) {
    const uint32_t patterns[] = {UINT32_C(0x80808080), UINT32_C(0x7fc01234),
        UINT32_C(0x80000000), UINT32_C(0x3fc00000)};
    uint32_t bits[4];
    uint64_t wide = UINT64_C(0xfff8000000001234);
    turbowasm_value result = {0};
    unsigned i;
    for (i = 0; i < 4; ++i) bits[i] = patterns[(seed + i) % 4];
    switch (type) {
        case 0x7f: result.kind = TURBOWASM_VALUE_I32; result.as.i32 = seed ? -1 : 33; break;
        case 0x7e: result.kind = TURBOWASM_VALUE_I64; result.as.i64 = INT64_MIN; break;
        case 0x7d: result.kind = TURBOWASM_VALUE_F32; memcpy(&result.as.f32, &bits[0], sizeof(uint32_t)); break;
        case 0x7c: result.kind = TURBOWASM_VALUE_F64; memcpy(&result.as.f64, &wide, sizeof(wide)); break;
        case 0x7b:
            result.kind = TURBOWASM_VALUE_V128; result.as.v128.shape = TURBOWASM_V128_RAW;
            cmeta_simd_v128_load(&result.as.v128.bits, bits); break;
        default: check(false, "invalid SIMD input");
    }
    return result;
}
static void equal_value(turbowasm_value actual, turbowasm_value expected) {
    size_t size;
    check_equal(actual.kind, expected.kind);
    if (actual.kind == TURBOWASM_VALUE_V128) {
        check_equal(actual.as.v128.shape, expected.as.v128.shape);
        check_equal(memcmp(&actual.as.v128.bits, &expected.as.v128.bits, sizeof(actual.as.v128.bits)), 0);
    } else {
        size = actual.kind == TURBOWASM_VALUE_I32 || actual.kind == TURBOWASM_VALUE_F32 ? 4u : 8u;
        check_equal(memcmp(&actual.as, &expected.as, size), 0);
    }
}
static outcome invoke(turbowasm_instance *instance, const turbowasm_value *args,
    size_t count, const turbowasm_execution_options *options) {
    outcome result = {0};
    result.status = turbowasm_instance_invoke_with_options(instance, 0, args, count,
        &result.value, 1, &result.count, &result.trap, options);
    return result;
}
static outcome compare(fixture *f, const turbowasm_value *args, signature s,
    const turbowasm_execution_options *options, uint32_t opcode) {
    outcome expected = invoke(&f->reference, args, s.count, options);
    outcome actual = invoke(&f->native, args, s.count, options);
    check_equal(actual.status, expected.status); check_equal(actual.trap, expected.trap);
    check_equal(actual.count, expected.count);
    if (actual.count) equal_value(actual.value, expected.value);
#ifdef TURBOWASM_TEST_MIR
    check(((turbowasm_instance_impl *)f->native.impl)->jit_functions[0].state == TURBOWASM_JIT_COMPILED,
        "SIMD opcode %x must compile", opcode);
#else
    (void)opcode;
#endif
    return actual;
}
static void memory_pattern(fixture *f) {
    uint8_t bytes[SAMPLE_BYTES];
    size_t i;
    for (i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(i * 7u + 0x80u);
    check_equal(turbowasm_instance_memory_write_bytes(f->native.impl, MEMORY_INDEX, 0, 0, bytes, sizeof(bytes)), TURBOWASM_OK);
    check_equal(turbowasm_instance_memory_write_bytes(f->reference.impl, MEMORY_INDEX, 0, 0, bytes, sizeof(bytes)), TURBOWASM_OK);
}
static void memory_equal(fixture *f) {
    uint8_t a[SAMPLE_BYTES], b[SAMPLE_BYTES];
    uint64_t offsets[] = {0, PAGE_SIZE - SAMPLE_BYTES};
    unsigned memory, position;
    for (memory = 0; memory <= MEMORY_INDEX; ++memory)
        for (position = 0; position < 2; ++position) {
            check_equal(turbowasm_instance_memory_read_bytes(f->native.impl, memory, offsets[position], 0, a, sizeof(a)), TURBOWASM_OK);
            check_equal(turbowasm_instance_memory_read_bytes(f->reference.impl, memory, offsets[position], 0, b, sizeof(b)), TURBOWASM_OK);
            check_equal(memcmp(a, b, sizeof(a)), 0);
        }
}

spec("native SIMD descriptor coverage") {
    it("executes every descriptor, lane and memory mode with matching bits, shape and fuel") {
        size_t index;
        for (index = 0; index < turbowasm_simd_exec_descriptor_count(); ++index) {
            const turbowasm_simd_exec_descriptor *d = turbowasm_simd_exec_descriptor_at(index);
            signature base = signature_for(d, false);
            unsigned mode, lane, lanes = base.lane ? d->vector_desc->lane_count : d->kind == TURBOWASM_SIMD_EXEC_SHUFFLE ? 2u : 1u;
            for (mode = 0; mode < (base.memory ? 4u : 1u); ++mode)
                for (lane = 0; lane < lanes; ++lane) {
                    fixture f;
                    signature s = signature_for(d, (mode & 1u) != 0);
                    turbowasm_value args[3];
                    turbowasm_execution_options options = {0};
                    unsigned i, pattern;
                    create(&f, d, s, mode, lane, 3);
                    if (s.memory) memory_pattern(&f);
                    for (pattern = 0; pattern < 4; ++pattern) {
                        for (i = 0; i < s.count; ++i) args[i] = value(s.inputs[i], i + pattern);
                        if (s.memory) {
                            if (s.inputs[0] == 0x7e) args[0].as.i64 = pattern;
                            else args[0].as.i32 = pattern;
                        }
                        check_equal(compare(&f, args, s, NULL, d->opcode).status, TURBOWASM_OK);
                        if (s.memory) memory_equal(&f);
                    }
                    options.has_fuel_limit = true;
                    for (i = 0; i <= s.count + 2u; ++i) {
                        options.fuel = i;
                        compare(&f, args, s, &options, d->opcode);
                        if (s.memory) memory_equal(&f);
                    }
                    if (s.memory) {
                        const int64_t addresses[] = {PAGE_SIZE - 4, PAGE_SIZE, INT64_MAX, -1};
                        for (i = 0; i < sizeof(addresses) / sizeof(*addresses); ++i) {
                            if (s.inputs[0] == 0x7e) args[0].as.i64 = addresses[i];
                            else args[0].as.i32 = (int32_t)addresses[i];
                            compare(&f, args, s, NULL, d->opcode);
                            memory_equal(&f);
                        }
                    }
                    destroy(&f);
                }
        }
    }
    it("rejects overflowed memory64 offsets and addresses before a lane store") {
        const turbowasm_simd_exec_descriptor *d = turbowasm_simd_exec_descriptor_find(0x58);
        unsigned mode;
        for (mode = 1; mode <= 3; mode += 2) {
            fixture f;
            signature s = signature_for(d, true);
            turbowasm_value args[] = {value(0x7e, 0), value(0x7b, 0)};
            outcome result;
            create(&f, d, s, mode, 0, UINT64_MAX);
            memory_pattern(&f); args[0].as.i64 = 1;
            result = compare(&f, args, s, NULL, d->opcode);
            check_equal(result.status, TURBOWASM_TRAPPED);
            check_equal(result.trap, TURBOWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
            memory_equal(&f); destroy(&f);
        }
    }
}
