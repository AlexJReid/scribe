#ifndef SCRIBE_X12_MAPPER_834_H
#define SCRIBE_X12_MAPPER_834_H

#include "event_writer.h"
#include "x12_parser.h"

typedef struct
{
    event_writer_t *writer;
} x12_mapper_834_t;

void x12_mapper_834_init(x12_mapper_834_t *mapper, event_writer_t *writer);
int x12_mapper_834_on_segment(const x12_segment_t *seg, void *user);
int x12_map_834_document(x12_document_t *doc, event_writer_t *writer);

#endif
