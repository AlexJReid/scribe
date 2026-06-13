#ifndef SCRIBE_TOKENISE_H
#define SCRIBE_TOKENISE_H

#include "x12_parser.h"

typedef enum
{
    /* Namespace choices are part of the non-PHI cross-file join contract. */
    TOK_PATIENT_ID,
    TOK_MEMBER_ID,
    TOK_PROVIDER_ID,
    TOK_CLAIM_ID,
    TOK_PAYER_CLAIM_CONTROL_NUMBER,
    TOK_PAYER_ID,
    TOK_PATIENT_NAME,
    TOK_MEMBER_NAME,
    TOK_PROVIDER_NAME,
    TOK_PAYER_NAME,
    TOK_ENTITY_NAME,
    TOK_MEMBER_DOB,
    TOK_REFERENCE_ID,
    TOK_UNKNOWN
} token_type_t;

#define TOKENISE_MAX_TOKEN_LEN 40u

const char *tokenise_namespace(token_type_t type);

int tokenise_value(
    token_type_t type,
    x12_str_t raw,
    char *out,
    size_t out_len);

#endif
