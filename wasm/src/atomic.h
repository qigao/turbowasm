#ifndef TURBOWASM_ATOMIC_H
#define TURBOWASM_ATOMIC_H

#include <stdint.h>

typedef enum turbowasm_atomic_kind {
    TURBOWASM_ATOMIC_LOAD = 0,
    TURBOWASM_ATOMIC_STORE,
    TURBOWASM_ATOMIC_RMW,
    TURBOWASM_ATOMIC_CMPXCHG
} turbowasm_atomic_kind;

typedef enum turbowasm_atomic_op {
    TURBOWASM_ATOMIC_OP_NONE = 0,
    TURBOWASM_ATOMIC_OP_ADD,
    TURBOWASM_ATOMIC_OP_SUB,
    TURBOWASM_ATOMIC_OP_AND,
    TURBOWASM_ATOMIC_OP_OR,
    TURBOWASM_ATOMIC_OP_XOR,
    TURBOWASM_ATOMIC_OP_XCHG
} turbowasm_atomic_op;

typedef enum turbowasm_atomic_value_type {
    TURBOWASM_ATOMIC_I32 = 0,
    TURBOWASM_ATOMIC_I64
} turbowasm_atomic_value_type;

typedef struct turbowasm_atomic_descriptor {
    uint32_t subopcode;
    turbowasm_atomic_kind kind;
    turbowasm_atomic_op op;
    turbowasm_atomic_value_type value_type;
    uint8_t width;
    uint8_t alignment_log2;
} turbowasm_atomic_descriptor;

const turbowasm_atomic_descriptor *
turbowasm_atomic_descriptor_find(uint32_t subopcode);

#endif /* TURBOWASM_ATOMIC_H */
