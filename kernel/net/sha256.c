#include "sha256.h"

/*
 * SHA-256（FIPS 180-4）+ HMAC（RFC 2104）。实现刻意保持无堆、无 libc：
 * 所有状态在调用方提供的上下文里，适合内核任务/控制台上下文调用。
 */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void mem_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static void mem_zero(uint8_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = 0;
    }
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t *block)
{
    uint32_t w[64];
    for (uint32_t i = 0; i < 16u; i++) {
        w[i] = load_be32(block + i * 4u);
    }
    for (uint32_t i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr32(w[i - 15u], 7u) ^ rotr32(w[i - 15u], 18u) ^
                      (w[i - 15u] >> 3);
        uint32_t s1 = rotr32(w[i - 2u], 17u) ^ rotr32(w[i - 2u], 19u) ^
                      (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->bitlen += (uint64_t)len * 8u;

    if (ctx->buflen != 0u) {
        uint32_t need = 64u - ctx->buflen;
        uint32_t take = (len < need) ? len : need;
        mem_copy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        len -= take;
        if (ctx->buflen == 64u) {
            sha256_transform(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
    while (len >= 64u) {
        sha256_transform(ctx, p);
        p += 64u;
        len -= 64u;
    }
    if (len != 0u) {
        mem_copy(ctx->buf, p, len);
        ctx->buflen = len;
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->bitlen;

    ctx->buf[ctx->buflen++] = 0x80u;
    if (ctx->buflen > 56u) {
        while (ctx->buflen < 64u) {
            ctx->buf[ctx->buflen++] = 0;
        }
        sha256_transform(ctx, ctx->buf);
        ctx->buflen = 0;
    }
    while (ctx->buflen < 56u) {
        ctx->buf[ctx->buflen++] = 0;
    }
    for (uint32_t i = 0; i < 8u; i++) {
        ctx->buf[56u + i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    sha256_transform(ctx, ctx->buf);
    ctx->buflen = 0;

    for (uint32_t i = 0; i < 8u; i++) {
        store_be32(out + i * 4u, ctx->state[i]);
    }
}

void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const uint8_t *key,
                      uint32_t keylen)
{
    uint8_t k0[64];
    uint8_t ipad[64];
    mem_zero(k0, sizeof(k0));

    if (keylen > 64u) {
        sha256_ctx_t kh;
        sha256_init(&kh);
        sha256_update(&kh, key, keylen);
        sha256_final(&kh, k0);
    } else {
        mem_copy(k0, key, keylen);
    }

    for (uint32_t i = 0; i < 64u; i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36u);
        ctx->opad[i] = (uint8_t)(k0[i] ^ 0x5cu);
    }
    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, ipad, 64u);
}

void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const void *data, uint32_t len)
{
    sha256_update(&ctx->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx_t *ctx, uint8_t out[32])
{
    uint8_t inner_digest[32];
    sha256_final(&ctx->inner, inner_digest);

    sha256_ctx_t outer;
    sha256_init(&outer);
    sha256_update(&outer, ctx->opad, 64u);
    sha256_update(&outer, inner_digest, 32u);
    sha256_final(&outer, out);
}
