#ifndef SCRIBE_EVENT_WRITER_H
#define SCRIBE_EVENT_WRITER_H

#include "journal.h"
#include "phi_vault.h"
#include "tokenise.h"
#include "x12_parser.h"

#include <stdio.h>

#define EVENT_WRITER_SOURCE_DROP_ID_MAX 256u

typedef enum
{
    EVENT_WRITER_MODE_JSON = 0,
    EVENT_WRITER_MODE_JOURNAL = 1
} event_writer_mode_t;

typedef struct
{
    FILE *fp;
    int owns_file;
    int mode;
    const char *source_file;
    const char *source_transaction;
    const char *run_id;
    x12_str_t isa13;
    x12_str_t gs06;
    x12_str_t st02;
    int include_phi;
    phi_vault_t *phi_vault;
    char current_source_drop_id[EVENT_WRITER_SOURCE_DROP_ID_MAX];
    /*
     * Journal mode tracks whether the per-source-drop context fields
     * (source_file, source_transaction, etc.) have already been written
     * out at the start of the current run; subsequent events skip them.
     */
    int journal_context_written;
    char journal_context_source_drop_id[EVENT_WRITER_SOURCE_DROP_ID_MAX];
    /*
     * Staging area for the current event. Filled in by event_add_* and
     * flushed by event_writer_end_event.
     */
    journal_record_builder_t builder;
    int builder_open;
} event_writer_t;

int event_writer_open(
    event_writer_t *writer,
    const char *out_path,
    const char *source_file,
    const char *source_transaction);

int event_writer_open_stream(
    event_writer_t *writer,
    FILE *fp,
    const char *source_file,
    const char *source_transaction);

int event_writer_close(event_writer_t *writer);

void event_writer_set_include_phi(event_writer_t *writer, int include_phi);
int event_writer_include_phi(const event_writer_t *writer);
void event_writer_set_run_id(event_writer_t *writer, const char *run_id);
int event_writer_set_mode(event_writer_t *writer, event_writer_mode_t mode);
void event_writer_set_phi_vault(event_writer_t *writer, phi_vault_t *vault);

int event_writer_record_phi_mapping(
    event_writer_t *writer,
    token_type_t type,
    x12_str_t raw);
int event_writer_record_phi_name(
    event_writer_t *writer,
    token_type_t name_type,
    x12_str_t last_name_or_org,
    x12_str_t first_name,
    token_type_t id_type,
    x12_str_t id_raw);

void event_writer_observe_control(
    event_writer_t *writer,
    const x12_segment_t *seg);

/*
 * Direct access to the underlying FILE*. Intended only for one-shot setup
 * (writing the journal header before switching to journal mode) and tests.
 * Mapper code should not need this.
 */
FILE *event_writer_underlying_file(event_writer_t *writer);

/*
 * Event lifecycle. Begin stages the event_type and the per-event context
 * fields into the builder; add_* fills in the payload; end emits the event
 * in whichever mode is active.
 */
int event_writer_begin_event(
    event_writer_t *writer,
    const char *event_type,
    const x12_segment_t *seg);
int event_writer_add_str(event_writer_t *writer, const char *name, x12_str_t value);
int event_writer_add_cstr(event_writer_t *writer, const char *name, const char *value);
int event_writer_add_bool(event_writer_t *writer, const char *name, int value);
int event_writer_add_str_array(
    event_writer_t *writer,
    const char *name,
    const x12_str_t *values,
    size_t count);
int event_writer_end_event(event_writer_t *writer);

/* Exposed for tests that escape strings into a stream of their own. */
int event_writer_write_json_string(FILE *fp, const char *data, size_t len);

#endif
