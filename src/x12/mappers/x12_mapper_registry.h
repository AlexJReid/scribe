#ifndef SCRIBE_X12_MAPPER_REGISTRY_H
#define SCRIBE_X12_MAPPER_REGISTRY_H

#include "event_writer.h"
#include "x12_parser.h"

#include <stddef.h>

typedef int (*x12_mapper_fn)(x12_document_t *doc, event_writer_t *writer);

typedef struct
{
    const char *type;
    x12_mapper_fn map;
} x12_mapper_entry_t;

const x12_mapper_entry_t *x12_mapper_table(size_t *out_count);
x12_mapper_fn x12_mapper_for_type(const char *type);

#endif
