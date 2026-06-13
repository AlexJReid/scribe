#ifndef SCRIBE_X12_MAPPER_PHI_H
#define SCRIBE_X12_MAPPER_PHI_H

#include "event_writer.h"
#include "tokenise.h"
#include "try.h"
#include "x12_parser.h"

#include <stddef.h>

/* An empty (zero-length, "") x12_str_t. */
x12_str_t x12_mapper_empty_str(void);

/* Element at index, or empty_str() when the segment has fewer elements. */
x12_str_t x12_mapper_element_or_empty(const x12_segment_t *seg, size_t index);

/* Add a PHI string field, but only when the writer is configured to
 * include PHI. No-op (X12_OK) otherwise. */
int x12_mapper_add_phi_str(event_writer_t *writer, const char *name, x12_str_t value);

/* When PHI is excluded this is a no-op (X12_OK). When PHI is included the
 * field is written as a token (empty for an empty raw value). */
int x12_mapper_add_phi_token(
    event_writer_t *writer,
    const char *name,
    token_type_t type,
    x12_str_t raw
);

/* Add a field as the raw value (recording a PHI mapping) when PHI is
 * included, or as a token when PHI is excluded. */
int x12_mapper_add_tokenized_or_phi(
    event_writer_t *writer,
    const char *name,
    token_type_t type,
    x12_str_t raw
);

#endif
