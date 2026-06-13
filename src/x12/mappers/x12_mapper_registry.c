#include "x12_mapper_registry.h"

#include "x12_mapper_270_271.h"
#include "x12_mapper_834.h"
#include "x12_mapper_835.h"
#include "x12_mapper_837.h"

#include <stddef.h>
#include <string.h>

static const x12_mapper_entry_t entries[] = {
    {"270", x12_map_270_document},
    {"271", x12_map_271_document},
    {"834", x12_map_834_document},
    {"835", x12_map_835_document},
    {"837", x12_map_837_document},
};

const x12_mapper_entry_t *x12_mapper_table(size_t *out_count)
{
    if (out_count != NULL)
    {
        *out_count = sizeof(entries) / sizeof(entries[0]);
    }
    return entries;
}

x12_mapper_fn x12_mapper_for_type(const char *type)
{
    size_t i;

    if (type == NULL)
    {
        return NULL;
    }

    for (i = 0u; i < sizeof(entries) / sizeof(entries[0]); i++)
    {
        if (strcmp(entries[i].type, type) == 0)
        {
            return entries[i].map;
        }
    }
    return NULL;
}
