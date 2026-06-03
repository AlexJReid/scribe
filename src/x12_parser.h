#ifndef SCRIBE_X12_PARSER_H
#define SCRIBE_X12_PARSER_H

#include <stddef.h>

typedef struct {
    char *ptr;
    size_t len;
} x12_str_t;

typedef struct {
    x12_str_t tag;
    x12_str_t elements[128];
    size_t element_count;
    size_t segment_index;
    size_t byte_offset;
} x12_segment_t;

typedef struct {
    char element_sep;
    char component_sep;
    char segment_term;
} x12_delimiters_t;

typedef struct {
    x12_delimiters_t delimiters;
    char *buffer;
    size_t buffer_len;
} x12_document_t;

typedef enum {
    X12_OK = 0,
    X12_ERR_INVALID_ARGUMENT = -1,
    X12_ERR_IO = -2,
    X12_ERR_NO_MEMORY = -3,
    X12_ERR_NOT_X12 = -4,
    X12_ERR_BAD_ISA = -5,
    X12_ERR_SEGMENT_OVERFLOW = -6,
    X12_ERR_MISSING_SEGMENT_TERM = -7,
    X12_ERR_ST_SE_COUNT = -8,
    X12_ERR_ST_SE_CONTROL = -9,
    X12_ERR_BUFFER_TOO_SMALL = -10,
    X12_ERR_UNSUPPORTED = -11,
    X12_ERR_NOT_FOUND = -12,
    X12_ERR_CONFLICT = -13
} x12_error_t;

typedef int (*x12_segment_cb)(
    const x12_segment_t *seg,
    void *user
);

int x12_document_detect_delimiters(x12_document_t *doc);

int x12_document_each_segment(
    x12_document_t *doc,
    x12_segment_cb cb,
    void *user
);

int x12_str_eq_cstr(x12_str_t value, const char *literal);
const char *x12_error_message(int code);

#endif
