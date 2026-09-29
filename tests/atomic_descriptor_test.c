#include "atomic.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static void test_complete_descriptor_range(void) {
    uint32_t opcode;

    assert(turbowasm_atomic_descriptor_find(0x0fu) == NULL);
    assert(turbowasm_atomic_descriptor_find(0x4fu) == NULL);

    for (opcode = 0x10u; opcode <= 0x4eu; ++opcode) {
        const turbowasm_atomic_descriptor *descriptor =
            turbowasm_atomic_descriptor_find(opcode);
        assert(descriptor != NULL);
        assert(descriptor->subopcode == opcode);
        assert(descriptor->width == 1u ||
               descriptor->width == 2u ||
               descriptor->width == 4u ||
               descriptor->width == 8u);
        assert(descriptor->alignment_log2 ==
               (descriptor->width == 1u ? 0u :
                descriptor->width == 2u ? 1u :
                descriptor->width == 4u ? 2u : 3u));
        assert(descriptor->value_type == TURBOWASM_ATOMIC_I32 ||
               descriptor->value_type == TURBOWASM_ATOMIC_I64);
    }
}

static void test_family_boundaries(void) {
    const turbowasm_atomic_descriptor *d;

    d = turbowasm_atomic_descriptor_find(0x10u);
    assert(d->kind == TURBOWASM_ATOMIC_LOAD);
    assert(d->value_type == TURBOWASM_ATOMIC_I32);
    assert(d->width == 4u);

    d = turbowasm_atomic_descriptor_find(0x16u);
    assert(d->kind == TURBOWASM_ATOMIC_LOAD);
    assert(d->value_type == TURBOWASM_ATOMIC_I64);
    assert(d->width == 4u);

    d = turbowasm_atomic_descriptor_find(0x17u);
    assert(d->kind == TURBOWASM_ATOMIC_STORE);

    d = turbowasm_atomic_descriptor_find(0x1eu);
    assert(d->kind == TURBOWASM_ATOMIC_RMW);
    assert(d->op == TURBOWASM_ATOMIC_OP_ADD);

    d = turbowasm_atomic_descriptor_find(0x25u);
    assert(d->op == TURBOWASM_ATOMIC_OP_SUB);
    d = turbowasm_atomic_descriptor_find(0x2cu);
    assert(d->op == TURBOWASM_ATOMIC_OP_AND);
    d = turbowasm_atomic_descriptor_find(0x33u);
    assert(d->op == TURBOWASM_ATOMIC_OP_OR);
    d = turbowasm_atomic_descriptor_find(0x3au);
    assert(d->op == TURBOWASM_ATOMIC_OP_XOR);
    d = turbowasm_atomic_descriptor_find(0x41u);
    assert(d->op == TURBOWASM_ATOMIC_OP_XCHG);

    d = turbowasm_atomic_descriptor_find(0x48u);
    assert(d->kind == TURBOWASM_ATOMIC_CMPXCHG);
    d = turbowasm_atomic_descriptor_find(0x4eu);
    assert(d->kind == TURBOWASM_ATOMIC_CMPXCHG);
    assert(d->value_type == TURBOWASM_ATOMIC_I64);
    assert(d->width == 4u);
}

int main(void) {
    test_complete_descriptor_range();
    test_family_boundaries();
    return 0;
}
