#include "x12_parser.h"

#include <ctype.h>
#include <string.h>

static x12_str_t empty_str(void)
{
    x12_str_t value;

    value.ptr = "";
    value.len = 0;
    return value;
}

static int is_line_break(char value)
{
    return value == '\r' || value == '\n';
}

static int parse_size(x12_str_t value, size_t *out)
{
    size_t result = 0;
    size_t i;

    if (value.len == 0 || out == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    for (i = 0; i < value.len; i++) {
        unsigned char ch = (unsigned char)value.ptr[i];

        if (!isdigit(ch)) {
            return X12_ERR_INVALID_ARGUMENT;
        }

        result = (result * 10u) + (size_t)(ch - '0');
    }

    *out = result;
    return X12_OK;
}

static int parse_segment(
    x12_document_t *doc,
    size_t segment_start,
    size_t segment_end,
    size_t segment_index,
    x12_segment_t *seg
)
{
    char *data_start;
    char *data_end;
    char *cursor;
    char *next;

    while (segment_end > segment_start &&
           is_line_break(doc->buffer[segment_end - 1u])) {
        doc->buffer[segment_end - 1u] = '\0';
        segment_end--;
    }

    data_start = doc->buffer + segment_start;
    data_end = doc->buffer + segment_end;

    seg->tag = empty_str();
    seg->element_count = 0;
    seg->segment_index = segment_index;
    seg->byte_offset = segment_start;

    cursor = data_start;
    next = cursor;
    while (next < data_end && *next != doc->delimiters.element_sep) {
        next++;
    }

    seg->tag.ptr = cursor;
    seg->tag.len = (size_t)(next - cursor);

    if (next == data_end) {
        return X12_OK;
    }

    *next = '\0';
    cursor = next + 1;

    while (cursor <= data_end) {
        if (seg->element_count >=
            (sizeof(seg->elements) / sizeof(seg->elements[0]))) {
            return X12_ERR_SEGMENT_OVERFLOW;
        }

        next = cursor;
        while (next < data_end && *next != doc->delimiters.element_sep) {
            next++;
        }

        seg->elements[seg->element_count].ptr = cursor;
        seg->elements[seg->element_count].len = (size_t)(next - cursor);
        seg->element_count++;

        if (next == data_end) {
            break;
        }

        *next = '\0';
        cursor = next + 1;
    }

    return X12_OK;
}

static int validate_transaction_counts(
    const x12_segment_t *seg,
    int *inside_transaction,
    size_t *segments_since_st,
    x12_str_t *current_st02
)
{
    if (x12_str_eq_cstr(seg->tag, "ST")) {
        if (*inside_transaction) {
            return X12_ERR_ST_SE_COUNT;
        }

        *inside_transaction = 1;
        *segments_since_st = 1;
        if (seg->element_count > 1) {
            *current_st02 = seg->elements[1];
        } else {
            *current_st02 = empty_str();
        }
        return X12_OK;
    }

    if (*inside_transaction) {
        *segments_since_st = *segments_since_st + 1u;
    }

    if (x12_str_eq_cstr(seg->tag, "SE")) {
        size_t expected = 0;
        int rc;

        if (!*inside_transaction || seg->element_count < 2) {
            return X12_ERR_ST_SE_COUNT;
        }

        rc = parse_size(seg->elements[0], &expected);
        if (rc != X12_OK) {
            return X12_ERR_ST_SE_COUNT;
        }

        if (expected != *segments_since_st) {
            return X12_ERR_ST_SE_COUNT;
        }

        if (seg->elements[1].len != current_st02->len ||
            memcmp(seg->elements[1].ptr, current_st02->ptr, current_st02->len) != 0) {
            return X12_ERR_ST_SE_CONTROL;
        }

        *inside_transaction = 0;
        *segments_since_st = 0;
        *current_st02 = empty_str();
    }

    return X12_OK;
}

int x12_document_detect_delimiters(x12_document_t *doc)
{
    if (doc == NULL || doc->buffer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (doc->buffer_len < 106u) {
        return X12_ERR_BAD_ISA;
    }

    if (memcmp(doc->buffer, "ISA", 3u) != 0) {
        return X12_ERR_NOT_X12;
    }

    doc->delimiters.element_sep = doc->buffer[3];
    doc->delimiters.component_sep = doc->buffer[104];
    doc->delimiters.segment_term = doc->buffer[105];

    if (doc->delimiters.element_sep == '\0' ||
        doc->delimiters.component_sep == '\0' ||
        doc->delimiters.segment_term == '\0' ||
        is_line_break(doc->delimiters.element_sep) ||
        is_line_break(doc->delimiters.component_sep) ||
        is_line_break(doc->delimiters.segment_term)) {
        return X12_ERR_BAD_ISA;
    }

    return X12_OK;
}

int x12_document_each_segment(
    x12_document_t *doc,
    x12_segment_cb cb,
    void *user
)
{
    size_t pos = 0;
    size_t segment_index = 1;
    int inside_transaction = 0;
    size_t segments_since_st = 0;
    x12_str_t current_st02 = empty_str();
    int rc;

    if (doc == NULL || doc->buffer == NULL || cb == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (doc->delimiters.segment_term == '\0') {
        rc = x12_document_detect_delimiters(doc);
        if (rc != X12_OK) {
            return rc;
        }
    }

    while (pos < doc->buffer_len) {
        size_t segment_start;
        size_t segment_end;
        x12_segment_t seg;

        while (pos < doc->buffer_len && is_line_break(doc->buffer[pos])) {
            pos++;
        }

        if (pos >= doc->buffer_len || doc->buffer[pos] == '\0') {
            break;
        }

        segment_start = pos;
        segment_end = pos;
        while (segment_end < doc->buffer_len &&
               doc->buffer[segment_end] != doc->delimiters.segment_term) {
            segment_end++;
        }

        if (segment_end >= doc->buffer_len) {
            return X12_ERR_MISSING_SEGMENT_TERM;
        }

        doc->buffer[segment_end] = '\0';

        rc = parse_segment(doc, segment_start, segment_end, segment_index, &seg);
        if (rc != X12_OK) {
            return rc;
        }

        if (seg.tag.len > 0) {
            rc = validate_transaction_counts(
                &seg,
                &inside_transaction,
                &segments_since_st,
                &current_st02
            );
            if (rc != X12_OK) {
                return rc;
            }

            rc = cb(&seg, user);
            if (rc != X12_OK) {
                return rc;
            }

            segment_index++;
        }

        pos = segment_end + 1u;
    }

    if (inside_transaction) {
        return X12_ERR_ST_SE_COUNT;
    }

    return X12_OK;
}

int x12_str_eq_cstr(x12_str_t value, const char *literal)
{
    size_t literal_len;

    if (literal == NULL) {
        return 0;
    }

    literal_len = strlen(literal);
    if (value.len != literal_len) {
        return 0;
    }

    return memcmp(value.ptr, literal, literal_len) == 0;
}

const char *x12_error_message(int code)
{
    switch (code) {
    case X12_OK:
        return "ok";
    case X12_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case X12_ERR_IO:
        return "I/O error";
    case X12_ERR_NO_MEMORY:
        return "out of memory";
    case X12_ERR_NOT_X12:
        return "file does not start with ISA";
    case X12_ERR_BAD_ISA:
        return "invalid or truncated ISA segment";
    case X12_ERR_SEGMENT_OVERFLOW:
        return "segment has more than 128 elements";
    case X12_ERR_MISSING_SEGMENT_TERM:
        return "missing segment terminator";
    case X12_ERR_ST_SE_COUNT:
        return "invalid ST/SE transaction segment count";
    case X12_ERR_ST_SE_CONTROL:
        return "ST02/SE02 control number mismatch";
    case X12_ERR_BUFFER_TOO_SMALL:
        return "output buffer too small";
    case X12_ERR_UNSUPPORTED:
        return "unsupported operation";
    case X12_ERR_NOT_FOUND:
        return "not found";
    default:
        return "unknown error";
    }
}
