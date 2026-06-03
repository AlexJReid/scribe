#include "aggregate_stitcher.h"

#include "event_writer.h"
#include "store.h"
#include "tokenise.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STITCH_LINE_MAX 8192u
#define STITCH_ID_MAX 128u
#define STITCH_VALUE_MAX 256u
#define STITCH_FINGERPRINT_MAX 384u
#define STITCH_MAX_AGGREGATES 128u
#define STITCH_MAX_SOURCE_EVENTS 512u
#define STITCH_MAX_UPDATE_BATCHES 128u

typedef struct {
    size_t event_id;
    char fingerprint[STITCH_FINGERPRINT_MAX];
} stitched_source_event_t;

typedef struct {
    char key[STITCH_ID_MAX];
    char claim_id[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_claim_control_number[STITCH_ID_MAX];
    char payer_claim_control_number_token[TOKENISE_MAX_TOKEN_LEN];
    char encounter_id[STITCH_ID_MAX];
    char claim_type[64];
    char claim_status_code[32];
    size_t version;
    int has_charge_context;
    int has_837;
    int has_835;
    size_t submitted_service_line_count;
    size_t remittance_service_line_count;
    size_t adjustment_count;
    stitched_source_event_t source_events[STITCH_MAX_SOURCE_EVENTS];
    size_t source_event_count;
} claim_aggregate_t;

typedef struct {
    claim_aggregate_t *aggregate;
    char drop_key[STITCH_FINGERPRINT_MAX];
    char source_drop_id[STITCH_VALUE_MAX];
    size_t first_source_event_index;
    size_t source_event_count;
    size_t updated_by_event_id;
    char updated_by_event_type[96];
} stitch_update_batch_t;

typedef struct {
    claim_aggregate_t aggregates[STITCH_MAX_AGGREGATES];
    size_t aggregate_count;
    stitch_update_batch_t update_batches[STITCH_MAX_UPDATE_BATCHES];
    size_t update_batch_count;
    char current_drop_key[STITCH_FINGERPRINT_MAX];
    char current_source_drop_id[STITCH_VALUE_MAX];
    size_t source_drop_count;
    char encounter_filter[STITCH_ID_MAX];
    int include_phi;
    FILE *out;
    scribe_store_t *read_store;
} stitch_state_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} stitch_json_buffer_t;

static void copy_cstr(char *out, size_t out_len, const char *value)
{
    size_t len;

    if (out == NULL || out_len == 0u) {
        return;
    }
    if (value == NULL) {
        value = "";
    }

    len = strlen(value);
    if (len >= out_len) {
        len = out_len - 1u;
    }
    memcpy(out, value, len);
    out[len] = '\0';
}

static int json_buffer_append(stitch_json_buffer_t *buffer, const char *value)
{
    size_t len;

    if (buffer == NULL || buffer->data == NULL || value == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    len = strlen(value);
    if (buffer->len + len >= buffer->cap) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(buffer->data + buffer->len, value, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return X12_OK;
}

static int json_buffer_append_char(stitch_json_buffer_t *buffer, char value)
{
    if (buffer == NULL || buffer->data == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (buffer->len + 1u >= buffer->cap) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    buffer->data[buffer->len++] = value;
    buffer->data[buffer->len] = '\0';
    return X12_OK;
}

static int json_buffer_append_json_string(stitch_json_buffer_t *buffer, const char *value)
{
    const unsigned char *cursor;
    char escaped[8];
    int written;
    int rc;

    if (value == NULL) {
        value = "";
    }

    rc = json_buffer_append_char(buffer, '"');
    if (rc != X12_OK) {
        return rc;
    }

    for (cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        switch (*cursor) {
            case '"':
                rc = json_buffer_append(buffer, "\\\"");
                break;
            case '\\':
                rc = json_buffer_append(buffer, "\\\\");
                break;
            case '\n':
                rc = json_buffer_append(buffer, "\\n");
                break;
            case '\r':
                rc = json_buffer_append(buffer, "\\r");
                break;
            case '\t':
                rc = json_buffer_append(buffer, "\\t");
                break;
            default:
                if (*cursor < 0x20u) {
                    written = snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int)*cursor);
                    if (written < 0 || (size_t)written >= sizeof(escaped)) {
                        return X12_ERR_BUFFER_TOO_SMALL;
                    }
                    rc = json_buffer_append(buffer, escaped);
                } else {
                    rc = json_buffer_append_char(buffer, (char)*cursor);
                }
                break;
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return json_buffer_append_char(buffer, '"');
}

static int json_buffer_append_string_field(
    stitch_json_buffer_t *buffer,
    const char *key,
    const char *value,
    int with_comma
)
{
    int rc;

    if (with_comma) {
        rc = json_buffer_append_char(buffer, ',');
        if (rc != X12_OK) {
            return rc;
        }
    }

    rc = json_buffer_append_json_string(buffer, key);
    if (rc != X12_OK) {
        return rc;
    }
    rc = json_buffer_append_char(buffer, ':');
    if (rc != X12_OK) {
        return rc;
    }
    return json_buffer_append_json_string(buffer, value);
}

static int json_buffer_append_size_field(
    stitch_json_buffer_t *buffer,
    const char *key,
    size_t value,
    int with_comma
)
{
    char number[32];
    int written;
    int rc;

    written = snprintf(number, sizeof(number), "%zu", value);
    if (written < 0 || (size_t)written >= sizeof(number)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    if (with_comma) {
        rc = json_buffer_append_char(buffer, ',');
        if (rc != X12_OK) {
            return rc;
        }
    }
    rc = json_buffer_append_json_string(buffer, key);
    if (rc != X12_OK) {
        return rc;
    }
    rc = json_buffer_append_char(buffer, ':');
    if (rc != X12_OK) {
        return rc;
    }
    return json_buffer_append(buffer, number);
}

static int json_buffer_append_bool_field(
    stitch_json_buffer_t *buffer,
    const char *key,
    int value,
    int with_comma
)
{
    int rc;

    if (with_comma) {
        rc = json_buffer_append_char(buffer, ',');
        if (rc != X12_OK) {
            return rc;
        }
    }
    rc = json_buffer_append_json_string(buffer, key);
    if (rc != X12_OK) {
        return rc;
    }
    rc = json_buffer_append_char(buffer, ':');
    if (rc != X12_OK) {
        return rc;
    }
    return json_buffer_append(buffer, value ? "true" : "false");
}

static int json_get_string(
    const char *line,
    const char *key,
    char *out,
    size_t out_len
)
{
    char pattern[96];
    const char *cursor;
    const char *start;
    size_t len = 0u;
    int written;

    if (line == NULL || key == NULL || out == NULL || out_len == 0u) {
        return 0;
    }

    out[0] = '\0';
    written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(pattern)) {
        return 0;
    }

    cursor = strstr(line, pattern);
    if (cursor == NULL) {
        return 0;
    }
    cursor += strlen(pattern);
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor != ':') {
        return 0;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor != '"') {
        return 0;
    }
    cursor++;
    start = cursor;

    while (*cursor != '\0' && *cursor != '"' && len + 1u < out_len) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
        }
        cursor++;
        len = (size_t)(cursor - start);
    }
    if (*cursor != '"' || len >= out_len) {
        return 0;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

static int json_get_number_text(
    const char *line,
    const char *key,
    char *out,
    size_t out_len
)
{
    char pattern[96];
    const char *cursor;
    const char *start;
    size_t len = 0u;
    int written;

    if (line == NULL || key == NULL || out == NULL || out_len == 0u) {
        return 0;
    }

    out[0] = '\0';
    written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(pattern)) {
        return 0;
    }

    cursor = strstr(line, pattern);
    if (cursor == NULL) {
        return 0;
    }
    cursor += strlen(pattern);
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor != ':') {
        return 0;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    start = cursor;
    while ((*cursor >= '0' && *cursor <= '9') && len + 1u < out_len) {
        cursor++;
        len = (size_t)(cursor - start);
    }
    if (len == 0u || len >= out_len) {
        return 0;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

static const char *claim_key(const char *claim_id, const char *claim_id_token)
{
    if (claim_id_token != NULL && claim_id_token[0] != '\0') {
        return claim_id_token;
    }

    return claim_id;
}

static void extract_claim_keys(
    const char *journal_line,
    char *claim_id,
    size_t claim_id_len,
    char *claim_id_token,
    size_t token_len
)
{
    claim_id[0] = '\0';
    claim_id_token[0] = '\0';
    (void)json_get_string(journal_line, "claim_id", claim_id, claim_id_len);
    (void)json_get_string(journal_line, "claim_id_token", claim_id_token, token_len);
}

static claim_aggregate_t *find_aggregate(stitch_state_t *state, const char *key)
{
    size_t i;

    if (state == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }

    for (i = 0u; i < state->aggregate_count; i++) {
        if (strcmp(state->aggregates[i].key, key) == 0) {
            return &state->aggregates[i];
        }
    }

    return NULL;
}

static claim_aggregate_t *find_or_add_aggregate(
    stitch_state_t *state,
    const char *claim_id,
    const char *claim_id_token
)
{
    const char *key = claim_key(claim_id, claim_id_token);
    claim_aggregate_t *aggregate;

    aggregate = find_aggregate(state, key);
    if (aggregate != NULL) {
        if (aggregate->claim_id[0] == '\0') {
            copy_cstr(aggregate->claim_id, sizeof(aggregate->claim_id), claim_id);
        }
        if (aggregate->claim_id_token[0] == '\0') {
            copy_cstr(aggregate->claim_id_token, sizeof(aggregate->claim_id_token), claim_id_token);
        }
        return aggregate;
    }

    if (state->aggregate_count >= STITCH_MAX_AGGREGATES) {
        return NULL;
    }

    aggregate = &state->aggregates[state->aggregate_count++];
    memset(aggregate, 0, sizeof(*aggregate));
    copy_cstr(aggregate->key, sizeof(aggregate->key), key);
    copy_cstr(aggregate->claim_id, sizeof(aggregate->claim_id), claim_id);
    copy_cstr(aggregate->claim_id_token, sizeof(aggregate->claim_id_token), claim_id_token);

    return aggregate;
}

static int aggregate_has_fingerprint(
    const claim_aggregate_t *aggregate,
    const char *fingerprint
)
{
    size_t i;

    for (i = 0u; i < aggregate->source_event_count; i++) {
        if (strcmp(aggregate->source_events[i].fingerprint, fingerprint) == 0) {
            return 1;
        }
    }

    return 0;
}

static int append_source_event(
    claim_aggregate_t *aggregate,
    size_t event_id,
    const char *fingerprint
)
{
    stitched_source_event_t *source_event;

    if (aggregate == NULL || fingerprint == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (aggregate->source_event_count >= STITCH_MAX_SOURCE_EVENTS) {
        return X12_ERR_NO_MEMORY;
    }

    source_event = &aggregate->source_events[aggregate->source_event_count++];
    source_event->event_id = event_id;
    copy_cstr(source_event->fingerprint, sizeof(source_event->fingerprint), fingerprint);

    return X12_OK;
}

static int make_drop_key(
    const char *journal_line,
    char *out,
    size_t out_len
)
{
    char source_file[STITCH_VALUE_MAX] = "";
    char source_transaction[32] = "";
    int written;

    if (journal_line == NULL || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    out[0] = '\0';
    (void)json_get_string(journal_line, "source_file", source_file, sizeof(source_file));
    (void)json_get_string(
        journal_line,
        "source_transaction",
        source_transaction,
        sizeof(source_transaction)
    );
    if (source_file[0] == '\0' && source_transaction[0] == '\0') {
        return X12_OK;
    }

    written = snprintf(out, out_len, "%s|%s", source_transaction, source_file);
    if (written < 0 || (size_t)written >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return X12_OK;
}

static int make_fingerprint(
    const char *journal_line,
    const char *event_type,
    const char *aggregate_key,
    char *out,
    size_t out_len
)
{
    char source_file[STITCH_VALUE_MAX] = "";
    char segment_index[32] = "";
    char service_line_number[32] = "";
    char procedure_code[32] = "";
    char adjustment_group_code[32] = "";
    int written;

    (void)json_get_string(journal_line, "source_file", source_file, sizeof(source_file));
    (void)json_get_number_text(journal_line, "source_segment_index", segment_index, sizeof(segment_index));
    (void)json_get_string(journal_line, "service_line_number", service_line_number, sizeof(service_line_number));
    (void)json_get_string(journal_line, "procedure_code", procedure_code, sizeof(procedure_code));
    (void)json_get_string(journal_line, "adjustment_group_code", adjustment_group_code, sizeof(adjustment_group_code));

    written = snprintf(
        out,
        out_len,
        "%s|%s|%s|%s|%s|%s|%s",
        event_type,
        source_file,
        segment_index,
        aggregate_key,
        service_line_number,
        procedure_code,
        adjustment_group_code
    );
    if (written < 0 || (size_t)written >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return X12_OK;
}

static int event_matches_filter(const stitch_state_t *state, const char *journal_line)
{
    char encounter_id[STITCH_ID_MAX];

    if (state->encounter_filter[0] == '\0') {
        return 1;
    }

    if (!json_get_string(journal_line, "encounter_id", encounter_id, sizeof(encounter_id))) {
        return 1;
    }

    return strcmp(encounter_id, state->encounter_filter) == 0;
}

static int event_has_encounter_id(const char *journal_line)
{
    char encounter_id[STITCH_ID_MAX];

    return json_get_string(journal_line, "encounter_id", encounter_id, sizeof(encounter_id));
}

static int write_applied_event_ids(FILE *fp, const claim_aggregate_t *aggregate)
{
    size_t i;

    if (fputs(",\"applied_event_ids\":[", fp) == EOF) {
        return X12_ERR_IO;
    }
    for (i = 0u; i < aggregate->source_event_count; i++) {
        if (i > 0u && fputc(',', fp) == EOF) {
            return X12_ERR_IO;
        }
        if (fprintf(fp, "%zu", aggregate->source_events[i].event_id) < 0) {
            return X12_ERR_IO;
        }
    }
    if (fputc(']', fp) == EOF) {
        return X12_ERR_IO;
    }

    return X12_OK;
}

static int write_update_event_ids(FILE *fp, const stitch_update_batch_t *batch)
{
    size_t i;
    size_t end;

    if (batch == NULL || batch->aggregate == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (fputs(",\"update_event_ids\":[", fp) == EOF) {
        return X12_ERR_IO;
    }
    end = batch->first_source_event_index + batch->source_event_count;
    for (i = batch->first_source_event_index; i < end; i++) {
        if (i > batch->first_source_event_index && fputc(',', fp) == EOF) {
            return X12_ERR_IO;
        }
        if (fprintf(fp, "%zu", batch->aggregate->source_events[i].event_id) < 0) {
            return X12_ERR_IO;
        }
    }
    if (fputc(']', fp) == EOF) {
        return X12_ERR_IO;
    }

    return X12_OK;
}

static int build_snapshot_state_json(
    const stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id,
    char *out,
    size_t out_len
)
{
    const claim_aggregate_t *aggregate;
    stitch_json_buffer_t buffer;
    int include_phi;
    int rc;

    if (state == NULL || batch == NULL || batch->aggregate == NULL ||
        aggregate_id == NULL || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    aggregate = batch->aggregate;
    include_phi = state->include_phi;
    buffer.data = out;
    buffer.len = 0u;
    buffer.cap = out_len;
    out[0] = '\0';

    rc = json_buffer_append_char(&buffer, '{');
    if (rc == X12_OK) {
        rc = json_buffer_append_string_field(
            &buffer,
            "event_type",
            "ClaimAggregateUpdated",
            0
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_string_field(&buffer, "aggregate_type", "claim", 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_string_field(&buffer, "aggregate_id", aggregate_id, 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_size_field(&buffer, "version", aggregate->version, 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_size_field(
            &buffer,
            "updated_by_event_id",
            batch->updated_by_event_id,
            1
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_string_field(
            &buffer,
            "updated_by_event_type",
            batch->updated_by_event_type,
            1
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_string_field(&buffer, "update_scope", "source_drop", 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_string_field(&buffer, "source_drop_id", batch->source_drop_id, 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_size_field(
            &buffer,
            "compacted_source_event_count",
            batch->source_event_count,
            1
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_bool_field(&buffer, "contains_phi", include_phi, 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append(&buffer, ",\"keys\":{");
    }
    if (rc == X12_OK && include_phi) {
        rc = json_buffer_append_string_field(&buffer, "claim_id", aggregate->claim_id, 0);
        if (rc == X12_OK) {
            rc = json_buffer_append_string_field(
                &buffer,
                "claim_id_token",
                aggregate->claim_id_token,
                1
            );
        }
    } else if (rc == X12_OK) {
        rc = json_buffer_append_string_field(&buffer, "claim_id", aggregate->key, 0);
    }
    if (rc == X12_OK && aggregate->payer_claim_control_number[0] != '\0') {
        if (include_phi) {
            rc = json_buffer_append_string_field(
                &buffer,
                "payer_claim_control_number",
                aggregate->payer_claim_control_number,
                1
            );
            if (rc == X12_OK) {
                rc = json_buffer_append_string_field(
                    &buffer,
                    "payer_claim_control_number_token",
                    aggregate->payer_claim_control_number_token,
                    1
                );
            }
        } else {
            rc = json_buffer_append_string_field(
                &buffer,
                "payer_claim_control_number",
                aggregate->payer_claim_control_number,
                1
            );
        }
    }
    if (rc == X12_OK && aggregate->encounter_id[0] != '\0') {
        rc = json_buffer_append_string_field(&buffer, "encounter_id", aggregate->encounter_id, 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append(&buffer, "},\"state\":{");
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_bool_field(
            &buffer,
            "has_charge_context",
            aggregate->has_charge_context,
            0
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_bool_field(&buffer, "has_837", aggregate->has_837, 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_bool_field(&buffer, "has_835", aggregate->has_835, 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_string_field(&buffer, "claim_type", aggregate->claim_type, 1);
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_string_field(
            &buffer,
            "claim_status_code",
            aggregate->claim_status_code,
            1
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_size_field(
            &buffer,
            "source_event_count",
            aggregate->source_event_count,
            1
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_size_field(
            &buffer,
            "submitted_service_line_count",
            aggregate->submitted_service_line_count,
            1
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_size_field(
            &buffer,
            "remittance_service_line_count",
            aggregate->remittance_service_line_count,
            1
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_size_field(
            &buffer,
            "adjustment_count",
            aggregate->adjustment_count,
            1
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append(&buffer, "},\"lineage\":{");
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_size_field(
            &buffer,
            "applied_event_count",
            aggregate->source_event_count,
            0
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append_size_field(
            &buffer,
            "update_event_count",
            batch->source_event_count,
            1
        );
    }
    if (rc == X12_OK) {
        rc = json_buffer_append(&buffer, "}}");
    }

    return rc;
}

static int persist_snapshot_to_read_store(
    stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id
)
{
    char state_json[4096];
    char updated_by_event_id[32];
    int written;
    int rc;

    if (state == NULL || state->read_store == NULL) {
        return X12_OK;
    }

    rc = build_snapshot_state_json(
        state,
        batch,
        aggregate_id,
        state_json,
        sizeof(state_json)
    );
    if (rc != X12_OK) {
        return rc;
    }

    written = snprintf(
        updated_by_event_id,
        sizeof(updated_by_event_id),
        "%zu",
        batch->updated_by_event_id
    );
    if (written < 0 || (size_t)written >= sizeof(updated_by_event_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return scribe_store_put_claim_aggregate(
        state->read_store,
        aggregate_id,
        batch->aggregate->version,
        state_json,
        updated_by_event_id,
        batch->source_drop_id
    );
}

static int write_snapshot(
    stitch_state_t *state,
    const stitch_update_batch_t *batch
)
{
    FILE *fp = state->out;
    const claim_aggregate_t *aggregate = batch->aggregate;
    char aggregate_id[STITCH_ID_MAX + 16u];
    int written;

    if (fputc('{', fp) == EOF) {
        return X12_ERR_IO;
    }
    written = snprintf(aggregate_id, sizeof(aggregate_id), "claim:%s", aggregate->key);
    if (written < 0 || (size_t)written >= sizeof(aggregate_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    if (event_writer_write_cstring_field(fp, "event_type", "ClaimAggregateUpdated", 0) != X12_OK ||
        event_writer_write_cstring_field(fp, "aggregate_type", "claim", 1) != X12_OK ||
        event_writer_write_cstring_field(fp, "aggregate_id", aggregate_id, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fprintf(
            fp,
            ",\"version\":%zu,\"updated_by_event_id\":%zu",
            aggregate->version,
            batch->updated_by_event_id
        ) < 0) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "updated_by_event_type", batch->updated_by_event_type, 1) != X12_OK ||
        event_writer_write_cstring_field(fp, "update_scope", "source_drop", 1) != X12_OK ||
        event_writer_write_cstring_field(fp, "source_drop_id", batch->source_drop_id, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fprintf(fp, ",\"compacted_source_event_count\":%zu", batch->source_event_count) < 0) {
        return X12_ERR_IO;
    }

    if (fputs(",\"keys\":{", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (state->include_phi) {
        if (event_writer_write_cstring_field(fp, "claim_id", aggregate->claim_id, 0) != X12_OK ||
            event_writer_write_cstring_field(fp, "claim_id_token", aggregate->claim_id_token, 1) != X12_OK) {
            return X12_ERR_IO;
        }
    } else if (event_writer_write_cstring_field(fp, "claim_id", aggregate->key, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (aggregate->payer_claim_control_number[0] != '\0') {
        if (state->include_phi) {
            if (event_writer_write_cstring_field(
                    fp,
                    "payer_claim_control_number",
                    aggregate->payer_claim_control_number,
                    1
                ) != X12_OK ||
                event_writer_write_cstring_field(
                    fp,
                    "payer_claim_control_number_token",
                    aggregate->payer_claim_control_number_token,
                    1
                ) != X12_OK) {
                return X12_ERR_IO;
            }
        } else if (event_writer_write_cstring_field(
                fp,
                "payer_claim_control_number",
                aggregate->payer_claim_control_number,
                1
            ) != X12_OK) {
            return X12_ERR_IO;
        }
    }
    if (aggregate->encounter_id[0] != '\0' &&
        event_writer_write_cstring_field(fp, "encounter_id", aggregate->encounter_id, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs("},\"state\":{", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (fprintf(
            fp,
            "\"has_charge_context\":%s,\"has_837\":%s,\"has_835\":%s",
            aggregate->has_charge_context ? "true" : "false",
            aggregate->has_837 ? "true" : "false",
            aggregate->has_835 ? "true" : "false"
        ) < 0) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "claim_type", aggregate->claim_type, 1) != X12_OK ||
        event_writer_write_cstring_field(fp, "claim_status_code", aggregate->claim_status_code, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fprintf(
            fp,
            ",\"source_event_count\":%zu,\"submitted_service_line_count\":%zu,"
            "\"remittance_service_line_count\":%zu,\"adjustment_count\":%zu",
            aggregate->source_event_count,
            aggregate->submitted_service_line_count,
            aggregate->remittance_service_line_count,
            aggregate->adjustment_count
        ) < 0) {
        return X12_ERR_IO;
    }
    if (fputc('}', fp) == EOF) {
        return X12_ERR_IO;
    }
    if (write_applied_event_ids(fp, aggregate) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_update_event_ids(fp, batch) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs("}\n", fp) == EOF) {
        return X12_ERR_IO;
    }

    return persist_snapshot_to_read_store(state, batch, aggregate_id);
}

static int record_batch_update(
    stitch_state_t *state,
    claim_aggregate_t *aggregate,
    const char *drop_key,
    size_t event_id,
    const char *event_type,
    const char *fingerprint
)
{
    stitch_update_batch_t *batch = NULL;
    size_t i;
    int is_new_batch = 0;
    int rc;

    if (state == NULL || aggregate == NULL || drop_key == NULL || fingerprint == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (aggregate->source_event_count >= STITCH_MAX_SOURCE_EVENTS) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < state->update_batch_count; i++) {
        if (state->update_batches[i].aggregate == aggregate &&
            strcmp(state->update_batches[i].drop_key, drop_key) == 0) {
            batch = &state->update_batches[i];
            break;
        }
    }

    if (batch == NULL) {
        if (state->update_batch_count >= STITCH_MAX_UPDATE_BATCHES) {
            return X12_ERR_NO_MEMORY;
        }
        batch = &state->update_batches[state->update_batch_count++];
        memset(batch, 0, sizeof(*batch));
        batch->aggregate = aggregate;
        copy_cstr(batch->drop_key, sizeof(batch->drop_key), drop_key);
        copy_cstr(batch->source_drop_id, sizeof(batch->source_drop_id), state->current_source_drop_id);
        batch->first_source_event_index = aggregate->source_event_count;
        aggregate->version++;
        is_new_batch = 1;
    }

    rc = append_source_event(aggregate, event_id, fingerprint);
    if (rc != X12_OK) {
        if (is_new_batch) {
            state->update_batch_count--;
            aggregate->version--;
        }
        return rc;
    }

    batch->source_event_count++;
    batch->updated_by_event_id = event_id;
    copy_cstr(batch->updated_by_event_type, sizeof(batch->updated_by_event_type), event_type);

    return X12_OK;
}

static int set_current_source_drop(stitch_state_t *state, const char *drop_key)
{
    const char *separator;
    size_t drop_type_len;
    char source_type[32];
    int written;
    int rc;

    if (state == NULL || drop_key == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    copy_cstr(state->current_drop_key, sizeof(state->current_drop_key), drop_key);
    state->source_drop_count++;

    separator = strchr(drop_key, '|');
    drop_type_len = separator == NULL ? strlen(drop_key) : (size_t)(separator - drop_key);
    if (drop_type_len == 0u) {
        written = snprintf(
            state->current_source_drop_id,
            sizeof(state->current_source_drop_id),
            "source:%zu",
            state->source_drop_count
        );
    } else {
        written = snprintf(
            state->current_source_drop_id,
            sizeof(state->current_source_drop_id),
            "%.*s:%zu",
            (int)drop_type_len,
            drop_key,
            state->source_drop_count
        );
    }
    if (written < 0 || (size_t)written >= sizeof(state->current_source_drop_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    if (state->read_store != NULL) {
        if (drop_type_len >= sizeof(source_type)) {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(source_type, drop_key, drop_type_len);
        source_type[drop_type_len] = '\0';
        rc = scribe_store_put_source_drop(
            state->read_store,
            state->current_source_drop_id,
            source_type,
            "",
            ""
        );
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int put_event_key_if_present(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    const char *event_id
)
{
    if (key_value == NULL || key_value[0] == '\0') {
        return X12_OK;
    }
    return scribe_store_put_event_key(store, key_type, key_value, event_id);
}

static int index_journal_event(
    stitch_state_t *state,
    const char *journal_line,
    size_t numeric_event_id,
    long long event_offset
)
{
    char event_id[32];
    char event_type[96];
    char segment_id[32];
    char claim_id[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_control[STITCH_ID_MAX];
    char payer_control_token[TOKENISE_MAX_TOKEN_LEN];
    char encounter_id[STITCH_ID_MAX];
    const char *claim_index_key;
    const char *payer_index_key;
    int written;
    int rc;

    if (state == NULL || state->read_store == NULL) {
        return X12_OK;
    }
    if (state->current_source_drop_id[0] == '\0') {
        return X12_OK;
    }
    if (!json_get_string(journal_line, "event_type", event_type, sizeof(event_type))) {
        return X12_OK;
    }

    written = snprintf(event_id, sizeof(event_id), "%zu", numeric_event_id);
    if (written < 0 || (size_t)written >= sizeof(event_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    if (!json_get_number_text(journal_line, "source_segment_index", segment_id, sizeof(segment_id))) {
        written = snprintf(segment_id, sizeof(segment_id), "%zu", numeric_event_id);
        if (written < 0 || (size_t)written >= sizeof(segment_id)) {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
    }

    rc = scribe_store_put_event(
        state->read_store,
        event_id,
        state->current_source_drop_id,
        event_type,
        segment_id,
        event_offset,
        (long long)strlen(journal_line),
        ""
    );
    if (rc != X12_OK) {
        return rc;
    }

    extract_claim_keys(
        journal_line,
        claim_id,
        sizeof(claim_id),
        claim_id_token,
        sizeof(claim_id_token)
    );
    claim_index_key = claim_key(claim_id, claim_id_token);
    rc = put_event_key_if_present(state->read_store, "claim_id", claim_index_key, event_id);
    if (rc != X12_OK) {
        return rc;
    }
    if (state->include_phi) {
        rc = put_event_key_if_present(state->read_store, "claim_id_raw", claim_id, event_id);
        if (rc != X12_OK) {
            return rc;
        }
    }

    payer_control[0] = '\0';
    payer_control_token[0] = '\0';
    (void)json_get_string(
        journal_line,
        "payer_claim_control_number",
        payer_control,
        sizeof(payer_control)
    );
    (void)json_get_string(
        journal_line,
        "payer_claim_control_number_token",
        payer_control_token,
        sizeof(payer_control_token)
    );
    payer_index_key = payer_control_token[0] != '\0' ? payer_control_token : payer_control;
    rc = put_event_key_if_present(
        state->read_store,
        "payer_claim_control_number",
        payer_index_key,
        event_id
    );
    if (rc != X12_OK) {
        return rc;
    }
    if (state->include_phi) {
        rc = put_event_key_if_present(
            state->read_store,
            "payer_claim_control_number_raw",
            payer_control,
            event_id
        );
        if (rc != X12_OK) {
            return rc;
        }
    }

    encounter_id[0] = '\0';
    (void)json_get_string(journal_line, "encounter_id", encounter_id, sizeof(encounter_id));
    return put_event_key_if_present(state->read_store, "encounter_id", encounter_id, event_id);
}

static int flush_update_batches(stitch_state_t *state)
{
    size_t i;
    int rc;

    if (state == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    for (i = 0u; i < state->update_batch_count; i++) {
        if (state->update_batches[i].source_event_count == 0u) {
            continue;
        }
        rc = write_snapshot(state, &state->update_batches[i]);
        if (rc != X12_OK) {
            return rc;
        }
    }

    state->update_batch_count = 0u;
    return X12_OK;
}

static int apply_claim_event(
    stitch_state_t *state,
    const char *journal_line,
    size_t event_id,
    const char *drop_key
)
{
    char event_type[96];
    char claim_id[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char fingerprint[STITCH_FINGERPRINT_MAX];
    char value[STITCH_VALUE_MAX];
    const char *key;
    claim_aggregate_t *aggregate;
    int has_encounter_id;
    int rc;

    if (!json_get_string(journal_line, "event_type", event_type, sizeof(event_type))) {
        return X12_OK;
    }
    if (strcmp(event_type, "EncounterObserved") == 0) {
        return X12_OK;
    }
    has_encounter_id = event_has_encounter_id(journal_line);
    if (!event_matches_filter(state, journal_line)) {
        return X12_OK;
    }

    extract_claim_keys(
        journal_line,
        claim_id,
        sizeof(claim_id),
        claim_id_token,
        sizeof(claim_id_token)
    );
    key = claim_key(claim_id, claim_id_token);
    if (key == NULL || key[0] == '\0') {
        return X12_OK;
    }

    aggregate = find_aggregate(state, key);
    if (aggregate == NULL && state->encounter_filter[0] != '\0' && !has_encounter_id) {
        return X12_OK;
    }
    if (aggregate == NULL) {
        aggregate = find_or_add_aggregate(state, claim_id, claim_id_token);
    }
    if (aggregate == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = make_fingerprint(journal_line, event_type, aggregate->key, fingerprint, sizeof(fingerprint));
    if (rc != X12_OK) {
        return rc;
    }
    if (aggregate_has_fingerprint(aggregate, fingerprint)) {
        return X12_OK;
    }

    if (strcmp(event_type, "ChargeTransactionObserved") == 0) {
        aggregate->has_charge_context = 1;
        if (json_get_string(journal_line, "encounter_id", value, sizeof(value))) {
            copy_cstr(aggregate->encounter_id, sizeof(aggregate->encounter_id), value);
        }
        if (json_get_string(journal_line, "claim_type", value, sizeof(value))) {
            copy_cstr(aggregate->claim_type, sizeof(aggregate->claim_type), value);
        }
    } else if (strcmp(event_type, "ClaimObserved") == 0 ||
               strcmp(event_type, "ClaimReferencedBillingProvider") == 0 ||
               strcmp(event_type, "ClaimReferencedSubscriber") == 0 ||
               strcmp(event_type, "ClaimReferencedPatient") == 0 ||
               strcmp(event_type, "ClaimReferencedRenderingProvider") == 0 ||
               strcmp(event_type, "ClaimDateRecorded") == 0 ||
               strcmp(event_type, "ClaimLineDateRecorded") == 0 ||
               strcmp(event_type, "ClaimDiagnosesRecorded") == 0 ||
               strcmp(event_type, "ClaimServiceLineRecorded") == 0) {
        aggregate->has_837 = 1;
        if (strcmp(event_type, "ClaimServiceLineRecorded") == 0) {
            aggregate->submitted_service_line_count++;
        }
    } else if (strcmp(event_type, "RemittanceClaimPaymentObserved") == 0 ||
               strcmp(event_type, "RemittanceClaimReferencedPatient") == 0 ||
               strcmp(event_type, "RemittanceClaimReferencedSubscriber") == 0 ||
               strcmp(event_type, "RemittanceServiceLinePaymentObserved") == 0 ||
               strcmp(event_type, "RemittanceAdjustmentObserved") == 0 ||
               strcmp(event_type, "RemittanceDateRecorded") == 0) {
        aggregate->has_835 = 1;
        if (json_get_string(journal_line, "claim_status_code", value, sizeof(value))) {
            copy_cstr(aggregate->claim_status_code, sizeof(aggregate->claim_status_code), value);
        }
        if (json_get_string(journal_line, "payer_claim_control_number", value, sizeof(value))) {
            copy_cstr(
                aggregate->payer_claim_control_number,
                sizeof(aggregate->payer_claim_control_number),
                value
            );
        }
        if (json_get_string(journal_line, "payer_claim_control_number_token", value, sizeof(value))) {
            copy_cstr(
                aggregate->payer_claim_control_number_token,
                sizeof(aggregate->payer_claim_control_number_token),
                value
            );
        }
        if (strcmp(event_type, "RemittanceServiceLinePaymentObserved") == 0) {
            aggregate->remittance_service_line_count++;
        } else if (strcmp(event_type, "RemittanceAdjustmentObserved") == 0) {
            aggregate->adjustment_count++;
        }
    } else if (strcmp(event_type, "PatientPaymentObserved") == 0 ||
               strcmp(event_type, "WriteoffObserved") == 0 ||
               strcmp(event_type, "RefundObserved") == 0) {
        aggregate->has_charge_context = 1;
    } else {
        return X12_OK;
    }

    return record_batch_update(state, aggregate, drop_key, event_id, event_type, fingerprint);
}

static int discover_encounter_claim(
    stitch_state_t *state,
    const char *journal_line
)
{
    char event_type[96];
    char claim_id[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char value[STITCH_VALUE_MAX];
    claim_aggregate_t *aggregate;

    if (state->encounter_filter[0] == '\0') {
        return X12_OK;
    }
    if (!json_get_string(journal_line, "event_type", event_type, sizeof(event_type)) ||
        strcmp(event_type, "ChargeTransactionObserved") != 0 ||
        !event_matches_filter(state, journal_line)) {
        return X12_OK;
    }

    extract_claim_keys(
        journal_line,
        claim_id,
        sizeof(claim_id),
        claim_id_token,
        sizeof(claim_id_token)
    );
    if (claim_key(claim_id, claim_id_token)[0] == '\0') {
        return X12_OK;
    }

    aggregate = find_or_add_aggregate(state, claim_id, claim_id_token);
    if (aggregate == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    if (json_get_string(journal_line, "encounter_id", value, sizeof(value))) {
        copy_cstr(aggregate->encounter_id, sizeof(aggregate->encounter_id), value);
    }
    if (json_get_string(journal_line, "claim_type", value, sizeof(value))) {
        copy_cstr(aggregate->claim_type, sizeof(aggregate->claim_type), value);
    }

    return X12_OK;
}

static int read_journal_for_discovery(stitch_state_t *state, const char *path)
{
    FILE *journal;
    char line[STITCH_LINE_MAX];
    int rc = X12_OK;

    if (state->encounter_filter[0] == '\0') {
        return X12_OK;
    }

    journal = fopen(path, "rb");
    if (journal == NULL) {
        return X12_ERR_IO;
    }

    while (fgets(line, sizeof(line), journal) != NULL) {
        rc = discover_encounter_claim(state, line);
        if (rc != X12_OK) {
            break;
        }
    }
    if (ferror(journal) && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (fclose(journal) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }

    return rc;
}

void aggregate_stitcher_input_init(aggregate_stitcher_input_t *input)
{
    if (input != NULL) {
        memset(input, 0, sizeof(*input));
    }
}

int aggregate_stitcher_stitch(const aggregate_stitcher_input_t *input)
{
    stitch_state_t *state;
    scribe_store_t read_store;
    FILE *journal;
    FILE *out;
    char line[STITCH_LINE_MAX];
    char drop_key[STITCH_FINGERPRINT_MAX];
    size_t event_id = 0u;
    int owns_out = 0;
    int owns_read_store = 0;
    int rc = X12_OK;

    if (input == NULL || input->journal_path == NULL || input->out_path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    journal = fopen(input->journal_path, "rb");
    if (journal == NULL) {
        return X12_ERR_IO;
    }

    if (strcmp(input->out_path, "-") == 0) {
        out = stdout;
    } else {
        out = fopen(input->out_path, "wb");
        owns_out = 1;
    }
    if (out == NULL) {
        (void)fclose(journal);
        return X12_ERR_IO;
    }

    state = (stitch_state_t *)calloc(1u, sizeof(*state));
    if (state == NULL) {
        (void)fclose(journal);
        if (owns_out) {
            (void)fclose(out);
        }
        return X12_ERR_NO_MEMORY;
    }

    state->out = out;
    state->include_phi = input->include_phi;
    if (input->encounter_id != NULL) {
        copy_cstr(state->encounter_filter, sizeof(state->encounter_filter), input->encounter_id);
    }
    if (input->read_store_path != NULL) {
        scribe_store_init(&read_store);
        rc = scribe_store_open(&read_store, input->read_store_path);
        if (rc == X12_OK) {
            rc = scribe_store_init_schema(&read_store);
        }
        if (rc != X12_OK) {
            (void)scribe_store_close(&read_store);
            (void)fclose(journal);
            if (owns_out) {
                (void)fclose(out);
            }
            free(state);
            return rc;
        }
        state->read_store = &read_store;
        owns_read_store = 1;
    }

    rc = read_journal_for_discovery(state, input->journal_path);
    if (rc != X12_OK) {
        (void)fclose(journal);
        if (owns_out) {
            (void)fclose(out);
        }
        if (owns_read_store) {
            (void)scribe_store_close(&read_store);
        }
        free(state);
        return rc;
    }

    while (1) {
        long event_offset = ftell(journal);

        if (event_offset < 0) {
            rc = X12_ERR_IO;
            break;
        }
        if (fgets(line, sizeof(line), journal) == NULL) {
            break;
        }
        event_id++;
        rc = make_drop_key(line, drop_key, sizeof(drop_key));
        if (rc != X12_OK) {
            break;
        }
        if (drop_key[0] != '\0' && strcmp(state->current_drop_key, drop_key) != 0) {
            rc = flush_update_batches(state);
            if (rc != X12_OK) {
                break;
            }
            rc = set_current_source_drop(state, drop_key);
            if (rc != X12_OK) {
                break;
            }
        }

        rc = index_journal_event(state, line, event_id, (long long)event_offset);
        if (rc != X12_OK) {
            break;
        }

        rc = apply_claim_event(state, line, event_id, drop_key);
        if (rc != X12_OK) {
            break;
        }
    }

    if (rc == X12_OK) {
        rc = flush_update_batches(state);
    }

    if (ferror(journal) && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (fclose(journal) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (fflush(out) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (owns_out && fclose(out) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (owns_read_store && scribe_store_close(&read_store) != X12_OK && rc == X12_OK) {
        rc = X12_ERR_IO;
    }

    free(state);
    return rc;
}

int aggregate_stitcher_run_cli(int argc, char **argv)
{
    aggregate_stitcher_input_t input;
    int i;

    aggregate_stitcher_input_init(&input);
    input.out_path = "-";

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--journal") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.journal_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.out_path = argv[++i];
        } else if (strcmp(argv[i], "--read-store") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.read_store_path = argv[++i];
        } else if (strcmp(argv[i], "--encounter-id") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.encounter_id = argv[++i];
        } else if (strcmp(argv[i], "--include-phi") == 0) {
            input.include_phi = 1;
        } else {
            return -1;
        }
    }

    if (input.journal_path == NULL) {
        return -1;
    }

    return aggregate_stitcher_stitch(&input);
}
