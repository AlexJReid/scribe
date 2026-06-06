#include "aggregate_stitcher.h"

#include "event_writer.h"
#include "journal.h"
#include "json_write.h"
#include "phi_vault.h"
#include "run_id.h"
#include "store.h"
#include "tokenise.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define json_get_string journal_event_get_string
#define json_get_number_text journal_event_get_number_text
#define json_get_array_string_at journal_event_get_array_string_at

#define STITCH_LINE_MAX 8192u
#define STITCH_ID_MAX 128u
#define STITCH_VALUE_MAX 256u
#define STITCH_FINGERPRINT_MAX 384u
#define STITCH_MAX_AGGREGATES 128u
#define STITCH_MAX_SOURCE_EVENTS 512u
#define STITCH_MAX_UPDATE_BATCHES 128u
#define STITCH_MAX_ENCOUNTERS 64u
#define STITCH_MAX_LINES_PER_CLAIM 64u
#define STITCH_MAX_ADJUSTMENTS_PER_LINE 8u
#define STITCH_MAX_ADJUSTMENT_VALUES 8u
#define STITCH_STATE_JSON_MAX 131072u

typedef struct {
    char adjustment_group_code[32];
    char reason_codes[STITCH_MAX_ADJUSTMENT_VALUES][32];
    char amounts[STITCH_MAX_ADJUSTMENT_VALUES][32];
    char quantities[STITCH_MAX_ADJUSTMENT_VALUES][32];
    size_t value_count;
} stitched_line_adjustment_t;

typedef struct {
    char service_line_number[32];
    char remit_service_line_number[32];
    char procedure_code[32];
    char description[STITCH_VALUE_MAX];
    char service_date[32];
    char match_method[48];
    int has_charge_context;
    char charge_amount[32];
    char charge_service_date[32];
    int has_submitted;
    char submitted_line_type[16];
    char submitted_procedure_code_qualifier[16];
    char submitted_procedure_code_set[32];
    char submitted_charge_amount[32];
    char submitted_unit_measure_code[16];
    char submitted_unit_count[32];
    char submitted_service_date[32];
    int has_remittance;
    char remittance_procedure_code_qualifier[16];
    char remittance_procedure_code_set[32];
    char remittance_line_charge_amount[32];
    char remittance_line_paid_amount[32];
    char remittance_paid_unit_count[32];
    char remittance_service_date[32];
    stitched_line_adjustment_t adjustments[STITCH_MAX_ADJUSTMENTS_PER_LINE];
    size_t adjustment_count;
} stitched_service_line_t;

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
    char patient_id[STITCH_ID_MAX];
    char patient_id_token[TOKENISE_MAX_TOKEN_LEN];
    char patient_name[STITCH_VALUE_MAX];
    char patient_name_token[TOKENISE_MAX_TOKEN_LEN];
    char claim_type[64];
    char claim_status_code[32];
    size_t version;
    int has_charge_context;
    int has_837;
    int has_835;
    size_t submitted_service_line_count;
    size_t remittance_service_line_count;
    size_t adjustment_count;
    stitched_service_line_t service_lines[STITCH_MAX_LINES_PER_CLAIM];
    size_t service_line_count;
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
    long long updated_by_journal_offset;
    long long updated_by_journal_length;
    char updated_by_event_type[96];
    char source_run_id[STITCH_ID_MAX];
} stitch_update_batch_t;

typedef struct {
    char encounter_id[STITCH_ID_MAX];
    char patient_id[STITCH_ID_MAX];
    char patient_id_token[TOKENISE_MAX_TOKEN_LEN];
    char patient_name[STITCH_VALUE_MAX];
    char patient_name_token[TOKENISE_MAX_TOKEN_LEN];
} stitch_encounter_context_t;

typedef struct {
    claim_aggregate_t aggregates[STITCH_MAX_AGGREGATES];
    size_t aggregate_count;
    stitch_update_batch_t update_batches[STITCH_MAX_UPDATE_BATCHES];
    size_t update_batch_count;
    stitch_encounter_context_t encounter_contexts[STITCH_MAX_ENCOUNTERS];
    size_t encounter_context_count;
    char current_drop_key[STITCH_FINGERPRINT_MAX];
    char current_source_drop_id[STITCH_VALUE_MAX];
    char current_source_run_id[STITCH_ID_MAX];
    size_t source_drop_count;
    char run_id[STITCH_ID_MAX];
    char encounter_filter[STITCH_ID_MAX];
    int include_phi;
    FILE *out;
    FILE *notify_out;
    scribe_store_t *read_store;
    phi_vault_t *phi_vault;
} stitch_state_t;

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

static void copy_str_slice(char *out, size_t out_len, const char *value, size_t value_len)
{
    if (out == NULL || out_len == 0u) {
        return;
    }
    if (value == NULL) {
        value = "";
        value_len = 0u;
    }
    if (value_len >= out_len) {
        value_len = out_len - 1u;
    }
    memcpy(out, value, value_len);
    out[value_len] = '\0';
}

static const char *claim_key(const char *claim_id, const char *claim_id_token)
{
    if (claim_id_token != NULL && claim_id_token[0] != '\0') {
        return claim_id_token;
    }

    return claim_id;
}

static void extract_claim_keys(
    const journal_event_t *journal_line,
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

static int tokenise_cstring(token_type_t type, const char *value, char *out, size_t out_len)
{
    x12_str_t raw;

    if (value == NULL || value[0] == '\0') {
        if (out != NULL && out_len > 0u) {
            out[0] = '\0';
        }
        return X12_OK;
    }

    raw.ptr = (char *)value;
    raw.len = strlen(value);
    return tokenise_value(type, raw, out, out_len);
}

static int resolve_phi_token(
    const stitch_state_t *state,
    const char *namespace_name,
    const char *token,
    char *out,
    size_t out_len
)
{
    int rc;

    if (out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    if (state == NULL || !state->include_phi || state->phi_vault == NULL ||
        namespace_name == NULL || token == NULL || token[0] == '\0') {
        return X12_OK;
    }

    rc = phi_vault_resolve(
        state->phi_vault,
        namespace_name,
        token,
        "stitch",
        "aggregate",
        out,
        out_len
    );
    if (rc == X12_ERR_NOT_FOUND) {
        out[0] = '\0';
        return X12_OK;
    }
    return rc;
}

static int extract_value_token_pair(
    const stitch_state_t *state,
    const journal_event_t *journal_line,
    const char *value_field,
    const char *token_field,
    const char *namespace_name,
    char *raw_out,
    size_t raw_out_len,
    char *token_out,
    size_t token_out_len
)
{
    char value[STITCH_VALUE_MAX];
    char token[TOKENISE_MAX_TOKEN_LEN];
    int has_value;
    int has_token;
    int rc;

    if (raw_out == NULL || raw_out_len == 0u || token_out == NULL || token_out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    raw_out[0] = '\0';
    token_out[0] = '\0';

    has_value = json_get_string(journal_line, value_field, value, sizeof(value));
    has_token = json_get_string(journal_line, token_field, token, sizeof(token));
    if (!has_value && !has_token) {
        return X12_OK;
    }

    if (has_token) {
        copy_cstr(token_out, token_out_len, token);
        if (has_value && state != NULL && state->include_phi) {
            copy_cstr(raw_out, raw_out_len, value);
        } else {
            rc = resolve_phi_token(state, namespace_name, token, raw_out, raw_out_len);
            if (rc != X12_OK) {
                return rc;
            }
        }
        return X12_OK;
    }

    copy_cstr(token_out, token_out_len, value);
    return resolve_phi_token(state, namespace_name, value, raw_out, raw_out_len);
}

static int make_patient_name(
    const char *last_name_or_org,
    const char *first_name,
    char *out,
    size_t out_len
)
{
    int written;

    if (out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    if (last_name_or_org == NULL || last_name_or_org[0] == '\0') {
        return X12_OK;
    }

    if (first_name != NULL && first_name[0] != '\0') {
        written = snprintf(out, out_len, "%s|%s", last_name_or_org, first_name);
    } else {
        written = snprintf(out, out_len, "%s", last_name_or_org);
    }
    if (written < 0 || (size_t)written >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return X12_OK;
}

static void split_patient_name(
    const char *raw_name,
    char *last_name_or_org,
    size_t last_name_or_org_len,
    char *first_name,
    size_t first_name_len
)
{
    const char *sep;

    if (last_name_or_org != NULL && last_name_or_org_len > 0u) {
        last_name_or_org[0] = '\0';
    }
    if (first_name != NULL && first_name_len > 0u) {
        first_name[0] = '\0';
    }
    if (raw_name == NULL || raw_name[0] == '\0') {
        return;
    }

    sep = strchr(raw_name, '|');
    if (sep == NULL) {
        copy_cstr(last_name_or_org, last_name_or_org_len, raw_name);
        return;
    }

    copy_str_slice(last_name_or_org, last_name_or_org_len, raw_name, (size_t)(sep - raw_name));
    copy_cstr(first_name, first_name_len, sep + 1);
}

static int set_patient_name(
    const stitch_state_t *state,
    char *patient_name,
    size_t patient_name_len,
    char *patient_name_token,
    size_t patient_name_token_len,
    const char *raw_name,
    int overwrite
)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    int rc;

    if (raw_name == NULL || raw_name[0] == '\0') {
        return X12_OK;
    }
    if (!overwrite && patient_name_token != NULL && patient_name_token[0] != '\0') {
        return X12_OK;
    }

    rc = tokenise_cstring(TOK_PATIENT_NAME, raw_name, token, sizeof(token));
    if (rc != X12_OK) {
        return rc;
    }
    if (patient_name_token != NULL && patient_name_token_len > 0u) {
        copy_cstr(patient_name_token, patient_name_token_len, token);
    }
    if (state != NULL && state->include_phi && patient_name != NULL && patient_name_len > 0u) {
        copy_cstr(patient_name, patient_name_len, raw_name);
    }

    return X12_OK;
}

static int set_patient_id(
    const stitch_state_t *state,
    char *patient_id,
    size_t patient_id_len,
    char *patient_id_token,
    size_t patient_id_token_len,
    const char *raw_id,
    const char *token,
    int overwrite
)
{
    if (!overwrite && patient_id_token != NULL && patient_id_token[0] != '\0') {
        return X12_OK;
    }
    if (patient_id_token != NULL && patient_id_token_len > 0u && token != NULL && token[0] != '\0') {
        copy_cstr(patient_id_token, patient_id_token_len, token);
    }
    if (state != NULL && state->include_phi &&
        patient_id != NULL && patient_id_len > 0u && raw_id != NULL && raw_id[0] != '\0') {
        copy_cstr(patient_id, patient_id_len, raw_id);
    }

    return X12_OK;
}

static stitch_encounter_context_t *find_or_add_encounter_context(
    stitch_state_t *state,
    const char *encounter_id
)
{
    stitch_encounter_context_t *context;
    size_t i;

    if (state == NULL || encounter_id == NULL || encounter_id[0] == '\0') {
        return NULL;
    }
    for (i = 0u; i < state->encounter_context_count; i++) {
        if (strcmp(state->encounter_contexts[i].encounter_id, encounter_id) == 0) {
            return &state->encounter_contexts[i];
        }
    }
    if (state->encounter_context_count >= STITCH_MAX_ENCOUNTERS) {
        return NULL;
    }

    context = &state->encounter_contexts[state->encounter_context_count++];
    memset(context, 0, sizeof(*context));
    copy_cstr(context->encounter_id, sizeof(context->encounter_id), encounter_id);
    return context;
}

static stitch_encounter_context_t *find_encounter_context(
    stitch_state_t *state,
    const char *encounter_id
)
{
    size_t i;

    if (state == NULL || encounter_id == NULL || encounter_id[0] == '\0') {
        return NULL;
    }
    for (i = 0u; i < state->encounter_context_count; i++) {
        if (strcmp(state->encounter_contexts[i].encounter_id, encounter_id) == 0) {
            return &state->encounter_contexts[i];
        }
    }
    return NULL;
}

static int resolve_patient_name_by_id(
    const stitch_state_t *state,
    const char *id_name_namespace,
    const char *id_token,
    char *patient_name,
    size_t patient_name_len,
    char *patient_name_token,
    size_t patient_name_token_len,
    int overwrite
)
{
    char raw_name[STITCH_VALUE_MAX];
    int rc;

    if (!overwrite && patient_name_token != NULL && patient_name_token[0] != '\0') {
        return X12_OK;
    }
    rc = resolve_phi_token(state, id_name_namespace, id_token, raw_name, sizeof(raw_name));
    if (rc != X12_OK) {
        return rc;
    }
    return set_patient_name(
        state,
        patient_name,
        patient_name_len,
        patient_name_token,
        patient_name_token_len,
        raw_name,
        overwrite
    );
}

static int capture_encounter_context(
    stitch_state_t *state,
    const journal_event_t *journal_line
)
{
    char event_type[96];
    char encounter_id[STITCH_ID_MAX];
    char patient_id[STITCH_ID_MAX];
    char patient_id_token[TOKENISE_MAX_TOKEN_LEN];
    stitch_encounter_context_t *context;
    int rc;

    if (state == NULL || journal_line == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (!json_get_string(journal_line, "event_type", event_type, sizeof(event_type)) ||
        strcmp(event_type, "EncounterObserved") != 0 ||
        !json_get_string(journal_line, "encounter_id", encounter_id, sizeof(encounter_id))) {
        return X12_OK;
    }

    context = find_or_add_encounter_context(state, encounter_id);
    if (context == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = extract_value_token_pair(
        state,
        journal_line,
        "patient_id",
        "patient_id_token",
        "patient_id",
        patient_id,
        sizeof(patient_id),
        patient_id_token,
        sizeof(patient_id_token)
    );
    if (rc != X12_OK) {
        return rc;
    }
    rc = set_patient_id(
        state,
        context->patient_id,
        sizeof(context->patient_id),
        context->patient_id_token,
        sizeof(context->patient_id_token),
        patient_id,
        patient_id_token,
        1
    );
    if (rc != X12_OK) {
        return rc;
    }
    return resolve_patient_name_by_id(
        state,
        "patient_id_name",
        context->patient_id_token,
        context->patient_name,
        sizeof(context->patient_name),
        context->patient_name_token,
        sizeof(context->patient_name_token),
        1
    );
}

static int apply_encounter_context_to_aggregate(
    stitch_state_t *state,
    claim_aggregate_t *aggregate
)
{
    stitch_encounter_context_t *context;
    int rc;

    if (state == NULL || aggregate == NULL || aggregate->encounter_id[0] == '\0') {
        return X12_OK;
    }
    context = find_encounter_context(state, aggregate->encounter_id);
    if (context == NULL) {
        return X12_OK;
    }

    rc = set_patient_id(
        state,
        aggregate->patient_id,
        sizeof(aggregate->patient_id),
        aggregate->patient_id_token,
        sizeof(aggregate->patient_id_token),
        context->patient_id,
        context->patient_id_token,
        0
    );
    if (rc != X12_OK) {
        return rc;
    }
    return set_patient_name(
        state,
        aggregate->patient_name,
        sizeof(aggregate->patient_name),
        aggregate->patient_name_token,
        sizeof(aggregate->patient_name_token),
        context->patient_name,
        0
    );
}

static int capture_reference_patient_context(
    stitch_state_t *state,
    claim_aggregate_t *aggregate,
    const journal_event_t *journal_line,
    const char *event_type
)
{
    const char *id_namespace;
    const char *id_name_namespace;
    int overwrite;
    char raw_id[STITCH_ID_MAX];
    char id_token[TOKENISE_MAX_TOKEN_LEN];
    char last_name_or_org[STITCH_VALUE_MAX];
    char first_name[STITCH_VALUE_MAX];
    char raw_name[STITCH_VALUE_MAX];
    int rc;

    if (state == NULL || aggregate == NULL || journal_line == NULL || event_type == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (strcmp(event_type, "ClaimReferencedPatient") == 0 ||
        strcmp(event_type, "RemittanceClaimReferencedPatient") == 0) {
        id_namespace = "patient_id";
        id_name_namespace = "patient_id_name";
        overwrite = 1;
    } else if (strcmp(event_type, "ClaimReferencedSubscriber") == 0 ||
               strcmp(event_type, "RemittanceClaimReferencedSubscriber") == 0) {
        id_namespace = "member_id";
        id_name_namespace = "member_id_name";
        overwrite = 0;
    } else {
        return X12_OK;
    }

    rc = extract_value_token_pair(
        state,
        journal_line,
        "id_value",
        "id_value_token",
        id_namespace,
        raw_id,
        sizeof(raw_id),
        id_token,
        sizeof(id_token)
    );
    if (rc != X12_OK) {
        return rc;
    }
    if (strcmp(id_namespace, "patient_id") == 0) {
        rc = set_patient_id(
            state,
            aggregate->patient_id,
            sizeof(aggregate->patient_id),
            aggregate->patient_id_token,
            sizeof(aggregate->patient_id_token),
            raw_id,
            id_token,
            overwrite
        );
        if (rc != X12_OK) {
            return rc;
        }
    }

    last_name_or_org[0] = '\0';
    first_name[0] = '\0';
    raw_name[0] = '\0';
    (void)json_get_string(journal_line, "last_name_or_org", last_name_or_org, sizeof(last_name_or_org));
    (void)json_get_string(journal_line, "first_name", first_name, sizeof(first_name));
    rc = make_patient_name(last_name_or_org, first_name, raw_name, sizeof(raw_name));
    if (rc != X12_OK) {
        return rc;
    }
    rc = set_patient_name(
        state,
        aggregate->patient_name,
        sizeof(aggregate->patient_name),
        aggregate->patient_name_token,
        sizeof(aggregate->patient_name_token),
        raw_name,
        overwrite
    );
    if (rc != X12_OK) {
        return rc;
    }

    return resolve_patient_name_by_id(
        state,
        id_name_namespace,
        id_token,
        aggregate->patient_name,
        sizeof(aggregate->patient_name),
        aggregate->patient_name_token,
        sizeof(aggregate->patient_name_token),
        overwrite
    );
}

static int resolve_identifier_output_pair(
    const stitch_state_t *state,
    const char *namespace_name,
    const char *raw_value,
    const char *token_value,
    char *raw_out,
    size_t raw_out_len,
    char *token_out,
    size_t token_out_len
)
{
    int rc;

    if (raw_out == NULL || raw_out_len == 0u || token_out == NULL || token_out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    raw_out[0] = '\0';
    token_out[0] = '\0';

    if (token_value != NULL && token_value[0] != '\0') {
        copy_cstr(token_out, token_out_len, token_value);
        if (state != NULL && state->include_phi && raw_value != NULL && raw_value[0] != '\0') {
            copy_cstr(raw_out, raw_out_len, raw_value);
        } else {
            rc = resolve_phi_token(state, namespace_name, token_value, raw_out, raw_out_len);
            if (rc != X12_OK) {
                return rc;
            }
        }
        return X12_OK;
    }

    if (raw_value == NULL || raw_value[0] == '\0') {
        return X12_OK;
    }
    if (state != NULL && state->include_phi) {
        rc = resolve_phi_token(state, namespace_name, raw_value, raw_out, raw_out_len);
        if (rc != X12_OK) {
            return rc;
        }
        if (raw_out[0] == '\0') {
            copy_cstr(raw_out, raw_out_len, raw_value);
        } else {
            copy_cstr(token_out, token_out_len, raw_value);
        }
    } else {
        copy_cstr(token_out, token_out_len, raw_value);
    }

    return X12_OK;
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

static stitched_service_line_t *add_service_line(claim_aggregate_t *aggregate)
{
    stitched_service_line_t *line;

    if (aggregate == NULL || aggregate->service_line_count >= STITCH_MAX_LINES_PER_CLAIM) {
        return NULL;
    }

    line = &aggregate->service_lines[aggregate->service_line_count++];
    memset(line, 0, sizeof(*line));
    copy_cstr(line->match_method, sizeof(line->match_method), "unmatched");
    return line;
}

static stitched_service_line_t *find_service_line_by_number(
    claim_aggregate_t *aggregate,
    const char *service_line_number
)
{
    size_t i;

    if (aggregate == NULL || service_line_number == NULL || service_line_number[0] == '\0') {
        return NULL;
    }

    for (i = 0u; i < aggregate->service_line_count; i++) {
        if (strcmp(aggregate->service_lines[i].service_line_number, service_line_number) == 0 ||
            strcmp(aggregate->service_lines[i].remit_service_line_number, service_line_number) == 0) {
            return &aggregate->service_lines[i];
        }
    }

    return NULL;
}

static stitched_service_line_t *find_or_add_service_line_by_number(
    claim_aggregate_t *aggregate,
    const char *service_line_number
)
{
    stitched_service_line_t *line = find_service_line_by_number(aggregate, service_line_number);

    if (line != NULL) {
        return line;
    }

    line = add_service_line(aggregate);
    if (line != NULL) {
        copy_cstr(line->service_line_number, sizeof(line->service_line_number), service_line_number);
    }
    return line;
}

static const char *service_line_effective_charge(const stitched_service_line_t *line)
{
    if (line == NULL) {
        return "";
    }
    if (line->charge_amount[0] != '\0') {
        return line->charge_amount;
    }
    return line->submitted_charge_amount;
}

static stitched_service_line_t *find_service_line_by_procedure_charge(
    claim_aggregate_t *aggregate,
    const char *procedure_code,
    const char *charge_amount
)
{
    const char *line_charge;
    size_t i;

    if (aggregate == NULL || procedure_code == NULL || procedure_code[0] == '\0') {
        return NULL;
    }

    for (i = 0u; i < aggregate->service_line_count; i++) {
        if (strcmp(aggregate->service_lines[i].procedure_code, procedure_code) != 0) {
            continue;
        }
        line_charge = service_line_effective_charge(&aggregate->service_lines[i]);
        if (charge_amount == NULL || charge_amount[0] == '\0' ||
            line_charge[0] == '\0' || strcmp(line_charge, charge_amount) == 0) {
            return &aggregate->service_lines[i];
        }
    }

    return NULL;
}

static void update_procedure_code_if_present(stitched_service_line_t *line, const char *procedure_code)
{
    if (line != NULL && procedure_code != NULL && procedure_code[0] != '\0' &&
        line->procedure_code[0] == '\0') {
        copy_cstr(line->procedure_code, sizeof(line->procedure_code), procedure_code);
    }
}

static void update_service_line_date(stitched_service_line_t *line)
{
    const char *submitted_date = "";

    if (line == NULL) {
        return;
    }
    if (line->charge_service_date[0] != '\0') {
        submitted_date = line->charge_service_date;
    } else if (line->submitted_service_date[0] != '\0') {
        submitted_date = line->submitted_service_date;
    }

    if (line->service_date[0] == '\0') {
        if (submitted_date[0] != '\0') {
            copy_cstr(line->service_date, sizeof(line->service_date), submitted_date);
        } else if (line->remittance_service_date[0] != '\0') {
            copy_cstr(line->service_date, sizeof(line->service_date), line->remittance_service_date);
        }
    }
    if (submitted_date[0] != '\0' &&
        line->remittance_service_date[0] != '\0' &&
        strcmp(submitted_date, line->remittance_service_date) == 0 &&
        strcmp(line->match_method, "procedure_charge") == 0) {
        copy_cstr(line->match_method, sizeof(line->match_method), "procedure_charge_date");
    }
}

static int apply_charge_service_line(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    char line_no[32];
    char value[STITCH_VALUE_MAX];
    stitched_service_line_t *line;

    (void)json_get_string(journal_line, "service_line_number", line_no, sizeof(line_no));
    line = find_or_add_service_line_by_number(aggregate, line_no);
    if (line == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    line->has_charge_context = 1;
    if (json_get_string(journal_line, "procedure_code", value, sizeof(value))) {
        copy_cstr(line->procedure_code, sizeof(line->procedure_code), value);
    }
    if (json_get_string(journal_line, "description", value, sizeof(value))) {
        copy_cstr(line->description, sizeof(line->description), value);
    }
    if (json_get_string(journal_line, "service_date", value, sizeof(value))) {
        copy_cstr(line->charge_service_date, sizeof(line->charge_service_date), value);
        update_service_line_date(line);
    }
    if (json_get_string(journal_line, "amount", value, sizeof(value))) {
        copy_cstr(line->charge_amount, sizeof(line->charge_amount), value);
    }

    return X12_OK;
}

static int apply_submitted_service_line(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    char line_no[32];
    char value[STITCH_VALUE_MAX];
    stitched_service_line_t *line;

    (void)json_get_string(journal_line, "service_line_number", line_no, sizeof(line_no));
    line = find_or_add_service_line_by_number(aggregate, line_no);
    if (line == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    line->has_submitted = 1;
    if (json_get_string(journal_line, "line_type", value, sizeof(value))) {
        copy_cstr(line->submitted_line_type, sizeof(line->submitted_line_type), value);
    }
    if (json_get_string(journal_line, "procedure_code_qualifier", value, sizeof(value))) {
        copy_cstr(
            line->submitted_procedure_code_qualifier,
            sizeof(line->submitted_procedure_code_qualifier),
            value
        );
    }
    if (json_get_string(journal_line, "procedure_code_set", value, sizeof(value))) {
        copy_cstr(line->submitted_procedure_code_set, sizeof(line->submitted_procedure_code_set), value);
    }
    if (json_get_string(journal_line, "procedure_code", value, sizeof(value))) {
        copy_cstr(line->procedure_code, sizeof(line->procedure_code), value);
    }
    if (json_get_array_string_at(journal_line, "raw_elements", 1u, value, sizeof(value))) {
        copy_cstr(line->submitted_charge_amount, sizeof(line->submitted_charge_amount), value);
    }
    if (json_get_array_string_at(journal_line, "raw_elements", 2u, value, sizeof(value))) {
        copy_cstr(line->submitted_unit_measure_code, sizeof(line->submitted_unit_measure_code), value);
    }
    if (json_get_array_string_at(journal_line, "raw_elements", 3u, value, sizeof(value))) {
        copy_cstr(line->submitted_unit_count, sizeof(line->submitted_unit_count), value);
    }

    return X12_OK;
}

static int apply_submitted_line_date(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    char line_no[32];
    char value[STITCH_VALUE_MAX];
    stitched_service_line_t *line;

    if (!json_get_string(journal_line, "service_line_number", line_no, sizeof(line_no)) ||
        line_no[0] == '\0' ||
        !json_get_string(journal_line, "date_value", value, sizeof(value))) {
        return X12_OK;
    }

    line = find_service_line_by_number(aggregate, line_no);
    if (line == NULL) {
        return X12_OK;
    }

    copy_cstr(line->submitted_service_date, sizeof(line->submitted_service_date), value);
    update_service_line_date(line);
    return X12_OK;
}

static int apply_remittance_service_line(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    char remit_line_no[32];
    char procedure_code[32];
    char charge_amount[32];
    char value[STITCH_VALUE_MAX];
    stitched_service_line_t *line;

    (void)json_get_string(journal_line, "service_line_number", remit_line_no, sizeof(remit_line_no));
    (void)json_get_string(journal_line, "procedure_code", procedure_code, sizeof(procedure_code));
    (void)json_get_string(journal_line, "line_charge_amount", charge_amount, sizeof(charge_amount));

    line = find_service_line_by_procedure_charge(aggregate, procedure_code, charge_amount);
    if (line != NULL) {
        copy_cstr(line->match_method, sizeof(line->match_method), "procedure_charge");
    } else {
        line = find_service_line_by_number(aggregate, remit_line_no);
        if (line != NULL) {
            copy_cstr(line->match_method, sizeof(line->match_method), "line_order");
        }
    }
    if (line == NULL) {
        line = find_or_add_service_line_by_number(aggregate, remit_line_no);
        if (line == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        copy_cstr(line->match_method, sizeof(line->match_method), "created_from_remittance");
    }

    line->has_remittance = 1;
    copy_cstr(line->remit_service_line_number, sizeof(line->remit_service_line_number), remit_line_no);
    update_procedure_code_if_present(line, procedure_code);
    if (json_get_string(journal_line, "procedure_code_qualifier", value, sizeof(value))) {
        copy_cstr(
            line->remittance_procedure_code_qualifier,
            sizeof(line->remittance_procedure_code_qualifier),
            value
        );
    }
    if (json_get_string(journal_line, "procedure_code_set", value, sizeof(value))) {
        copy_cstr(line->remittance_procedure_code_set, sizeof(line->remittance_procedure_code_set), value);
    }
    copy_cstr(line->remittance_line_charge_amount, sizeof(line->remittance_line_charge_amount), charge_amount);
    if (json_get_string(journal_line, "line_paid_amount", value, sizeof(value))) {
        copy_cstr(line->remittance_line_paid_amount, sizeof(line->remittance_line_paid_amount), value);
    }
    if (json_get_string(journal_line, "paid_service_unit_count", value, sizeof(value))) {
        copy_cstr(line->remittance_paid_unit_count, sizeof(line->remittance_paid_unit_count), value);
    }
    update_service_line_date(line);

    return X12_OK;
}

static int apply_remittance_line_date(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    char remit_line_no[32];
    char value[STITCH_VALUE_MAX];
    stitched_service_line_t *line;

    if (!json_get_string(journal_line, "service_line_number", remit_line_no, sizeof(remit_line_no)) ||
        remit_line_no[0] == '\0' ||
        !json_get_string(journal_line, "date_value", value, sizeof(value))) {
        return X12_OK;
    }

    line = find_service_line_by_number(aggregate, remit_line_no);
    if (line == NULL) {
        return X12_OK;
    }

    copy_cstr(line->remittance_service_date, sizeof(line->remittance_service_date), value);
    update_service_line_date(line);
    return X12_OK;
}

static int apply_service_line_adjustment(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    char line_no[32];
    char value[STITCH_VALUE_MAX];
    stitched_service_line_t *line;
    stitched_line_adjustment_t *adjustment;
    size_t i;

    if (!json_get_string(journal_line, "service_line_number", line_no, sizeof(line_no)) ||
        line_no[0] == '\0') {
        return X12_OK;
    }

    line = find_service_line_by_number(aggregate, line_no);
    if (line == NULL) {
        line = find_or_add_service_line_by_number(aggregate, line_no);
        if (line == NULL) {
            return X12_ERR_NO_MEMORY;
        }
    }
    if (line->adjustment_count >= STITCH_MAX_ADJUSTMENTS_PER_LINE) {
        return X12_OK;
    }

    adjustment = &line->adjustments[line->adjustment_count++];
    memset(adjustment, 0, sizeof(*adjustment));
    if (json_get_string(journal_line, "adjustment_group_code", value, sizeof(value))) {
        copy_cstr(adjustment->adjustment_group_code, sizeof(adjustment->adjustment_group_code), value);
    }
    for (i = 0u; i < STITCH_MAX_ADJUSTMENT_VALUES; i++) {
        if (!json_get_array_string_at(journal_line, "reason_codes", i, adjustment->reason_codes[i], sizeof(adjustment->reason_codes[i])) &&
            !json_get_array_string_at(journal_line, "amounts", i, adjustment->amounts[i], sizeof(adjustment->amounts[i])) &&
            !json_get_array_string_at(journal_line, "quantities", i, adjustment->quantities[i], sizeof(adjustment->quantities[i]))) {
            break;
        }
        (void)json_get_array_string_at(
            journal_line,
            "reason_codes",
            i,
            adjustment->reason_codes[i],
            sizeof(adjustment->reason_codes[i])
        );
        (void)json_get_array_string_at(
            journal_line,
            "amounts",
            i,
            adjustment->amounts[i],
            sizeof(adjustment->amounts[i])
        );
        (void)json_get_array_string_at(
            journal_line,
            "quantities",
            i,
            adjustment->quantities[i],
            sizeof(adjustment->quantities[i])
        );
        adjustment->value_count++;
    }

    return X12_OK;
}

static int make_drop_key(
    const journal_event_t *journal_line,
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
    const journal_event_t *journal_line,
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

static int event_matches_filter(const stitch_state_t *state, const journal_event_t *journal_line)
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

static int event_has_encounter_id(const journal_event_t *journal_line)
{
    char encounter_id[STITCH_ID_MAX];

    return json_get_string(journal_line, "encounter_id", encounter_id, sizeof(encounter_id));
}

static int snapshot_add_string_array32(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    const char values[][32],
    size_t value_count
)
{
    yyjson_mut_val *arr;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, obj, key);
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < value_count; i++) {
        rc = json_writer_array_add_string(writer, arr, values[i]);
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_adjustments(
    json_writer_t *writer,
    yyjson_mut_val *line_obj,
    const stitched_service_line_t *line
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *adjustment_obj;
    const stitched_line_adjustment_t *adjustment;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, line_obj, "adjustments");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < line->adjustment_count; i++) {
        adjustment = &line->adjustments[i];
        adjustment_obj = json_writer_array_add_object(writer, arr);
        if (adjustment_obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }

        rc = json_writer_add_string(
            writer,
            adjustment_obj,
            "adjustment_group_code",
            adjustment->adjustment_group_code
        );
        if (rc == X12_OK) {
            rc = snapshot_add_string_array32(
                writer,
                adjustment_obj,
                "reason_codes",
                adjustment->reason_codes,
                adjustment->value_count
            );
        }
        if (rc == X12_OK) {
            rc = snapshot_add_string_array32(
                writer,
                adjustment_obj,
                "amounts",
                adjustment->amounts,
                adjustment->value_count
            );
        }
        if (rc == X12_OK) {
            rc = snapshot_add_string_array32(
                writer,
                adjustment_obj,
                "quantities",
                adjustment->quantities,
                adjustment->value_count
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_charge_context(
    json_writer_t *writer,
    yyjson_mut_val *line_obj,
    const stitched_service_line_t *line
)
{
    yyjson_mut_val *obj;
    int rc;

    if (!line->has_charge_context) {
        return X12_OK;
    }

    obj = json_writer_add_object(writer, line_obj, "charge_context");
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_string(writer, obj, "amount", line->charge_amount);
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "description", line->description);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "service_date", line->charge_service_date);
    }

    return rc;
}

static int snapshot_add_submitted_line(
    json_writer_t *writer,
    yyjson_mut_val *line_obj,
    const stitched_service_line_t *line
)
{
    yyjson_mut_val *obj;
    int rc;

    if (!line->has_submitted) {
        return X12_OK;
    }

    obj = json_writer_add_object(writer, line_obj, "submitted");
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_string(writer, obj, "line_type", line->submitted_line_type);
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "procedure_code_qualifier",
            line->submitted_procedure_code_qualifier
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "procedure_code_set",
            line->submitted_procedure_code_set
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "charge_amount", line->submitted_charge_amount);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "unit_measure_code",
            line->submitted_unit_measure_code
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "unit_count", line->submitted_unit_count);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "service_date", line->submitted_service_date);
    }

    return rc;
}

static int snapshot_add_remittance_line(
    json_writer_t *writer,
    yyjson_mut_val *line_obj,
    const stitched_service_line_t *line
)
{
    yyjson_mut_val *obj;
    int rc;

    if (!line->has_remittance) {
        return X12_OK;
    }

    obj = json_writer_add_object(writer, line_obj, "remittance");
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_string(
        writer,
        obj,
        "procedure_code_qualifier",
        line->remittance_procedure_code_qualifier
    );
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "procedure_code_set",
            line->remittance_procedure_code_set
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "line_charge_amount",
            line->remittance_line_charge_amount
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "line_paid_amount",
            line->remittance_line_paid_amount
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "paid_service_unit_count",
            line->remittance_paid_unit_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "service_date", line->remittance_service_date);
    }

    return rc;
}

static int snapshot_add_service_lines(
    json_writer_t *writer,
    yyjson_mut_val *root,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *line_obj;
    const stitched_service_line_t *line;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, root, "service_lines");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < aggregate->service_line_count; i++) {
        line = &aggregate->service_lines[i];
        line_obj = json_writer_array_add_object(writer, arr);
        if (line_obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }

        rc = json_writer_add_string(writer, line_obj, "service_line_number", line->service_line_number);
        if (rc == X12_OK) {
            rc = json_writer_add_string(
                writer,
                line_obj,
                "remittance_service_line_number",
                line->remit_service_line_number
            );
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, line_obj, "procedure_code", line->procedure_code);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, line_obj, "description", line->description);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, line_obj, "service_date", line->service_date);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, line_obj, "match_method", line->match_method);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_charge_context(writer, line_obj, line);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_submitted_line(writer, line_obj, line);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_remittance_line(writer, line_obj, line);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_adjustments(writer, line_obj, line);
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_source_event_ids(
    json_writer_t *writer,
    yyjson_mut_val *root,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *arr;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, root, "applied_event_ids");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < aggregate->source_event_count; i++) {
        rc = json_writer_array_add_size(writer, arr, aggregate->source_events[i].event_id);
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_update_event_ids(
    json_writer_t *writer,
    yyjson_mut_val *root,
    const stitch_update_batch_t *batch
)
{
    yyjson_mut_val *arr;
    size_t i;
    size_t end;
    int rc;

    if (batch == NULL || batch->aggregate == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    arr = json_writer_add_array(writer, root, "update_event_ids");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    end = batch->first_source_event_index + batch->source_event_count;
    for (i = batch->first_source_event_index; i < end; i++) {
        rc = json_writer_array_add_size(writer, arr, batch->aggregate->source_events[i].event_id);
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int build_snapshot_doc(
    const stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id,
    json_writer_t *writer,
    int include_contains_phi,
    int include_lineage,
    int include_event_ids
)
{
    const claim_aggregate_t *aggregate;
    yyjson_mut_val *root;
    yyjson_mut_val *keys;
    yyjson_mut_val *snapshot_state;
    yyjson_mut_val *lineage;
    int include_phi;
    int rc;
    char claim_id[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_control[STITCH_ID_MAX];
    char payer_control_token[TOKENISE_MAX_TOKEN_LEN];

    if (state == NULL || batch == NULL || batch->aggregate == NULL ||
        aggregate_id == NULL || writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    aggregate = batch->aggregate;
    include_phi = state->include_phi;

    rc = json_writer_init_object(writer);
    if (rc != X12_OK) {
        return rc;
    }

    root = json_writer_root(writer);
    rc = json_writer_add_string(writer, root, "event_type", "ClaimAggregateUpdated");
    if (rc == X12_OK && state->run_id[0] != '\0') {
        rc = json_writer_add_string(writer, root, "run_id", state->run_id);
    }
    if (rc == X12_OK && batch->source_run_id[0] != '\0') {
        rc = json_writer_add_string(writer, root, "source_run_id", batch->source_run_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "aggregate_type", "claim");
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "aggregate_id", aggregate_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(writer, root, "version", aggregate->version);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(writer, root, "updated_by_event_id", batch->updated_by_event_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "updated_by_event_type", batch->updated_by_event_type);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "update_scope", "source_drop");
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "source_drop_id", batch->source_drop_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            root,
            "compacted_source_event_count",
            batch->source_event_count
        );
    }
    if (rc == X12_OK && include_contains_phi) {
        rc = json_writer_add_bool(writer, root, "contains_phi", include_phi);
    }
    if (rc != X12_OK) {
        return rc;
    }

    keys = json_writer_add_object(writer, root, "keys");
    if (keys == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = resolve_identifier_output_pair(
        state,
        "claim_id",
        aggregate->claim_id,
        aggregate->claim_id_token,
        claim_id,
        sizeof(claim_id),
        claim_id_token,
        sizeof(claim_id_token)
    );
    if (rc != X12_OK) {
        return rc;
    }
    if (include_phi && claim_id[0] != '\0') {
        rc = json_writer_add_string(writer, keys, "claim_id", claim_id);
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, keys, "claim_id_token", claim_id_token);
        }
    } else {
        rc = json_writer_add_string(
            writer,
            keys,
            "claim_id",
            claim_id_token[0] != '\0' ? claim_id_token : aggregate->key
        );
    }
    if (rc != X12_OK) {
        return rc;
    }

    rc = resolve_identifier_output_pair(
        state,
        "payer_claim_control_number",
        aggregate->payer_claim_control_number,
        aggregate->payer_claim_control_number_token,
        payer_control,
        sizeof(payer_control),
        payer_control_token,
        sizeof(payer_control_token)
    );
    if (rc != X12_OK) {
        return rc;
    }
    if (payer_control[0] != '\0' || payer_control_token[0] != '\0') {
        if (include_phi && payer_control[0] != '\0') {
            rc = json_writer_add_string(
                writer,
                keys,
                "payer_claim_control_number",
                payer_control
            );
            if (rc == X12_OK) {
                rc = json_writer_add_string(
                    writer,
                    keys,
                    "payer_claim_control_number_token",
                    payer_control_token
                );
            }
        } else {
            rc = json_writer_add_string(
                writer,
                keys,
                "payer_claim_control_number",
                payer_control_token
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
    }
    if (aggregate->encounter_id[0] != '\0') {
        rc = json_writer_add_string(writer, keys, "encounter_id", aggregate->encounter_id);
        if (rc != X12_OK) {
            return rc;
        }
    }
    if (include_phi && aggregate->patient_id[0] != '\0') {
        rc = json_writer_add_string(writer, keys, "patient_id", aggregate->patient_id);
        if (rc == X12_OK && aggregate->patient_id_token[0] != '\0') {
            rc = json_writer_add_string(
                writer,
                keys,
                "patient_id_token",
                aggregate->patient_id_token
            );
        }
    } else if (aggregate->patient_id_token[0] != '\0') {
        rc = json_writer_add_string(writer, keys, "patient_id", aggregate->patient_id_token);
    }
    if (rc != X12_OK) {
        return rc;
    }
    if (include_phi && aggregate->patient_name[0] != '\0') {
        char patient_last_name_or_org[STITCH_VALUE_MAX];
        char patient_first_name[STITCH_VALUE_MAX];

        split_patient_name(
            aggregate->patient_name,
            patient_last_name_or_org,
            sizeof(patient_last_name_or_org),
            patient_first_name,
            sizeof(patient_first_name)
        );
        rc = json_writer_add_string(
            writer,
            keys,
            "patient_last_name_or_org",
            patient_last_name_or_org
        );
        if (rc == X12_OK && patient_first_name[0] != '\0') {
            rc = json_writer_add_string(writer, keys, "patient_first_name", patient_first_name);
        }
        if (rc == X12_OK && aggregate->patient_name_token[0] != '\0') {
            rc = json_writer_add_string(
                writer,
                keys,
                "patient_name_token",
                aggregate->patient_name_token
            );
        }
    } else if (aggregate->patient_name_token[0] != '\0') {
        rc = json_writer_add_string(writer, keys, "patient_name_token", aggregate->patient_name_token);
    }
    if (rc != X12_OK) {
        return rc;
    }

    snapshot_state = json_writer_add_object(writer, root, "state");
    if (snapshot_state == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_bool(
        writer,
        snapshot_state,
        "has_charge_context",
        aggregate->has_charge_context
    );
    if (rc == X12_OK) {
        rc = json_writer_add_bool(writer, snapshot_state, "has_837", aggregate->has_837);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_bool(writer, snapshot_state, "has_835", aggregate->has_835);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, snapshot_state, "claim_type", aggregate->claim_type);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            snapshot_state,
            "claim_status_code",
            aggregate->claim_status_code
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            snapshot_state,
            "source_event_count",
            aggregate->source_event_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            snapshot_state,
            "submitted_service_line_count",
            aggregate->submitted_service_line_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            snapshot_state,
            "remittance_service_line_count",
            aggregate->remittance_service_line_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            snapshot_state,
            "adjustment_count",
            aggregate->adjustment_count
        );
    }
    if (rc == X12_OK) {
        rc = snapshot_add_service_lines(writer, root, aggregate);
    }
    if (rc != X12_OK) {
        return rc;
    }

    if (include_lineage) {
        lineage = json_writer_add_object(writer, root, "lineage");
        if (lineage == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        rc = json_writer_add_size(
            writer,
            lineage,
            "applied_event_count",
            aggregate->source_event_count
        );
        if (rc == X12_OK) {
            rc = json_writer_add_size(
                writer,
                lineage,
                "update_event_count",
                batch->source_event_count
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
    }
    if (include_event_ids) {
        rc = snapshot_add_source_event_ids(writer, root, aggregate);
        if (rc == X12_OK) {
            rc = snapshot_add_update_event_ids(writer, root, batch);
        }
    }

    return rc;
}

static int build_snapshot_state_json(
    const stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id,
    char *out,
    size_t out_len
)
{
    json_writer_t writer = {0};
    int rc;

    if (out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';

    rc = build_snapshot_doc(
        state,
        batch,
        aggregate_id,
        &writer,
        1,
        1,
        0
    );
    if (rc == X12_OK) {
        rc = json_writer_write_cstring(&writer, out, out_len);
    }
    json_writer_free(&writer);
    return rc;
}

static int persist_snapshot_to_read_store(
    stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id
)
{
    char *state_json;
    char updated_by_event_id[32];
    int written;
    int rc;

    if (state == NULL || state->read_store == NULL) {
        return X12_OK;
    }

    state_json = (char *)malloc(STITCH_STATE_JSON_MAX);
    if (state_json == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = build_snapshot_state_json(
        state,
        batch,
        aggregate_id,
        state_json,
        STITCH_STATE_JSON_MAX
    );
    if (rc != X12_OK) {
        free(state_json);
        return rc;
    }

    written = snprintf(
        updated_by_event_id,
        sizeof(updated_by_event_id),
        "%zu",
        batch->updated_by_event_id
    );
    if (written < 0 || (size_t)written >= sizeof(updated_by_event_id)) {
        free(state_json);
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    rc = scribe_store_put_claim_aggregate(
        state->read_store,
        aggregate_id,
        batch->aggregate->version,
        state_json,
        updated_by_event_id,
        batch->source_drop_id
    );
    free(state_json);
    return rc;
}

static int write_notification(
    stitch_state_t *state,
    const stitch_update_batch_t *batch,
    const char *aggregate_id
)
{
    json_writer_t writer = {0};
    yyjson_mut_val *root;
    char notification_id[STITCH_ID_MAX + 64u];
    int written;
    int rc;

    if (state == NULL || batch == NULL || batch->aggregate == NULL ||
        aggregate_id == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (state->notify_out == NULL) {
        return X12_OK;
    }

    written = snprintf(
        notification_id,
        sizeof(notification_id),
        "%s:%zu",
        aggregate_id,
        batch->aggregate->version
    );
    if (written < 0 || (size_t)written >= sizeof(notification_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    rc = json_writer_init_object(&writer);
    if (rc != X12_OK) {
        return rc;
    }
    root = json_writer_root(&writer);

    rc = json_writer_add_string(&writer, root, "event_type", "AggregateVersionRecorded");
    if (rc == X12_OK) {
        rc = json_writer_add_bool(&writer, root, "ok", 1);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(&writer, root, "notification_id", notification_id);
    }
    if (rc == X12_OK && state->run_id[0] != '\0') {
        rc = json_writer_add_string(&writer, root, "run_id", state->run_id);
    }
    if (rc == X12_OK && batch->source_run_id[0] != '\0') {
        rc = json_writer_add_string(&writer, root, "source_run_id", batch->source_run_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(&writer, root, "aggregate_type", "claim");
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(&writer, root, "aggregate_id", aggregate_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(&writer, root, "version", batch->aggregate->version);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(&writer, root, "source_drop_id", batch->source_drop_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(&writer, root, "updated_by_event_id", batch->updated_by_event_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            &writer,
            root,
            "updated_by_event_type",
            batch->updated_by_event_type
        );
    }
    if (rc == X12_OK && batch->updated_by_journal_offset >= 0) {
        rc = json_writer_add_size(
            &writer,
            root,
            "updated_by_journal_offset",
            (size_t)batch->updated_by_journal_offset
        );
    }
    if (rc == X12_OK && batch->updated_by_journal_length >= 0) {
        rc = json_writer_add_size(
            &writer,
            root,
            "updated_by_journal_length",
            (size_t)batch->updated_by_journal_length
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_write_fp(&writer, state->notify_out, 1);
    }
    json_writer_free(&writer);

    return rc;
}

static int write_snapshot(
    stitch_state_t *state,
    const stitch_update_batch_t *batch
)
{
    json_writer_t writer = {0};
    char aggregate_id[STITCH_ID_MAX + 16u];
    int written;
    int rc;

    if (state == NULL || batch == NULL || batch->aggregate == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    written = snprintf(
        aggregate_id,
        sizeof(aggregate_id),
        "claim:%s",
        batch->aggregate->key
    );
    if (written < 0 || (size_t)written >= sizeof(aggregate_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    rc = build_snapshot_doc(
        state,
        batch,
        aggregate_id,
        &writer,
        0,
        0,
        1
    );
    if (rc == X12_OK) {
        rc = json_writer_write_fp(&writer, state->out, 1);
    }
    json_writer_free(&writer);
    if (rc != X12_OK) {
        return rc;
    }

    rc = persist_snapshot_to_read_store(state, batch, aggregate_id);
    if (rc != X12_OK) {
        return rc;
    }

    return write_notification(state, batch, aggregate_id);
}

static void capture_current_source_run_id(
    stitch_state_t *state,
    const journal_event_t *journal_line
)
{
    if (state == NULL || journal_line == NULL) {
        return;
    }

    state->current_source_run_id[0] = '\0';
    (void)json_get_string(
        journal_line,
        "run_id",
        state->current_source_run_id,
        sizeof(state->current_source_run_id)
    );
}

static int record_batch_update(
    stitch_state_t *state,
    claim_aggregate_t *aggregate,
    const char *drop_key,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
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
        copy_cstr(batch->source_run_id, sizeof(batch->source_run_id), state->current_source_run_id);
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
    batch->updated_by_journal_offset = journal_offset;
    batch->updated_by_journal_length = journal_length;
    copy_cstr(batch->updated_by_event_type, sizeof(batch->updated_by_event_type), event_type);
    if (state->current_source_run_id[0] != '\0') {
        copy_cstr(batch->source_run_id, sizeof(batch->source_run_id), state->current_source_run_id);
    }

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
    const journal_event_t *journal_line,
    size_t numeric_event_id,
    long long event_offset,
    long long event_length
)
{
    char event_id[32];
    char event_type[96];
    char segment_id[32];
    char claim_id[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char claim_id_raw[STITCH_ID_MAX];
    char claim_id_index_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_control[STITCH_ID_MAX];
    char payer_control_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_control_raw[STITCH_ID_MAX];
    char payer_control_index_token[TOKENISE_MAX_TOKEN_LEN];
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
        event_length,
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
        rc = resolve_identifier_output_pair(
            state,
            "claim_id",
            claim_id,
            claim_id_token,
            claim_id_raw,
            sizeof(claim_id_raw),
            claim_id_index_token,
            sizeof(claim_id_index_token)
        );
        if (rc == X12_OK) {
            rc = put_event_key_if_present(state->read_store, "claim_id_raw", claim_id_raw, event_id);
        }
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
        rc = resolve_identifier_output_pair(
            state,
            "payer_claim_control_number",
            payer_control,
            payer_control_token,
            payer_control_raw,
            sizeof(payer_control_raw),
            payer_control_index_token,
            sizeof(payer_control_index_token)
        );
        if (rc == X12_OK) {
            rc = put_event_key_if_present(
                state->read_store,
                "payer_claim_control_number_raw",
                payer_control_raw,
                event_id
            );
        }
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
    const journal_event_t *journal_line,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
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
            rc = apply_encounter_context_to_aggregate(state, aggregate);
            if (rc != X12_OK) {
                return rc;
            }
        }
        if (json_get_string(journal_line, "claim_type", value, sizeof(value))) {
            copy_cstr(aggregate->claim_type, sizeof(aggregate->claim_type), value);
        }
        rc = apply_charge_service_line(aggregate, journal_line);
        if (rc != X12_OK) {
            return rc;
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
        rc = capture_reference_patient_context(state, aggregate, journal_line, event_type);
        if (rc != X12_OK) {
            return rc;
        }
        if (strcmp(event_type, "ClaimServiceLineRecorded") == 0) {
            aggregate->submitted_service_line_count++;
            rc = apply_submitted_service_line(aggregate, journal_line);
            if (rc != X12_OK) {
                return rc;
            }
        } else if (strcmp(event_type, "ClaimLineDateRecorded") == 0) {
            rc = apply_submitted_line_date(aggregate, journal_line);
            if (rc != X12_OK) {
                return rc;
            }
        }
    } else if (strcmp(event_type, "RemittanceClaimPaymentObserved") == 0 ||
               strcmp(event_type, "RemittanceClaimReferencedPatient") == 0 ||
               strcmp(event_type, "RemittanceClaimReferencedSubscriber") == 0 ||
               strcmp(event_type, "RemittanceServiceLinePaymentObserved") == 0 ||
               strcmp(event_type, "RemittanceAdjustmentObserved") == 0 ||
               strcmp(event_type, "RemittanceDateRecorded") == 0) {
        aggregate->has_835 = 1;
        rc = capture_reference_patient_context(state, aggregate, journal_line, event_type);
        if (rc != X12_OK) {
            return rc;
        }
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
            rc = apply_remittance_service_line(aggregate, journal_line);
            if (rc != X12_OK) {
                return rc;
            }
        } else if (strcmp(event_type, "RemittanceAdjustmentObserved") == 0) {
            aggregate->adjustment_count++;
            rc = apply_service_line_adjustment(aggregate, journal_line);
            if (rc != X12_OK) {
                return rc;
            }
        } else if (strcmp(event_type, "RemittanceDateRecorded") == 0) {
            rc = apply_remittance_line_date(aggregate, journal_line);
            if (rc != X12_OK) {
                return rc;
            }
        }
    } else if (strcmp(event_type, "PatientPaymentObserved") == 0 ||
               strcmp(event_type, "WriteoffObserved") == 0 ||
               strcmp(event_type, "RefundObserved") == 0) {
        aggregate->has_charge_context = 1;
    } else {
        return X12_OK;
    }

    return record_batch_update(
        state,
        aggregate,
        drop_key,
        event_id,
        journal_offset,
        journal_length,
        event_type,
        fingerprint
    );
}

static int discover_encounter_claim(
    stitch_state_t *state,
    const journal_event_t *journal_line
)
{
    char event_type[96];
    char claim_id[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char value[STITCH_VALUE_MAX];
    claim_aggregate_t *aggregate;
    int rc;

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
        rc = apply_encounter_context_to_aggregate(state, aggregate);
        if (rc != X12_OK) {
            return rc;
        }
    }
    if (json_get_string(journal_line, "claim_type", value, sizeof(value))) {
        copy_cstr(aggregate->claim_type, sizeof(aggregate->claim_type), value);
    }

    return X12_OK;
}

static int read_journal_for_discovery(stitch_state_t *state, const char *path)
{
    journal_reader_t journal;
    journal_event_t record;
    int rc = X12_OK;

    if (state->encounter_filter[0] == '\0') {
        return X12_OK;
    }

    journal_reader_init(&journal);
    rc = journal_reader_open(&journal, path);
    if (rc != X12_OK) {
        return rc;
    }

    while (rc == X12_OK) {
        rc = journal_reader_next(&journal, &record);
        if (rc != X12_OK || record.record_len == 0u) {
            break;
        }
        rc = capture_encounter_context(state, &record);
        if (rc == X12_OK) {
            rc = discover_encounter_claim(state, &record);
        }
    }

    if (journal_reader_close(&journal) != X12_OK && rc == X12_OK) {
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
    phi_vault_t phi_vault;
    journal_reader_t journal;
    journal_event_t record;
    FILE *out;
    FILE *notify_out = NULL;
    char drop_key[STITCH_FINGERPRINT_MAX];
    char generated_run_id[96];
    size_t event_id = 0u;
    int owns_out = 0;
    int owns_notify_out = 0;
    int owns_read_store = 0;
    int owns_phi_vault = 0;
    int rc = X12_OK;

    if (input == NULL || input->journal_path == NULL || input->out_path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (input->notify_out_path != NULL &&
        strcmp(input->out_path, input->notify_out_path) == 0) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    journal_reader_init(&journal);
    rc = journal_reader_open(&journal, input->journal_path);
    if (rc != X12_OK) {
        return rc;
    }

    if (strcmp(input->out_path, "-") == 0) {
        out = stdout;
    } else {
        out = fopen(input->out_path, "wb");
        owns_out = 1;
    }
    if (out == NULL) {
        (void)journal_reader_close(&journal);
        return X12_ERR_IO;
    }

    if (input->notify_out_path != NULL) {
        if (strcmp(input->notify_out_path, "-") == 0) {
            notify_out = stdout;
        } else {
            notify_out = fopen(input->notify_out_path, "wb");
            owns_notify_out = 1;
        }
        if (notify_out == NULL) {
            (void)journal_reader_close(&journal);
            if (owns_out) {
                (void)fclose(out);
            }
            return X12_ERR_IO;
        }
    }

    state = (stitch_state_t *)calloc(1u, sizeof(*state));
    if (state == NULL) {
        (void)journal_reader_close(&journal);
        if (owns_out) {
            (void)fclose(out);
        }
        if (owns_notify_out) {
            (void)fclose(notify_out);
        }
        return X12_ERR_NO_MEMORY;
    }

    state->out = out;
    state->notify_out = notify_out;
    state->include_phi = input->include_phi;
    if (input->run_id != NULL && input->run_id[0] != '\0') {
        copy_cstr(state->run_id, sizeof(state->run_id), input->run_id);
    } else {
        rc = scribe_run_id_generate(generated_run_id, sizeof(generated_run_id));
        if (rc != X12_OK) {
            (void)journal_reader_close(&journal);
            if (owns_out) {
                (void)fclose(out);
            }
            if (owns_notify_out) {
                (void)fclose(notify_out);
            }
            free(state);
            return rc;
        }
        copy_cstr(state->run_id, sizeof(state->run_id), generated_run_id);
    }
    if (input->phi_vault_path != NULL) {
        phi_vault_init(&phi_vault);
        rc = phi_vault_open(&phi_vault, input->phi_vault_path);
        if (rc == X12_OK) {
            rc = phi_vault_init_schema(&phi_vault);
        }
        if (rc != X12_OK) {
            (void)phi_vault_close(&phi_vault);
            (void)journal_reader_close(&journal);
            if (owns_out) {
                (void)fclose(out);
            }
            if (owns_notify_out) {
                (void)fclose(notify_out);
            }
            free(state);
            return rc;
        }
        state->phi_vault = &phi_vault;
        owns_phi_vault = 1;
    }
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
            (void)journal_reader_close(&journal);
            if (owns_out) {
                (void)fclose(out);
            }
            if (owns_notify_out) {
                (void)fclose(notify_out);
            }
            if (owns_phi_vault) {
                (void)phi_vault_close(&phi_vault);
            }
            free(state);
            return rc;
        }
        state->read_store = &read_store;
        owns_read_store = 1;
    }

    rc = read_journal_for_discovery(state, input->journal_path);
    if (rc != X12_OK) {
        (void)journal_reader_close(&journal);
        if (owns_out) {
            (void)fclose(out);
        }
        if (owns_notify_out) {
            (void)fclose(notify_out);
        }
        if (owns_read_store) {
            (void)scribe_store_close(&read_store);
        }
        if (owns_phi_vault) {
            (void)phi_vault_close(&phi_vault);
        }
        free(state);
        return rc;
    }

    while (rc == X12_OK) {
        rc = journal_reader_next(&journal, &record);
        if (rc != X12_OK || record.record_len == 0u) {
            break;
        }
        event_id++;
        rc = make_drop_key(&record, drop_key, sizeof(drop_key));
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
            capture_current_source_run_id(state, &record);
        }

        rc = index_journal_event(
            state,
            &record,
            event_id,
            record.offset,
            record.stored_len
        );
        if (rc != X12_OK) {
            break;
        }

        rc = capture_encounter_context(state, &record);
        if (rc == X12_OK) {
            rc = apply_claim_event(
                state,
                &record,
                event_id,
                record.offset,
                record.stored_len,
                drop_key
            );
        }
        if (rc != X12_OK) {
            break;
        }
    }

    if (rc == X12_OK) {
        rc = flush_update_batches(state);
    }

    if (journal_reader_close(&journal) != X12_OK && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (fflush(out) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (notify_out != NULL && fflush(notify_out) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (owns_out && fclose(out) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (owns_notify_out && fclose(notify_out) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (owns_read_store && scribe_store_close(&read_store) != X12_OK && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (owns_phi_vault && phi_vault_close(&phi_vault) != X12_OK && rc == X12_OK) {
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
        } else if (strcmp(argv[i], "--notify-out") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.notify_out_path = argv[++i];
        } else if (strcmp(argv[i], "--phi-vault") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.phi_vault_path = argv[++i];
        } else if (strcmp(argv[i], "--encounter-id") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.encounter_id = argv[++i];
        } else if (strcmp(argv[i], "--include-phi") == 0) {
            input.include_phi = 1;
        } else if (strcmp(argv[i], "--run-id") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.run_id = argv[++i];
        } else {
            return -1;
        }
    }

    if (input.journal_path == NULL) {
        return -1;
    }

    return aggregate_stitcher_stitch(&input);
}
