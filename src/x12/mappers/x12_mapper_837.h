#ifndef SCRIBE_X12_MAPPER_837_H
#define SCRIBE_X12_MAPPER_837_H

#include "event_writer.h"
#include "x12_parser.h"

typedef struct
{
    int present;
    x12_segment_t segment;
} x12_mapper_837_buffered_segment_t;

typedef struct
{
    event_writer_t *writer;
    x12_str_t current_claim_id;
    x12_str_t current_service_line_number;
    x12_mapper_837_buffered_segment_t billing_provider;
    x12_mapper_837_buffered_segment_t subscriber;
    x12_mapper_837_buffered_segment_t patient;
    x12_mapper_837_buffered_segment_t rendering_provider;
    x12_mapper_837_buffered_segment_t billing_provider_taxonomy;
    x12_mapper_837_buffered_segment_t rendering_provider_taxonomy;
    int billing_provider_taxonomy_pending_for_nm1;
    x12_mapper_837_buffered_segment_t subscriber_info;
    x12_mapper_837_buffered_segment_t subscriber_demographics;
    x12_mapper_837_buffered_segment_t patient_info;
    x12_mapper_837_buffered_segment_t patient_demographics;
    const char *current_provider_context;
    int in_service_line;
    int current_party;
    char component_sep;
} x12_mapper_837_t;

void x12_mapper_837_init(x12_mapper_837_t *mapper, event_writer_t *writer);
int x12_mapper_837_on_segment(const x12_segment_t *seg, void *user);
int x12_map_837_document(x12_document_t *doc, event_writer_t *writer);

#endif
