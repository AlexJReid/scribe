#include "tokenise.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_DIGEST_BYTES 16u
#define TOKEN_DIGEST_HEX_LEN (TOKEN_DIGEST_BYTES * 2u)

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx_t;

static const uint32_t sha256_k[64] = {
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

const char *tokenise_namespace(token_type_t type)
{
    switch (type) {
    case TOK_PATIENT_ID:
        return "patient_id";
    case TOK_MEMBER_ID:
        return "member_id";
    case TOK_PROVIDER_ID:
        return "provider_id";
    case TOK_CLAIM_ID:
        return "claim_id";
    case TOK_PAYER_CLAIM_CONTROL_NUMBER:
        return "payer_claim_control_number";
    case TOK_PAYER_ID:
        return "payer_id";
    case TOK_PATIENT_NAME:
        return "patient_name";
    case TOK_MEMBER_NAME:
        return "member_name";
    case TOK_PROVIDER_NAME:
        return "provider_name";
    case TOK_PAYER_NAME:
        return "payer_name";
    case TOK_ENTITY_NAME:
        return "entity_name";
    case TOK_UNKNOWN:
    default:
        return "unknown";
    }
}

static uint32_t rotr32(uint32_t value, uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static uint32_t sha256_ch(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

static uint32_t sha256_maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t sha256_ep0(uint32_t x)
{
    return rotr32(x, 2u) ^ rotr32(x, 13u) ^ rotr32(x, 22u);
}

static uint32_t sha256_ep1(uint32_t x)
{
    return rotr32(x, 6u) ^ rotr32(x, 11u) ^ rotr32(x, 25u);
}

static uint32_t sha256_sig0(uint32_t x)
{
    return rotr32(x, 7u) ^ rotr32(x, 18u) ^ (x >> 3u);
}

static uint32_t sha256_sig1(uint32_t x)
{
    return rotr32(x, 17u) ^ rotr32(x, 19u) ^ (x >> 10u);
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t m[64];
    uint32_t t1;
    uint32_t t2;
    size_t i;

    for (i = 0; i < 16u; i++) {
        m[i] = ((uint32_t)data[i * 4u] << 24u) |
               ((uint32_t)data[i * 4u + 1u] << 16u) |
               ((uint32_t)data[i * 4u + 2u] << 8u) |
               (uint32_t)data[i * 4u + 3u];
    }

    for (i = 16u; i < 64u; i++) {
        m[i] = sha256_sig1(m[i - 2u]) + m[i - 7u] +
               sha256_sig0(m[i - 15u]) + m[i - 16u];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64u; i++) {
        t1 = h + sha256_ep1(e) + sha256_ch(e, f, g) + sha256_k[i] + m[i];
        t2 = sha256_ep0(a) + sha256_maj(a, b, c);
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

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64u) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512u;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32])
{
    uint32_t i = ctx->datalen;
    size_t j;

    ctx->data[i++] = 0x80u;
    if (i > 56u) {
        while (i < 64u) {
            ctx->data[i++] = 0x00u;
        }
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56u);
    } else {
        while (i < 56u) {
            ctx->data[i++] = 0x00u;
        }
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8u;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8u);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16u);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24u);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32u);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40u);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48u);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56u);
    sha256_transform(ctx, ctx->data);

    for (j = 0; j < 4u; j++) {
        hash[j] = (uint8_t)(ctx->state[0] >> (24u - j * 8u));
        hash[j + 4u] = (uint8_t)(ctx->state[1] >> (24u - j * 8u));
        hash[j + 8u] = (uint8_t)(ctx->state[2] >> (24u - j * 8u));
        hash[j + 12u] = (uint8_t)(ctx->state[3] >> (24u - j * 8u));
        hash[j + 16u] = (uint8_t)(ctx->state[4] >> (24u - j * 8u));
        hash[j + 20u] = (uint8_t)(ctx->state[5] >> (24u - j * 8u));
        hash[j + 24u] = (uint8_t)(ctx->state[6] >> (24u - j * 8u));
        hash[j + 28u] = (uint8_t)(ctx->state[7] >> (24u - j * 8u));
    }
}

static void hmac_sha256(
    const uint8_t *key,
    size_t key_len,
    const char *namespace,
    x12_str_t raw,
    uint8_t digest[32]
)
{
    uint8_t key_block[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t key_hash[32];
    uint8_t inner_hash[32];
    sha256_ctx_t ctx;
    size_t i;

    if (key_len > 64u) {
        sha256_init(&ctx);
        sha256_update(&ctx, key, key_len);
        sha256_final(&ctx, key_hash);
        key = key_hash;
        key_len = sizeof(key_hash);
    }

    memset(key_block, 0, sizeof(key_block));
    memcpy(key_block, key, key_len);

    for (i = 0; i < sizeof(key_block); i++) {
        ipad[i] = key_block[i] ^ 0x36u;
        opad[i] = key_block[i] ^ 0x5cu;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    sha256_update(&ctx, (const uint8_t *)namespace, strlen(namespace));
    sha256_update(&ctx, (const uint8_t *)":", 1u);
    sha256_update(&ctx, (const uint8_t *)raw.ptr, raw.len);
    sha256_final(&ctx, inner_hash);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, inner_hash, sizeof(inner_hash));
    sha256_final(&ctx, digest);
}

static const char *token_key(void)
{
    const char *key = getenv("SCRIBE_TOKEN_KEY");

    if (key != NULL && key[0] != '\0') {
        return key;
    }

    /*
     * Development fallback for deterministic local tests only. Production
     * should provide SCRIBE_TOKEN_KEY from a secret manager or equivalent.
     */
    return "scribe-dev-token-key";
}

int tokenise_value(
    token_type_t type,
    x12_str_t raw,
    char *out,
    size_t out_len
)
{
    static const char hex[] = "0123456789abcdef";
    const char *type_name = tokenise_namespace(type);
    const char *key = token_key();
    uint8_t digest[32];
    size_t offset;
    size_t i;

    if (raw.ptr == NULL || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    hmac_sha256(
        (const uint8_t *)key,
        strlen(key),
        type_name,
        raw,
        digest
    );

    if (TOKEN_DIGEST_HEX_LEN >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    offset = 0u;
    for (i = 0; i < TOKEN_DIGEST_BYTES; i++) {
        out[offset + i * 2u] = hex[(digest[i] >> 4u) & 0x0fu];
        out[offset + i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    out[offset + TOKEN_DIGEST_HEX_LEN] = '\0';

    return X12_OK;
}
