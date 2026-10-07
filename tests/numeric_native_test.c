#include <turbowasm/turbowasm.h>
#include "instance_internal.h"
#ifdef TURBOWASM_TEST_MIR
#include "jit/mir_backend.h"
#endif
#include <tinytest.h>
#include <math.h>
#include <string.h>

enum { MODULE_CAPACITY = 96, SCALAR_FIRST = 0x45, SCALAR_LAST = 0xc4, SAT_COUNT = 8 };
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

static void create(fixture *f, uint32_t opcode, const turbowasm_numeric_signature *signature,
    bool constant, uint64_t bits) {
    static const uint8_t header[] = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
    size_t size = sizeof(header), section, body;
    uint32_t i, count = constant ? 0u : signature->input_count;
    memset(f, 0, sizeof(*f)); memcpy(f->bytes, header, sizeof(header));
    f->bytes[size++] = 1; f->bytes[size++] = (uint8_t)(count + 5u);
    f->bytes[size++] = 1; f->bytes[size++] = 0x60; f->bytes[size++] = (uint8_t)count;
    for (i = 0; i < count; ++i) f->bytes[size++] = signature->input_type;
    f->bytes[size++] = 1; f->bytes[size++] = signature->output_type;
    f->bytes[size++] = 3; f->bytes[size++] = 2; f->bytes[size++] = 1; f->bytes[size++] = 0;
    f->bytes[size++] = 10; section = size++; f->bytes[size++] = 1; body = size++;
    f->bytes[size++] = 0;
    for (i = 0; i < count; ++i) { f->bytes[size++] = 0x20; f->bytes[size++] = (uint8_t)i; }
    if (constant) {
        uint32_t width = signature->output_type == 0x7du ? 4u : 8u;
        f->bytes[size++] = width == 4u ? 0x43 : 0x44;
        for (i = 0; i < width; ++i) f->bytes[size++] = (uint8_t)(bits >> (i * 8u));
    } else if (opcode >= TURBOWASM_JIT_NUMERIC_SAT_BASE) {
        f->bytes[size++] = 0xfc;
        f->bytes[size++] = (uint8_t)(opcode - TURBOWASM_JIT_NUMERIC_SAT_BASE);
    } else f->bytes[size++] = (uint8_t)opcode;
    f->bytes[size++] = 0x0b;
    f->bytes[body] = (uint8_t)(size - body - 1u);
    f->bytes[section] = (uint8_t)(size - section - 1u);
    check_true(size <= MODULE_CAPACITY);
    check(turbowasm_module_load_borrowed(&f->module, f->bytes, size) == TURBOWASM_OK,
        "numeric signature must independently validate: opcode=%x", opcode);
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
static turbowasm_value value(uint8_t type, uint64_t bits) {
    turbowasm_value result = {0};
    uint32_t low = (uint32_t)bits;
    switch (type) {
        case 0x7f: result.kind = TURBOWASM_VALUE_I32; memcpy(&result.as.i32, &low, sizeof(low)); break;
        case 0x7e: result.kind = TURBOWASM_VALUE_I64; memcpy(&result.as.i64, &bits, sizeof(bits)); break;
        case 0x7d: result.kind = TURBOWASM_VALUE_F32; memcpy(&result.as.f32, &low, sizeof(low)); break;
        case 0x7c: result.kind = TURBOWASM_VALUE_F64; memcpy(&result.as.f64, &bits, sizeof(bits)); break;
        default: check(false, "invalid numeric type");
    }
    return result;
}
static void equal_value(turbowasm_value a, turbowasm_value b, bool exact) {
    size_t width = sizeof(uint64_t);
    check_equal(a.kind, b.kind);
    if (!exact && a.kind == TURBOWASM_VALUE_F32 && isnan(a.as.f32)) {
        check_true(isnan(b.as.f32)); return;
    }
    if (!exact && a.kind == TURBOWASM_VALUE_F64 && isnan(a.as.f64)) {
        check_true(isnan(b.as.f64)); return;
    }
    if (a.kind == TURBOWASM_VALUE_I32 || a.kind == TURBOWASM_VALUE_F32) width = sizeof(uint32_t);
    check_equal(memcmp(&a.as, &b.as, width), 0);
}
static outcome invoke(turbowasm_instance *instance, const turbowasm_value *args,
    size_t count, const turbowasm_execution_options *options) {
    outcome result = {0};
    result.status = turbowasm_instance_invoke_with_options(instance, 0, args, count,
        &result.value, 1, &result.count, &result.trap, options);
    return result;
}
static outcome compare(fixture *f, const turbowasm_value *args, size_t count,
    const turbowasm_execution_options *options, bool exact) {
    outcome expected = invoke(&f->reference, args, count, options);
    outcome actual = invoke(&f->native, args, count, options);
    check_equal(actual.status, expected.status); check_equal(actual.trap, expected.trap);
    check_equal(actual.count, expected.count);
    if (actual.count != 0) equal_value(actual.value, expected.value, exact);
#ifdef TURBOWASM_TEST_MIR
    check_equal(((turbowasm_instance_impl *)f->native.impl)->jit_functions[0].state, TURBOWASM_JIT_COMPILED);
#endif
    return actual;
}

static const uint64_t integer_bits[] = {
    0, 1, 2, 31, 32, 63, 64, 65, UINT64_MAX, UINT64_MAX - 1u,
    UINT64_C(0x7fffffff), UINT64_C(0x80000000), UINT64_C(0xffffffff),
    UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000),
    UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)
};
static const uint64_t float32_bits[] = {
    0, UINT64_C(0x80000000), UINT64_C(0x3f800000), UINT64_C(0xbf800000),
    UINT64_C(0x3f000000), UINT64_C(0x3fc00000), UINT64_C(0x40200000), UINT64_C(0xbfc00000),
    UINT64_C(0x7f800000), UINT64_C(0xff800000), UINT64_C(0x7fc01234), UINT64_C(0xffc05678),
    UINT64_C(0x7f801234), 1, UINT64_C(0x007fffff), UINT64_C(0x00800000),
    UINT64_C(0x4effffff), UINT64_C(0x4f000000), UINT64_C(0xcf000000),
    UINT64_C(0x4f800000), UINT64_C(0x5f000000), UINT64_C(0x5f800000), UINT64_C(0xdf000000)
};
static const uint64_t float64_bits[] = {
    0, UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000),
    UINT64_C(0x3fe0000000000000), UINT64_C(0x3ff8000000000000), UINT64_C(0x4004000000000000),
    UINT64_C(0xbff8000000000000), UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
    UINT64_C(0x7ff8000000001234), UINT64_C(0xfff8000000005678), UINT64_C(0x7ff0000000001234),
    1, UINT64_C(0x000fffffffffffff), UINT64_C(0x0010000000000000),
    UINT64_C(0x41dfffffffffffff), UINT64_C(0x41e0000000000000), UINT64_C(0xc1e0000000000000),
    UINT64_C(0x41f0000000000000), UINT64_C(0x43e0000000000000), UINT64_C(0x43f0000000000000),
    UINT64_C(0xc3e0000000000000)
};

spec("native scalar numeric operations") {
    it("executes every scalar opcode and saturation with integer and floating boundaries") {
        uint32_t opcode;
        for (opcode = SCALAR_FIRST; opcode < TURBOWASM_JIT_NUMERIC_SAT_BASE + SAT_COUNT; ++opcode) {
            turbowasm_numeric_signature signature;
            fixture f;
            const uint64_t *bits = integer_bits;
            size_t count = sizeof(integer_bits) / sizeof(*integer_bits), i, j;
            turbowasm_execution_options options = {0};
            turbowasm_value args[2];
            if (opcode == SCALAR_LAST + 1) opcode = TURBOWASM_JIT_NUMERIC_SAT_BASE;
            check_true(turbowasm_numeric_signature_get(opcode, &signature));
            create(&f, opcode, &signature, false, 0);
            if (signature.input_type == 0x7d) { bits = float32_bits; count = sizeof(float32_bits) / sizeof(*float32_bits); }
            if (signature.input_type == 0x7c) { bits = float64_bits; count = sizeof(float64_bits) / sizeof(*float64_bits); }
            for (i = 0; i < count; ++i) {
                args[0] = value(signature.input_type, bits[i]);
                for (j = 0; j < (signature.input_count == 2 ? count : 1u); ++j) {
                    args[1] = value(signature.input_type, bits[j]);
                    compare(&f, args, signature.input_count, NULL,
                        opcode == 0x8b || opcode == 0x8c || opcode == 0x98 ||
                        opcode == 0x99 || opcode == 0x9a || opcode == 0xa6 || opcode >= 0xbc);
                }
            }
            options.has_fuel_limit = true;
            for (i = 0; i <= signature.input_count + 2u; ++i) {
                options.fuel = i;
                compare(&f, args, signature.input_count, &options, false);
            }
            destroy(&f);
        }
    }
    it("preserves every bit of floating constants including signaling NaNs and negative zero") {
        unsigned wide;
        for (wide = 0; wide < 2; ++wide) {
            const uint64_t *bits = wide ? float64_bits : float32_bits;
            size_t count = wide ? sizeof(float64_bits) / sizeof(*float64_bits) : sizeof(float32_bits) / sizeof(*float32_bits), i;
            turbowasm_numeric_signature signature = {0, wide ? 0x7c : 0x7d, 0};
            for (i = 0; i < count; ++i) {
                fixture f;
                outcome result;
                create(&f, 0, &signature, true, bits[i]);
                result = compare(&f, NULL, 0, NULL, true);
                check_equal(result.status, TURBOWASM_OK);
                equal_value(result.value, value(signature.output_type, bits[i]), true);
                destroy(&f);
            }
        }
    }
    it("does not publish an output on invalid inputs or a numeric trap") {
        turbowasm_jit_invocation_context context = {0};
        turbowasm_value args[] = {value(0x7f, 1), value(0x7f, 0)};
        turbowasm_value result = value(0x7f, 73), before = result;
        check_equal(turbowasm_jit_numeric(&context, 0x6d, args, &result), TURBOWASM_TRAPPED);
        check_equal(context.call_trap, TURBOWASM_TRAP_INTEGER_DIVIDE_BY_ZERO);
        equal_value(result, before, true);
        args[0].kind = TURBOWASM_VALUE_F64;
        check_equal(turbowasm_jit_numeric(&context, 0x67, args, &result), TURBOWASM_TYPE_MISMATCH);
        equal_value(result, before, true);
        check_equal(turbowasm_jit_numeric(&context, -1, args, &result), TURBOWASM_UNSUPPORTED);
        check_equal(turbowasm_jit_numeric(&context, 0x67, NULL, &result), TURBOWASM_INVALID_ARGUMENT);
    }
}
