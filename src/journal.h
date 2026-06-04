#ifndef SCRIBE_JOURNAL_H
#define SCRIBE_JOURNAL_H

#include "x12_parser.h"

#include <stddef.h>
#include <stdio.h>

#define JOURNAL_EVENT_MAX_FIELDS 160u

typedef enum {
    JOURNAL_VALUE_STRING = 1,
    JOURNAL_VALUE_BOOL = 2,
    JOURNAL_VALUE_U64 = 3,
    JOURNAL_VALUE_STRING_ARRAY = 4
} journal_value_type_t;

typedef struct {
    unsigned short key_id;
    unsigned char type;
    const unsigned char *data;
    size_t len;
} journal_event_field_t;

typedef struct {
    const unsigned char *record;
    size_t record_len;
    long long offset;
    long long stored_len;
    journal_event_field_t fields[JOURNAL_EVENT_MAX_FIELDS];
    size_t field_count;
} journal_event_t;

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
    unsigned short field_count;
} journal_record_builder_t;

typedef struct {
    FILE *fp;
    unsigned char *buffer;
    size_t buffer_cap;
} journal_reader_t;

int journal_write_header(FILE *fp);

void journal_record_builder_init(journal_record_builder_t *builder);
void journal_record_builder_free(journal_record_builder_t *builder);
int journal_record_builder_reset(journal_record_builder_t *builder);
int journal_record_add_string(
    journal_record_builder_t *builder,
    const char *key,
    const char *value,
    size_t value_len
);
int journal_record_add_cstring(
    journal_record_builder_t *builder,
    const char *key,
    const char *value
);
int journal_record_add_bool(
    journal_record_builder_t *builder,
    const char *key,
    int value
);
int journal_record_add_u64(
    journal_record_builder_t *builder,
    const char *key,
    unsigned long long value
);
int journal_record_add_string_array(
    journal_record_builder_t *builder,
    const char *key,
    const x12_str_t *values,
    size_t count
);
int journal_write_record(
    FILE *fp,
    journal_record_builder_t *builder,
    long long *out_offset,
    long long *out_stored_len
);

void journal_reader_init(journal_reader_t *reader);
int journal_reader_open(journal_reader_t *reader, const char *path);
int journal_reader_next(journal_reader_t *reader, journal_event_t *out);
int journal_reader_close(journal_reader_t *reader);

int journal_event_get_string(
    const journal_event_t *event,
    const char *key,
    char *out,
    size_t out_len
);
int journal_event_get_bool(const journal_event_t *event, const char *key, int *out);
int journal_event_get_number_text(
    const journal_event_t *event,
    const char *key,
    char *out,
    size_t out_len
);
int journal_event_get_array_string_at(
    const journal_event_t *event,
    const char *key,
    size_t index,
    char *out,
    size_t out_len
);

#endif
