#ifndef SCRIBE_PHI_VAULT_H
#define SCRIBE_PHI_VAULT_H

#include "x12_parser.h"

#include <stddef.h>

#define PHI_VAULT_NAMESPACE_MAX 96u
#define PHI_VAULT_TOKEN_MAX 64u

typedef struct {
    void *db;
} phi_vault_t;

void phi_vault_init(phi_vault_t *vault);
int phi_vault_open(phi_vault_t *vault, const char *path);
int phi_vault_close(phi_vault_t *vault);
int phi_vault_init_schema(phi_vault_t *vault);

int phi_vault_put_mapping(
    phi_vault_t *vault,
    const char *namespace_name,
    const char *token,
    x12_str_t raw_value,
    const char *source_drop_id
);

int phi_vault_resolve(
    phi_vault_t *vault,
    const char *namespace_name,
    const char *token,
    const char *actor,
    const char *purpose,
    char *out,
    size_t out_len
);

#endif
