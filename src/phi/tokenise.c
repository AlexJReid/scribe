#include "tokenise.h"
#include "try.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_DIGEST_BYTES 16u
#define TOKEN_DIGEST_HEX_LEN (TOKEN_DIGEST_BYTES * 2u)
#define TOKEN_HMAC_DIGEST_BYTES 32u

const char *tokenise_namespace(token_type_t type)
{
    switch (type)
    {
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
    case TOK_MEMBER_DOB:
        return "member_dob";
    case TOK_REFERENCE_ID:
        return "reference_id";
    case TOK_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *token_key(void)
{
    const char *key = getenv("SCRIBE_TOKEN_KEY");

    if (key != NULL && key[0] != '\0')
    {
        return key;
    }

    /*
     * Development fallback for deterministic local tests only. Production
     * should provide SCRIBE_TOKEN_KEY from a secret manager or equivalent.
     */
    return "scribe-dev-token-key";
}

static int build_hmac_message(
    const char *namespace_name,
    x12_str_t raw,
    unsigned char **out,
    size_t *out_len)
{
    unsigned char *message;
    size_t namespace_len;
    size_t len;

    if (namespace_name == NULL || raw.ptr == NULL || out == NULL || out_len == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    namespace_len = strlen(namespace_name);
    if (namespace_len > (size_t)-1 - 1u ||
        raw.len > (size_t)-1 - namespace_len - 1u)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    len = namespace_len + 1u + raw.len;
    message = (unsigned char *)malloc(len);
    if (message == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }

    memcpy(message, namespace_name, namespace_len);
    message[namespace_len] = ':';
    memcpy(message + namespace_len + 1u, raw.ptr, raw.len);

    *out = message;
    *out_len = len;
    return X12_OK;
}

static int hmac_sha256(
    const unsigned char *key,
    size_t key_len,
    const char *namespace_name,
    x12_str_t raw,
    unsigned char digest[TOKEN_HMAC_DIGEST_BYTES])
{
    unsigned char *message = NULL;
    unsigned int digest_len = 0u;
    unsigned char *result;
    size_t message_len = 0u;

    if (key == NULL || digest == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (key_len > (size_t)INT_MAX)
    {
        return X12_ERR_UNSUPPORTED;
    }

    TRY(build_hmac_message(namespace_name, raw, &message, &message_len));

    result = HMAC(
        EVP_sha256(),
        key,
        (int)key_len,
        message,
        message_len,
        digest,
        &digest_len);
    OPENSSL_cleanse(message, message_len);
    free(message);

    if (result == NULL || digest_len != TOKEN_HMAC_DIGEST_BYTES)
    {
        return X12_ERR_IO;
    }

    return X12_OK;
}

int tokenise_value(
    token_type_t type,
    x12_str_t raw,
    char *out,
    size_t out_len)
{
    static const char hex[] = "0123456789abcdef";
    const char *type_name = tokenise_namespace(type);
    const char *key = token_key();
    unsigned char digest[TOKEN_HMAC_DIGEST_BYTES];
    size_t i;

    if (raw.ptr == NULL || out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (TOKEN_DIGEST_HEX_LEN >= out_len)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    TRY(hmac_sha256(
        (const unsigned char *)key,
        strlen(key),
        type_name,
        raw,
        digest));

    for (i = 0; i < TOKEN_DIGEST_BYTES; i++)
    {
        out[i * 2u] = hex[(digest[i] >> 4u) & 0x0fu];
        out[i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    out[TOKEN_DIGEST_HEX_LEN] = '\0';
    OPENSSL_cleanse(digest, sizeof(digest));

    return X12_OK;
}
