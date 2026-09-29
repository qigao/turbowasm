#ifndef TURBOWASM_SHA256_H
#define TURBOWASM_SHA256_H

#include <stddef.h>
#include <stdint.h>

enum { TURBOWASM_SHA256_DIGEST_SIZE = 32 };

void turbowasm_sha256(
    const uint8_t *bytes,
    size_t size,
    uint8_t out[TURBOWASM_SHA256_DIGEST_SIZE]);

#endif
