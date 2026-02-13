/*
 * sh_hash_sha256.c - SHA-256 implementation (FIPS 180-4)
 *
 * Standalone implementation with no external dependencies.
 * Reference: https://csrc.nist.gov/publications/detail/fips/180/4/final
 */

#include "sh_hash_sha256.h"
#include <string.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/* ============================================================================
 * Helper Macros
 * ============================================================================ */

#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x,y,z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)       (ROTR(x, 2)  ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)       (ROTR(x, 6)  ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x)      (ROTR(x, 7)  ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x)      (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

/* Big-endian load/store */
static uint32_t be32_load(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void be32_store(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void be64_store(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

/* ============================================================================
 * Block Processing
 * ============================================================================ */

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    /* Prepare message schedule */
    for (i = 0; i < 16; i++)
        W[i] = be32_load(block + 4 * i);
    for (i = 16; i < 64; i++)
        W[i] = SIG1(W[i-2]) + W[i-7] + SIG0(W[i-15]) + W[i-16];

    /* Initialize working variables */
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    /* 64 rounds */
    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e,f,g) + K[i] + W[i];
        uint32_t t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    /* Update state */
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void sh_sha256_init(ShSha256 *ctx)
{
    if (!ctx) return;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
    memset(ctx->buffer, 0, 64);
}

void sh_sha256_update(ShSha256 *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t buf_used;

    if (!ctx || !data || len == 0) return;

    buf_used = (size_t)(ctx->count & 63);
    ctx->count += len;

    /* If we have buffered data, try to complete a block */
    if (buf_used > 0) {
        size_t fill = 64 - buf_used;
        if (len < fill) {
            memcpy(ctx->buffer + buf_used, p, len);
            return;
        }
        memcpy(ctx->buffer + buf_used, p, fill);
        sha256_transform(ctx->state, ctx->buffer);
        p += fill;
        len -= fill;
    }

    /* Process full blocks */
    while (len >= 64) {
        sha256_transform(ctx->state, p);
        p += 64;
        len -= 64;
    }

    /* Buffer remainder */
    if (len > 0)
        memcpy(ctx->buffer, p, len);
}

void sh_sha256_final(ShSha256 *ctx, uint8_t digest[32])
{
    size_t buf_used;
    uint64_t bit_count;
    int i;

    if (!ctx || !digest) return;

    buf_used = (size_t)(ctx->count & 63);
    bit_count = ctx->count * 8;

    /* Pad with 0x80 */
    ctx->buffer[buf_used++] = 0x80;

    /* If not enough room for length, pad and process */
    if (buf_used > 56) {
        memset(ctx->buffer + buf_used, 0, 64 - buf_used);
        sha256_transform(ctx->state, ctx->buffer);
        buf_used = 0;
    }

    /* Pad to 56 bytes and append 64-bit big-endian length */
    memset(ctx->buffer + buf_used, 0, 56 - buf_used);
    be64_store(ctx->buffer + 56, bit_count);
    sha256_transform(ctx->state, ctx->buffer);

    /* Write digest */
    for (i = 0; i < 8; i++)
        be32_store(digest + 4 * i, ctx->state[i]);

    /* Clear sensitive data */
    memset(ctx, 0, sizeof(*ctx));
}

void sh_sha256_hex(const void *data, size_t len, char out[65])
{
    static const char hex_chars[] = "0123456789abcdef";
    ShSha256 ctx;
    uint8_t digest[32];
    int i;

    if (!out) return;

    sh_sha256_init(&ctx);
    sh_sha256_update(&ctx, data, len);
    sh_sha256_final(&ctx, digest);

    for (i = 0; i < 32; i++) {
        out[2*i]     = hex_chars[(digest[i] >> 4) & 0x0f];
        out[2*i + 1] = hex_chars[digest[i] & 0x0f];
    }
    out[64] = '\0';
}
