#ifndef SCRIBE_EVENT_WRITER_H
#define SCRIBE_EVENT_WRITER_H

#include "journal.h"
#include "phi_vault.h"
#include "tokenise.h"
#include "x12_parser.h"

#include <stdio.h>

typedef struct {
    FILE *fp;
    int owns_file;
    const char *source_file;
    const char *source_transaction;
    x12_str_t isa13;
    x12_str_t gs06;
    x12_str_t st02;
    int include_phi;
    phi_vault_t *phi_vault;
    const char *phi_source_ref;
    int binary_journal;
    FILE *payload_sink;
    journal_record_builder_t journal_record;
} event_writer_t;

int event_writer_open(
    event_writer_t *writer,
    const char *out_path,
    const char *source_file,
    const char *source_transaction
);

int event_writer_open_stream(
    event_writer_t *writer,
    FILE *fp,
    const char *source_file,
    const char *source_transaction
);

int event_writer_close(event_writer_t *writer);

void event_writer_set_include_phi(event_writer_t *writer, int include_phi);
int event_writer_include_phi(const event_writer_t *writer);
int event_writer_set_binary_journal(event_writer_t *writer, int binary_journal);
void event_writer_set_phi_vault(
    event_writer_t *writer,
    phi_vault_t *vault,
    const char *source_ref
);
int event_writer_record_phi_mapping(
    event_writer_t *writer,
    token_type_t type,
    x12_str_t raw
);
int event_writer_record_phi_name(
    event_writer_t *writer,
    token_type_t name_type,
    x12_str_t last_name_or_org,
    x12_str_t first_name,
    token_type_t id_type,
    x12_str_t id_raw
);

void event_writer_observe_control(
    event_writer_t *writer,
    const x12_segment_t *seg
);

FILE *event_writer_stream(event_writer_t *writer);

int event_writer_begin_event(
    event_writer_t *writer,
    const char *event_type,
    const x12_segment_t *seg
);

int event_writer_end_event(event_writer_t *writer);

int event_writer_write_json_string(FILE *fp, const char *data, size_t len);
int event_writer_write_cstring(FILE *fp, const char *value);
int event_writer_write_string_field(
    FILE *fp,
    const char *name,
    x12_str_t value,
    int prefix_comma
);
int event_writer_write_cstring_field(
    FILE *fp,
    const char *name,
    const char *value,
    int prefix_comma
);
int event_writer_write_bool_field(
    FILE *fp,
    const char *name,
    int value,
    int prefix_comma
);
int event_writer_write_str_array_field(
    FILE *fp,
    const char *name,
    const x12_str_t *values,
    size_t count,
    int prefix_comma
);

#endif
