#ifndef TUDOU_SHA256_H
#define TUDOU_SHA256_H

#include <stdint.h>

/*
 * 最小 SHA-256 / HMAC-SHA256（内核自包含，不依赖 libc/openssl）。
 *
 * 用途：UDP 远程 MCP 信封完整性/来源认证（kernel/net/netmcp.c）。只做
 * 流式哈希，不分配堆内存；上下文可放在栈上（约 112B / 176B）。
 */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    uint32_t buflen;
} sha256_ctx_t;

typedef struct {
    sha256_ctx_t inner;
    uint8_t opad[64];
} hmac_sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, uint32_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[32]);

void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const uint8_t *key,
                      uint32_t keylen);
void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const void *data, uint32_t len);
void hmac_sha256_final(hmac_sha256_ctx_t *ctx, uint8_t out[32]);

#endif /* TUDOU_SHA256_H */
