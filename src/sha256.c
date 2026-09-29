#include "sha256.h"

#include <stdint.h>
#include <string.h>

typedef struct turbowasm_sha256_state {
    uint32_t h[8];
    uint64_t total;
    uint8_t block[64];
    size_t used;
} turbowasm_sha256_state;

static uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24u) |
           ((uint32_t)p[1] << 16u) |
           ((uint32_t)p[2] << 8u) |
           (uint32_t)p[3];
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24u);
    p[1] = (uint8_t)(v >> 16u);
    p[2] = (uint8_t)(v >> 8u);
    p[3] = (uint8_t)v;
}

static const uint32_t k[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491),
    UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
    UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
    UINT32_C(0xd807aa98), UINT32_C(0x12835b01),
    UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe),
    UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
    UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
    UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa),
    UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d),
    UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
    UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967),
    UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138),
    UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb),
    UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
    UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
    UINT32_C(0xd192e819), UINT32_C(0xd6990624),
    UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08),
    UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
    UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
    UINT32_C(0x748f82ee), UINT32_C(0x78a5636f),
    UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb),
    UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2)
};

static void transform(
    turbowasm_sha256_state *state,
    const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a,b,c,d,e,f,g,h;
    uint32_t i;

    for (i = 0u; i < 16u; ++i)
        w[i] = read_be32(block + i * 4u);
    for (; i < 64u; ++i) {
        uint32_t s0 =
            rotr32(w[i - 15u], 7u) ^
            rotr32(w[i - 15u], 18u) ^
            (w[i - 15u] >> 3u);
        uint32_t s1 =
            rotr32(w[i - 2u], 17u) ^
            rotr32(w[i - 2u], 19u) ^
            (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a=state->h[0]; b=state->h[1]; c=state->h[2]; d=state->h[3];
    e=state->h[4]; f=state->h[5]; g=state->h[6]; h=state->h[7];

    for (i = 0u; i < 64u; ++i) {
        uint32_t s1 = rotr32(e,6u)^rotr32(e,11u)^rotr32(e,25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a,2u)^rotr32(a,13u)^rotr32(a,22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }

    state->h[0]+=a; state->h[1]+=b; state->h[2]+=c; state->h[3]+=d;
    state->h[4]+=e; state->h[5]+=f; state->h[6]+=g; state->h[7]+=h;
}

static void update(
    turbowasm_sha256_state *state,
    const uint8_t *bytes,
    size_t size) {
    while (size != 0u) {
        size_t take = 64u - state->used;
        if (take > size)
            take = size;
        memcpy(state->block + state->used, bytes, take);
        state->used += take;
        state->total += take;
        bytes += take;
        size -= take;
        if (state->used == 64u) {
            transform(state, state->block);
            state->used = 0u;
        }
    }
}

void turbowasm_sha256(
    const uint8_t *bytes,
    size_t size,
    uint8_t out[TURBOWASM_SHA256_DIGEST_SIZE]) {
    turbowasm_sha256_state state = {
        {
            UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
            UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
            UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
            UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
        },
        0u, {0}, 0u
    };
    uint64_t bits;
    uint32_t i;

    if (out == NULL || (size != 0u && bytes == NULL))
        return;

    update(&state, bytes, size);
    bits = state.total * UINT64_C(8);

    state.block[state.used++] = 0x80u;
    if (state.used > 56u) {
        memset(state.block + state.used, 0, 64u - state.used);
        transform(&state, state.block);
        state.used = 0u;
    }
    memset(state.block + state.used, 0, 56u - state.used);
    for (i = 0u; i < 8u; ++i)
        state.block[63u - i] = (uint8_t)(bits >> (8u * i));
    transform(&state, state.block);

    for (i = 0u; i < 8u; ++i)
        write_be32(out + i * 4u, state.h[i]);
}
