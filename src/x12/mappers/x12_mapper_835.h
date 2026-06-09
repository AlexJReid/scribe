#ifndef SCRIBE_X12_MAPPER_835_H
#define SCRIBE_X12_MAPPER_835_H

#include "event_writer.h"
#include "x12_parser.h"

typedef struct {
    int present;
    x12_segment_t segment;
} x12_mapper_835_buffered_segment_t;

typedef struct {
    event_writer_t *writer;
    x12_mapper_835_buffered_segment_t bpr;
    x12_str_t remittance_id;
    x12_str_t current_claim_id;
    size_t current_service_line_index;
    char current_service_line_number[32];
    int in_claim;
    int in_service_line;
    char component_sep;
} x12_mapper_835_t;

void x12_mapper_835_init(x12_mapper_835_t *mapper, event_writer_t *writer);
int x12_mapper_835_on_segment(const x12_segment_t *seg, void *user);
int x12_map_835_document(x12_document_t *doc, event_writer_t *writer);

#endif
