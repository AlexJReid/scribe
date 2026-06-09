#ifndef SCRIBE_X12_MAPPER_270_271_H
#define SCRIBE_X12_MAPPER_270_271_H

#include "event_writer.h"
#include "x12_parser.h"

typedef struct {
    event_writer_t *writer;
    const char *transaction_type;
    int is_response;
    x12_str_t eligibility_id;
    x12_str_t current_member_id;
    x12_str_t current_payer_id;
    x12_str_t current_service_type_code;
    char component_sep;
} x12_mapper_270_271_t;

void x12_mapper_270_init(x12_mapper_270_271_t *mapper, event_writer_t *writer);
void x12_mapper_271_init(x12_mapper_270_271_t *mapper, event_writer_t *writer);
int x12_mapper_270_271_on_segment(const x12_segment_t *seg, void *user);
int x12_map_270_document(x12_document_t *doc, event_writer_t *writer);
int x12_map_271_document(x12_document_t *doc, event_writer_t *writer);

#endif
