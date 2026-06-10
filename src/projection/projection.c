#include "projection.h"

#include "balance_projector.h"

#include <stddef.h>
#include <string.h>

static const projection_plugin_t projection_plugins[] = {
    {
        "balance",
        "claim -> service line ledger balance",
        balance_projector_run_cli
    }
};

const projection_plugin_t *projection_find(const char *name)
{
    size_t i;

    if (name == NULL) {
        return NULL;
    }

    for (i = 0u; i < sizeof(projection_plugins) / sizeof(projection_plugins[0]); i++) {
        if (strcmp(projection_plugins[i].name, name) == 0) {
            return &projection_plugins[i];
        }
    }

    return NULL;
}

void projection_write_usage(FILE *fp)
{
    size_t i;

    if (fp == NULL) {
        return;
    }

    fputs("available projections:\n", fp);
    for (i = 0u; i < sizeof(projection_plugins) / sizeof(projection_plugins[0]); i++) {
        fprintf(fp, "  %s - %s\n", projection_plugins[i].name, projection_plugins[i].summary);
    }
}
