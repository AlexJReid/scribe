#include "aggregate_stitcher.h"
#include "private.h"

#include "run_id.h"
#include "str_util.h"
#include "try.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static uint64_t stable_hash_update(uint64_t hash, const char *value)
{
    const unsigned char *p = (const unsigned char *)(value == NULL ? "" : value);

    while (*p != '\0') {
        hash ^= (uint64_t)*p++;
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t stable_hash_update_i64(uint64_t hash, long long value)
{
    char text[32];
    int written;

    written = snprintf(text, sizeof(text), "%lld", value);
    if (written < 0 || (size_t)written >= sizeof(text)) {
        return hash;
    }
    return stable_hash_update(hash, text);
}

static int stable_event_id(
    const journal_event_t *journal_line,
    size_t fallback_event_id,
    char *out,
    size_t out_len
)
{
    char segment_id[SCRIBE_STORE_ID_MAX];
    uint64_t hash = 1469598103934665603ull;
    int written;

    if (journal_line == NULL || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    segment_id[0] = '\0';
    if (journal_line->segment_path != NULL && journal_line->segment_path[0] != '\0') {
        scribe_copy_cstr(segment_id, sizeof(segment_id), journal_line->segment_path);
    } else if (!json_get_number_text(journal_line, "source_segment_index", segment_id, sizeof(segment_id))) {
        written = snprintf(segment_id, sizeof(segment_id), "%zu", fallback_event_id);
        if (written < 0 || (size_t)written >= sizeof(segment_id)) {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
    }

    hash = stable_hash_update(hash, segment_id);
    hash = stable_hash_update(hash, ":");
    hash = stable_hash_update_i64(hash, journal_line->offset);
    hash = stable_hash_update(hash, ":");
    hash = stable_hash_update_i64(hash, journal_line->stored_len);

    written = snprintf(out, out_len, "ev:%016llx", (unsigned long long)hash);
    if (written < 0 || (size_t)written >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    return X12_OK;
}

static int stable_numeric_event_id(
    const journal_event_t *journal_line,
    size_t fallback_event_id,
    size_t *out
)
{
    char event_id[SCRIBE_STORE_ID_MAX];
    char *end;
    unsigned long long value;

    if (out == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    TRY(stable_event_id(journal_line, fallback_event_id, event_id, sizeof(event_id)));
    if (strncmp(event_id, "ev:", 3u) != 0) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    value = strtoull(event_id + 3, &end, 16);
    if (end == event_id + 3 || *end != '\0') {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out = (size_t)value;
    return X12_OK;
}

static int claim_aggregate_id_from_key(
    const char *key,
    char *out,
    size_t out_len
)
{
    int written;

    if (key == NULL || key[0] == '\0' || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    written = snprintf(out, out_len, "claim:%s", key);
    if (written < 0 || (size_t)written >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    return X12_OK;
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
        scribe_copy_cstr(token_out, token_out_len, token);
        if (has_value && state != NULL && state->include_phi) {
            scribe_copy_cstr(raw_out, raw_out_len, value);
        } else {
            TRY(resolve_phi_token(state, namespace_name, token, raw_out, raw_out_len));
        }
        return X12_OK;
    }

    scribe_copy_cstr(token_out, token_out_len, value);
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

void claim_stitch_split_patient_name(
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
        scribe_copy_cstr(last_name_or_org, last_name_or_org_len, raw_name);
        return;
    }

    scribe_copy_str_slice(last_name_or_org, last_name_or_org_len, raw_name, (size_t)(sep - raw_name));
    scribe_copy_cstr(first_name, first_name_len, sep + 1);
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

    if (raw_name == NULL || raw_name[0] == '\0') {
        return X12_OK;
    }
    if (!overwrite && patient_name_token != NULL && patient_name_token[0] != '\0') {
        return X12_OK;
    }

    TRY(tokenise_cstring(TOK_PATIENT_NAME, raw_name, token, sizeof(token)));
    if (patient_name_token != NULL && patient_name_token_len > 0u) {
        scribe_copy_cstr(patient_name_token, patient_name_token_len, token);
    }
    if (state != NULL && state->include_phi && patient_name != NULL && patient_name_len > 0u) {
        scribe_copy_cstr(patient_name, patient_name_len, raw_name);
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
        scribe_copy_cstr(patient_id_token, patient_id_token_len, token);
    }
    if (state != NULL && state->include_phi &&
        patient_id != NULL && patient_id_len > 0u && raw_id != NULL && raw_id[0] != '\0') {
        scribe_copy_cstr(patient_id, patient_id_len, raw_id);
    }

    return X12_OK;
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

    if (!overwrite && patient_name_token != NULL && patient_name_token[0] != '\0') {
        return X12_OK;
    }
    TRY(resolve_phi_token(state, id_name_namespace, id_token, raw_name, sizeof(raw_name)));
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

    TRY(extract_value_token_pair(
        state,
        journal_line,
        "id_value",
        "id_value_token",
        id_namespace,
        raw_id,
        sizeof(raw_id),
        id_token,
        sizeof(id_token)
    ));
    if (strcmp(id_namespace, "patient_id") == 0) {
        TRY(set_patient_id(
            state,
            aggregate->patient_id,
            sizeof(aggregate->patient_id),
            aggregate->patient_id_token,
            sizeof(aggregate->patient_id_token),
            raw_id,
            id_token,
            overwrite
        ));
    }

    last_name_or_org[0] = '\0';
    first_name[0] = '\0';
    raw_name[0] = '\0';
    (void)json_get_string(journal_line, "last_name_or_org", last_name_or_org, sizeof(last_name_or_org));
    (void)json_get_string(journal_line, "first_name", first_name, sizeof(first_name));
    TRY(make_patient_name(last_name_or_org, first_name, raw_name, sizeof(raw_name)));
    TRY(set_patient_name(
        state,
        aggregate->patient_name,
        sizeof(aggregate->patient_name),
        aggregate->patient_name_token,
        sizeof(aggregate->patient_name_token),
        raw_name,
        overwrite
    ));

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

int claim_stitch_resolve_identifier_output_pair(
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

    if (raw_out == NULL || raw_out_len == 0u || token_out == NULL || token_out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    raw_out[0] = '\0';
    token_out[0] = '\0';

    if (token_value != NULL && token_value[0] != '\0') {
        scribe_copy_cstr(token_out, token_out_len, token_value);
        if (state != NULL && state->include_phi && raw_value != NULL && raw_value[0] != '\0') {
            scribe_copy_cstr(raw_out, raw_out_len, raw_value);
        } else {
            TRY(resolve_phi_token(state, namespace_name, token_value, raw_out, raw_out_len));
        }
        return X12_OK;
    }

    if (raw_value == NULL || raw_value[0] == '\0') {
        return X12_OK;
    }
    if (state != NULL && state->include_phi) {
        TRY(resolve_phi_token(state, namespace_name, raw_value, raw_out, raw_out_len));
        if (raw_out[0] == '\0') {
            scribe_copy_cstr(raw_out, raw_out_len, raw_value);
        } else {
            scribe_copy_cstr(token_out, token_out_len, raw_value);
        }
    } else {
        scribe_copy_cstr(token_out, token_out_len, raw_value);
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

static int hydrate_claim_aggregate_if_present(stitch_state_t *state, const char *key)
{
    char aggregate_id[STITCH_ID_MAX + 16u];
    char *state_json;
    size_t version = 0u;
    int rc;

    if (state == NULL || !state->incremental || state->read_store == NULL ||
        key == NULL || key[0] == '\0' || find_aggregate(state, key) != NULL) {
        return X12_OK;
    }

    TRY(claim_aggregate_id_from_key(key, aggregate_id, sizeof(aggregate_id)));

    state_json = (char *)malloc(STITCH_STATE_JSON_MAX);
    if (state_json == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = scribe_store_get_latest_claim_aggregate(
        state->read_store,
        aggregate_id,
        &version,
        state_json,
        STITCH_STATE_JSON_MAX
    );
    if (rc == X12_ERR_NOT_FOUND) {
        free(state_json);
        return X12_OK;
    }
    if (rc == X12_OK) {
        rc = claim_stitch_hydrate_snapshot(state, aggregate_id, state_json);
    }
    if (rc == X12_OK) {
        fprintf(
            stderr,
            "scribe stitch claims: hydrate aggregate=%s version=%zu\n",
            aggregate_id,
            version
        );
    }
    free(state_json);
    return rc;
}

static claim_aggregate_t *find_or_add_aggregate(
    stitch_state_t *state,
    const char *claim_id,
    const char *claim_id_token
)
{
    const char *key = claim_key(claim_id, claim_id_token);
    claim_aggregate_t *aggregate;

    if (hydrate_claim_aggregate_if_present(state, key) != X12_OK) {
        return NULL;
    }

    aggregate = find_aggregate(state, key);
    if (aggregate != NULL) {
        if (aggregate->claim_id[0] == '\0') {
            scribe_copy_cstr(aggregate->claim_id, sizeof(aggregate->claim_id), claim_id);
        }
        if (aggregate->claim_id_token[0] == '\0') {
            scribe_copy_cstr(aggregate->claim_id_token, sizeof(aggregate->claim_id_token), claim_id_token);
        }
        return aggregate;
    }

    if (state->aggregate_count >= STITCH_MAX_AGGREGATES) {
        return NULL;
    }

    aggregate = &state->aggregates[state->aggregate_count++];
    memset(aggregate, 0, sizeof(*aggregate));
    scribe_copy_cstr(aggregate->key, sizeof(aggregate->key), key);
    scribe_copy_cstr(aggregate->claim_id, sizeof(aggregate->claim_id), claim_id);
    scribe_copy_cstr(aggregate->claim_id_token, sizeof(aggregate->claim_id_token), claim_id_token);

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
    scribe_copy_cstr(source_event->fingerprint, sizeof(source_event->fingerprint), fingerprint);

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
    scribe_copy_cstr(line->match_method, sizeof(line->match_method), "unmatched");
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
        scribe_copy_cstr(line->service_line_number, sizeof(line->service_line_number), service_line_number);
    }
    return line;
}

static const char *service_line_effective_charge(const stitched_service_line_t *line)
{
    if (line == NULL) {
        return "";
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
        scribe_copy_cstr(line->procedure_code, sizeof(line->procedure_code), procedure_code);
    }
}

static void update_service_line_date(stitched_service_line_t *line)
{
    const char *submitted_date = "";

    if (line == NULL) {
        return;
    }
    if (line->submitted_service_date[0] != '\0') {
        submitted_date = line->submitted_service_date;
    }

    if (line->service_date[0] == '\0') {
        if (submitted_date[0] != '\0') {
            scribe_copy_cstr(line->service_date, sizeof(line->service_date), submitted_date);
        } else if (line->remittance_service_date[0] != '\0') {
            scribe_copy_cstr(line->service_date, sizeof(line->service_date), line->remittance_service_date);
        }
    }
    if (submitted_date[0] != '\0' &&
        line->remittance_service_date[0] != '\0' &&
        strcmp(submitted_date, line->remittance_service_date) == 0 &&
        strcmp(line->match_method, "procedure_charge") == 0) {
        scribe_copy_cstr(line->match_method, sizeof(line->match_method), "procedure_charge_date");
    }
}

static void copy_journal_string_if_present(
    const journal_event_t *journal_line,
    const char *field,
    char *out,
    size_t out_len
)
{
    char value[STITCH_VALUE_MAX];

    if (json_get_string(journal_line, field, value, sizeof(value))) {
        scribe_copy_cstr(out, out_len, value);
    }
}

static int apply_claim_observed(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    copy_journal_string_if_present(
        journal_line,
        "total_charge_amount",
        aggregate->claim_envelope.total_charge_amount,
        sizeof(aggregate->claim_envelope.total_charge_amount)
    );
    copy_journal_string_if_present(
        journal_line,
        "facility_type_code",
        aggregate->claim_envelope.facility_type_code,
        sizeof(aggregate->claim_envelope.facility_type_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "facility_code_qualifier",
        aggregate->claim_envelope.facility_code_qualifier,
        sizeof(aggregate->claim_envelope.facility_code_qualifier)
    );
    copy_journal_string_if_present(
        journal_line,
        "claim_frequency_type_code",
        aggregate->claim_envelope.claim_frequency_type_code,
        sizeof(aggregate->claim_envelope.claim_frequency_type_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "provider_signature_indicator",
        aggregate->claim_envelope.provider_signature_indicator,
        sizeof(aggregate->claim_envelope.provider_signature_indicator)
    );
    copy_journal_string_if_present(
        journal_line,
        "assignment_or_plan_participation_code",
        aggregate->claim_envelope.assignment_or_plan_participation_code,
        sizeof(aggregate->claim_envelope.assignment_or_plan_participation_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "benefits_assignment_certification_indicator",
        aggregate->claim_envelope.benefits_assignment_certification_indicator,
        sizeof(aggregate->claim_envelope.benefits_assignment_certification_indicator)
    );
    copy_journal_string_if_present(
        journal_line,
        "release_of_information_code",
        aggregate->claim_envelope.release_of_information_code,
        sizeof(aggregate->claim_envelope.release_of_information_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "patient_signature_source_code",
        aggregate->claim_envelope.patient_signature_source_code,
        sizeof(aggregate->claim_envelope.patient_signature_source_code)
    );
    if (aggregate->claim_type[0] == '\0' && aggregate->claim_envelope.facility_type_code[0] != '\0') {
        scribe_copy_cstr(aggregate->claim_type, sizeof(aggregate->claim_type), aggregate->claim_envelope.facility_type_code);
    }

    return X12_OK;
}

static int apply_subscriber_information(
    stitch_state_t *state,
    claim_aggregate_t *aggregate,
    const journal_event_t *journal_line
)
{
    int rc;

    copy_journal_string_if_present(
        journal_line,
        "payer_responsibility_sequence_number_code",
        aggregate->subscriber.payer_responsibility_sequence_number_code,
        sizeof(aggregate->subscriber.payer_responsibility_sequence_number_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "individual_relationship_code",
        aggregate->subscriber.individual_relationship_code,
        sizeof(aggregate->subscriber.individual_relationship_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "claim_filing_indicator_code",
        aggregate->subscriber.claim_filing_indicator_code,
        sizeof(aggregate->subscriber.claim_filing_indicator_code)
    );

    rc = extract_value_token_pair(
        state,
        journal_line,
        "insured_group_or_policy_number",
        "insured_group_or_policy_number_token",
        "reference_id",
        aggregate->subscriber.insured_group_or_policy_number,
        sizeof(aggregate->subscriber.insured_group_or_policy_number),
        aggregate->subscriber.insured_group_or_policy_number_token,
        sizeof(aggregate->subscriber.insured_group_or_policy_number_token)
    );

    return rc;
}

static int apply_patient_information(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    copy_journal_string_if_present(
        journal_line,
        "individual_relationship_code",
        aggregate->patient.individual_relationship_code,
        sizeof(aggregate->patient.individual_relationship_code)
    );

    return X12_OK;
}

static int apply_demographics(
    stitch_state_t *state,
    claim_aggregate_t *aggregate,
    const journal_event_t *journal_line
)
{
    stitched_party_context_t *party = &aggregate->subscriber;
    char party_scope[32];
    int rc;

    party_scope[0] = '\0';
    (void)json_get_string(journal_line, "party_scope", party_scope, sizeof(party_scope));
    if (strcmp(party_scope, "patient") == 0) {
        party = &aggregate->patient;
    }

    copy_journal_string_if_present(journal_line, "date_format", party->date_format, sizeof(party->date_format));
    copy_journal_string_if_present(journal_line, "gender_code", party->gender_code, sizeof(party->gender_code));
    rc = extract_value_token_pair(
        state,
        journal_line,
        "date_of_birth",
        "date_of_birth_token",
        "member_dob",
        party->date_of_birth,
        sizeof(party->date_of_birth),
        party->date_of_birth_token,
        sizeof(party->date_of_birth_token)
    );

    return rc;
}

static int apply_claim_date(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    stitched_claim_date_t *date;

    if (aggregate->claim_date_count >= STITCH_MAX_CLAIM_DATES) {
        return X12_OK;
    }

    date = &aggregate->claim_dates[aggregate->claim_date_count++];
    memset(date, 0, sizeof(*date));
    copy_journal_string_if_present(journal_line, "date_qualifier", date->date_qualifier, sizeof(date->date_qualifier));
    copy_journal_string_if_present(journal_line, "date_format", date->date_format, sizeof(date->date_format));
    copy_journal_string_if_present(journal_line, "date_value", date->date_value, sizeof(date->date_value));
    if (strcmp(date->date_qualifier, "435") == 0) {
        scribe_copy_cstr(
            aggregate->institutional_claim.admission_date,
            sizeof(aggregate->institutional_claim.admission_date),
            date->date_value
        );
    } else if (strcmp(date->date_qualifier, "096") == 0) {
        scribe_copy_cstr(
            aggregate->institutional_claim.discharge_date,
            sizeof(aggregate->institutional_claim.discharge_date),
            date->date_value
        );
    }

    return X12_OK;
}

static int copy_reference_from_event(
    stitch_state_t *state,
    stitched_claim_reference_t *reference,
    const journal_event_t *journal_line
)
{
    int rc;

    memset(reference, 0, sizeof(*reference));
    copy_journal_string_if_present(
        journal_line,
        "reference_scope",
        reference->reference_scope,
        sizeof(reference->reference_scope)
    );
    copy_journal_string_if_present(
        journal_line,
        "service_line_number",
        reference->service_line_number,
        sizeof(reference->service_line_number)
    );
    copy_journal_string_if_present(
        journal_line,
        "reference_qualifier",
        reference->reference_qualifier,
        sizeof(reference->reference_qualifier)
    );
    rc = extract_value_token_pair(
        state,
        journal_line,
        "reference_identification",
        "reference_identification_token",
        "reference_id",
        reference->reference_identification,
        sizeof(reference->reference_identification),
        reference->reference_identification_token,
        sizeof(reference->reference_identification_token)
    );

    return rc;
}

static int apply_claim_reference(
    stitch_state_t *state,
    claim_aggregate_t *aggregate,
    const journal_event_t *journal_line
)
{
    char reference_scope[32];
    char service_line_number[32];
    stitched_service_line_t *line;

    reference_scope[0] = '\0';
    service_line_number[0] = '\0';
    (void)json_get_string(journal_line, "reference_scope", reference_scope, sizeof(reference_scope));
    (void)json_get_string(journal_line, "service_line_number", service_line_number, sizeof(service_line_number));

    if (strcmp(reference_scope, "service_line") == 0) {
        line = find_or_add_service_line_by_number(aggregate, service_line_number);
        if (line == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        if (line->reference_count >= STITCH_MAX_REFERENCES_PER_LINE) {
            return X12_OK;
        }
        return copy_reference_from_event(
            state,
            &line->references[line->reference_count++],
            journal_line
        );
    }

    if (aggregate->claim_reference_count >= STITCH_MAX_REFERENCES_PER_CLAIM) {
        return X12_OK;
    }
    return copy_reference_from_event(
        state,
        &aggregate->claim_references[aggregate->claim_reference_count++],
        journal_line
    );
}

static int apply_diagnoses(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    size_t i;

    copy_journal_string_if_present(
        journal_line,
        "principal_diagnosis_code",
        aggregate->principal_diagnosis_code,
        sizeof(aggregate->principal_diagnosis_code)
    );
    aggregate->other_diagnosis_count = 0u;
    for (i = 0u; i < STITCH_MAX_DIAGNOSES; i++) {
        if (!json_get_array_string_at(
                journal_line,
                "other_diagnosis_codes",
                i,
                aggregate->other_diagnosis_codes[i],
                sizeof(aggregate->other_diagnosis_codes[i])
            )) {
            break;
        }
        aggregate->other_diagnosis_count++;
    }

    return X12_OK;
}

static int apply_healthcare_code(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    stitched_healthcare_code_t *code;
    size_t i;

    if (aggregate->healthcare_code_count >= STITCH_MAX_HEALTHCARE_CODES) {
        return X12_OK;
    }

    code = &aggregate->healthcare_codes[aggregate->healthcare_code_count++];
    memset(code, 0, sizeof(*code));
    copy_journal_string_if_present(
        journal_line,
        "healthcare_code_kind",
        code->healthcare_code_kind,
        sizeof(code->healthcare_code_kind)
    );
    copy_journal_string_if_present(
        journal_line,
        "healthcare_code_qualifier",
        code->healthcare_code_qualifier,
        sizeof(code->healthcare_code_qualifier)
    );
    copy_journal_string_if_present(
        journal_line,
        "healthcare_code",
        code->healthcare_code,
        sizeof(code->healthcare_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "healthcare_code_date_format",
        code->healthcare_code_date_format,
        sizeof(code->healthcare_code_date_format)
    );
    copy_journal_string_if_present(
        journal_line,
        "healthcare_code_date_value",
        code->healthcare_code_date_value,
        sizeof(code->healthcare_code_date_value)
    );
    copy_journal_string_if_present(
        journal_line,
        "healthcare_code_amount",
        code->healthcare_code_amount,
        sizeof(code->healthcare_code_amount)
    );
    for (i = 0u; i < STITCH_MAX_HEALTHCARE_CODE_COMPONENTS; i++) {
        if (!json_get_array_string_at(
                journal_line,
                "healthcare_code_components",
                i,
                code->healthcare_code_components[i],
                sizeof(code->healthcare_code_components[i])
            )) {
            break;
        }
        code->healthcare_code_component_count++;
    }
    if (strcmp(code->healthcare_code_kind, "diagnosis_related_group") == 0 ||
        strcmp(code->healthcare_code_qualifier, "DR") == 0) {
        scribe_copy_cstr(
            aggregate->institutional_claim.diagnosis_related_group_code,
            sizeof(aggregate->institutional_claim.diagnosis_related_group_code),
            code->healthcare_code
        );
    }

    return X12_OK;
}

static int apply_provider_taxonomy(claim_aggregate_t *aggregate, const journal_event_t *journal_line)
{
    stitched_provider_taxonomy_t *taxonomy;

    if (aggregate->provider_taxonomy_count >= STITCH_MAX_PROVIDER_TAXONOMIES) {
        return X12_OK;
    }

    taxonomy = &aggregate->provider_taxonomies[aggregate->provider_taxonomy_count++];
    memset(taxonomy, 0, sizeof(*taxonomy));
    copy_journal_string_if_present(
        journal_line,
        "reference_scope",
        taxonomy->reference_scope,
        sizeof(taxonomy->reference_scope)
    );
    copy_journal_string_if_present(
        journal_line,
        "service_line_number",
        taxonomy->service_line_number,
        sizeof(taxonomy->service_line_number)
    );
    copy_journal_string_if_present(
        journal_line,
        "provider_context",
        taxonomy->provider_context,
        sizeof(taxonomy->provider_context)
    );
    copy_journal_string_if_present(
        journal_line,
        "provider_role_code",
        taxonomy->provider_role_code,
        sizeof(taxonomy->provider_role_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "reference_identification_qualifier",
        taxonomy->reference_identification_qualifier,
        sizeof(taxonomy->reference_identification_qualifier)
    );
    copy_journal_string_if_present(
        journal_line,
        "provider_taxonomy_code",
        taxonomy->provider_taxonomy_code,
        sizeof(taxonomy->provider_taxonomy_code)
    );

    return X12_OK;
}

static int apply_institutional_information(
    claim_aggregate_t *aggregate,
    const journal_event_t *journal_line
)
{
    copy_journal_string_if_present(
        journal_line,
        "admission_type_code",
        aggregate->institutional_claim.admission_type_code,
        sizeof(aggregate->institutional_claim.admission_type_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "admission_source_code",
        aggregate->institutional_claim.admission_source_code,
        sizeof(aggregate->institutional_claim.admission_source_code)
    );
    copy_journal_string_if_present(
        journal_line,
        "patient_status_code",
        aggregate->institutional_claim.patient_status_code,
        sizeof(aggregate->institutional_claim.patient_status_code)
    );

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
        scribe_copy_cstr(line->submitted_line_type, sizeof(line->submitted_line_type), value);
    }
    if (json_get_string(journal_line, "procedure_code_qualifier", value, sizeof(value))) {
        scribe_copy_cstr(
            line->submitted_procedure_code_qualifier,
            sizeof(line->submitted_procedure_code_qualifier),
            value
        );
    }
    if (json_get_string(journal_line, "procedure_code_set", value, sizeof(value))) {
        scribe_copy_cstr(line->submitted_procedure_code_set, sizeof(line->submitted_procedure_code_set), value);
    }
    if (json_get_string(journal_line, "procedure_code", value, sizeof(value))) {
        scribe_copy_cstr(line->procedure_code, sizeof(line->procedure_code), value);
    }
    if (json_get_string(journal_line, "charge_amount", value, sizeof(value))) {
        scribe_copy_cstr(line->submitted_charge_amount, sizeof(line->submitted_charge_amount), value);
    }
    if (json_get_string(journal_line, "unit_measure_code", value, sizeof(value))) {
        scribe_copy_cstr(line->submitted_unit_measure_code, sizeof(line->submitted_unit_measure_code), value);
    }
    if (json_get_string(journal_line, "unit_count", value, sizeof(value))) {
        scribe_copy_cstr(line->submitted_unit_count, sizeof(line->submitted_unit_count), value);
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

    scribe_copy_cstr(line->submitted_service_date, sizeof(line->submitted_service_date), value);
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
        scribe_copy_cstr(line->match_method, sizeof(line->match_method), "procedure_charge");
    } else {
        line = find_service_line_by_number(aggregate, remit_line_no);
        if (line != NULL) {
            scribe_copy_cstr(line->match_method, sizeof(line->match_method), "line_order");
        }
    }
    if (line == NULL) {
        line = find_or_add_service_line_by_number(aggregate, remit_line_no);
        if (line == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        scribe_copy_cstr(line->match_method, sizeof(line->match_method), "created_from_remittance");
    }

    line->has_remittance = 1;
    scribe_copy_cstr(line->remit_service_line_number, sizeof(line->remit_service_line_number), remit_line_no);
    update_procedure_code_if_present(line, procedure_code);
    if (json_get_string(journal_line, "procedure_code_qualifier", value, sizeof(value))) {
        scribe_copy_cstr(
            line->remittance_procedure_code_qualifier,
            sizeof(line->remittance_procedure_code_qualifier),
            value
        );
    }
    if (json_get_string(journal_line, "procedure_code_set", value, sizeof(value))) {
        scribe_copy_cstr(line->remittance_procedure_code_set, sizeof(line->remittance_procedure_code_set), value);
    }
    scribe_copy_cstr(line->remittance_line_charge_amount, sizeof(line->remittance_line_charge_amount), charge_amount);
    if (json_get_string(journal_line, "line_paid_amount", value, sizeof(value))) {
        scribe_copy_cstr(line->remittance_line_paid_amount, sizeof(line->remittance_line_paid_amount), value);
    }
    if (json_get_string(journal_line, "paid_service_unit_count", value, sizeof(value))) {
        scribe_copy_cstr(line->remittance_paid_unit_count, sizeof(line->remittance_paid_unit_count), value);
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

    scribe_copy_cstr(line->remittance_service_date, sizeof(line->remittance_service_date), value);
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
        scribe_copy_cstr(adjustment->adjustment_group_code, sizeof(adjustment->adjustment_group_code), value);
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
    char source_drop_id[STITCH_VALUE_MAX] = "";
    char source_file[STITCH_VALUE_MAX] = "";
    char source_transaction[32] = "";
    int written;

    if (journal_line == NULL || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    out[0] = '\0';
    if (json_get_string(journal_line, "source_drop_id", source_drop_id, sizeof(source_drop_id))) {
        scribe_copy_cstr(out, out_len, source_drop_id);
        return X12_OK;
    }

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
    char reference_qualifier[32] = "";
    char healthcare_code_qualifier[32] = "";
    char healthcare_code[64] = "";
    int written;

    (void)json_get_string(journal_line, "source_file", source_file, sizeof(source_file));
    (void)json_get_number_text(journal_line, "source_segment_index", segment_index, sizeof(segment_index));
    (void)json_get_string(journal_line, "service_line_number", service_line_number, sizeof(service_line_number));
    (void)json_get_string(journal_line, "procedure_code", procedure_code, sizeof(procedure_code));
    (void)json_get_string(journal_line, "adjustment_group_code", adjustment_group_code, sizeof(adjustment_group_code));
    (void)json_get_string(journal_line, "reference_qualifier", reference_qualifier, sizeof(reference_qualifier));
    (void)json_get_string(
        journal_line,
        "healthcare_code_qualifier",
        healthcare_code_qualifier,
        sizeof(healthcare_code_qualifier)
    );
    (void)json_get_string(journal_line, "healthcare_code", healthcare_code, sizeof(healthcare_code));

    written = snprintf(
        out,
        out_len,
        "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s",
        event_type,
        source_file,
        segment_index,
        aggregate_key,
        service_line_number,
        procedure_code,
        adjustment_group_code,
        reference_qualifier,
        healthcare_code_qualifier,
        healthcare_code
    );
    if (written < 0 || (size_t)written >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return X12_OK;
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
        scribe_copy_cstr(batch->drop_key, sizeof(batch->drop_key), drop_key);
        scribe_copy_cstr(batch->source_drop_id, sizeof(batch->source_drop_id), state->current_source_drop_id);
        scribe_copy_cstr(batch->source_run_id, sizeof(batch->source_run_id), state->current_source_run_id);
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
    scribe_copy_cstr(batch->updated_by_event_type, sizeof(batch->updated_by_event_type), event_type);
    if (state->current_source_run_id[0] != '\0') {
        scribe_copy_cstr(batch->source_run_id, sizeof(batch->source_run_id), state->current_source_run_id);
    }

    return X12_OK;
}

static int set_current_source_drop(
    stitch_state_t *state,
    const char *drop_key,
    const journal_event_t *journal_line
)
{
    const char *separator;
    size_t drop_type_len;
    char source_file[STITCH_VALUE_MAX] = "";
    char source_type[32];
    int written;
    int rc;

    if (state == NULL || drop_key == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    scribe_copy_cstr(state->current_drop_key, sizeof(state->current_drop_key), drop_key);
    state->source_drop_count++;

    separator = strchr(drop_key, '|');
    if (journal_line != NULL) {
        (void)json_get_string(journal_line, "source_file", source_file, sizeof(source_file));
    }
    if (source_file[0] == '\0' && separator != NULL && separator[1] != '\0') {
        scribe_copy_cstr(source_file, sizeof(source_file), separator + 1);
    }

    if (separator == NULL && strchr(drop_key, ':') != NULL) {
        const char *type_end = strchr(drop_key, ':');

        scribe_copy_cstr(state->current_source_drop_id, sizeof(state->current_source_drop_id), drop_key);
        drop_type_len = (size_t)(type_end - drop_key);
    } else {
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
    }

    if (drop_type_len == 0u) {
        written = snprintf(
            source_type,
            sizeof(source_type),
            "source"
        );
    } else {
        written = snprintf(
            source_type,
            sizeof(source_type),
            "%.*s",
            (int)drop_type_len,
            drop_key
        );
    }
    if (written < 0 || (size_t)written >= sizeof(source_type)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    if (state->read_store != NULL) {
        rc = scribe_store_put_source_drop(
            state->read_store,
            state->current_source_drop_id,
            source_type,
            source_file,
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

static int mark_claim_dirty_for_key(
    stitch_state_t *state,
    const char *key_type,
    const char *key_value,
    const char *event_id
)
{
    char aggregate_ids[8][SCRIBE_STORE_ID_MAX];
    char aggregate_id[SCRIBE_STORE_ID_MAX];
    size_t aggregate_count = 0u;
    size_t i;
    int rc;

    if (state == NULL || !state->incremental || state->read_store == NULL ||
        key_type == NULL || key_value == NULL || key_value[0] == '\0' ||
        event_id == NULL || event_id[0] == '\0') {
        return X12_OK;
    }

    TRY(scribe_store_find_claim_aggregate_ids_by_key(
        state->read_store,
        key_type,
        key_value,
        aggregate_ids,
        8u,
        &aggregate_count
    ));

    if (aggregate_count == 0u && strcmp(key_type, "claim_id") == 0) {
        TRY(claim_aggregate_id_from_key(key_value, aggregate_id, sizeof(aggregate_id)));
        TRY(scribe_store_put_claim_aggregate_key(
            state->read_store,
            "claim_id",
            key_value,
            aggregate_id
        ));
        scribe_copy_cstr(aggregate_ids[0], sizeof(aggregate_ids[0]), aggregate_id);
        aggregate_count = 1u;
    }

    for (i = 0u; i < aggregate_count; i++) {
        rc = scribe_store_put_aggregate_event_route(
            state->read_store,
            "claim",
            aggregate_ids[i],
            event_id
        );
        if (rc == X12_OK) {
            rc = scribe_store_mark_dirty_aggregate(
                state->read_store,
                "claim",
                aggregate_ids[i],
                state->current_source_drop_id,
                event_id
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
        state->dirty_route_count++;
    }

    return X12_OK;
}

static int index_journal_event(
    stitch_state_t *state,
    const journal_event_t *journal_line,
    size_t numeric_event_id,
    long long event_offset,
    long long event_length
)
{
    char event_id[SCRIBE_STORE_ID_MAX];
    char event_type[96];
    char segment_id[SCRIBE_STORE_ID_MAX];
    char claim_id[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char claim_id_raw[STITCH_ID_MAX];
    char claim_id_index_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_control[STITCH_ID_MAX];
    char payer_control_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_control_raw[STITCH_ID_MAX];
    char payer_control_index_token[TOKENISE_MAX_TOKEN_LEN];
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

    TRY(stable_event_id(journal_line, numeric_event_id, event_id, sizeof(event_id)));
    if (journal_line->segment_path != NULL && journal_line->segment_path[0] != '\0') {
        scribe_copy_cstr(segment_id, sizeof(segment_id), journal_line->segment_path);
    } else if (!json_get_number_text(journal_line, "source_segment_index", segment_id, sizeof(segment_id))) {
        written = snprintf(segment_id, sizeof(segment_id), "%zu", numeric_event_id);
        if (written < 0 || (size_t)written >= sizeof(segment_id)) {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
    }

    TRY(scribe_store_put_event(
        state->read_store,
        event_id,
        state->current_source_drop_id,
        event_type,
        segment_id,
        event_offset,
        event_length,
        ""
    ));

    extract_claim_keys(
        journal_line,
        claim_id,
        sizeof(claim_id),
        claim_id_token,
        sizeof(claim_id_token)
    );
    claim_index_key = claim_key(claim_id, claim_id_token);
    TRY(put_event_key_if_present(state->read_store, "claim_id", claim_index_key, event_id));
    TRY(mark_claim_dirty_for_key(state, "claim_id", claim_index_key, event_id));
    if (state->include_phi) {
        rc = claim_stitch_resolve_identifier_output_pair(
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
        if (rc == X12_OK) {
            rc = mark_claim_dirty_for_key(state, "claim_id_raw", claim_id_raw, event_id);
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
    TRY(put_event_key_if_present(
        state->read_store,
        "payer_claim_control_number",
        payer_index_key,
        event_id
    ));
    TRY(mark_claim_dirty_for_key(
        state,
        "payer_claim_control_number",
        payer_index_key,
        event_id
    ));
    if (state->include_phi) {
        rc = claim_stitch_resolve_identifier_output_pair(
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
        if (rc == X12_OK) {
            rc = mark_claim_dirty_for_key(
                state,
                "payer_claim_control_number_raw",
                payer_control_raw,
                event_id
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

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

    if (!json_get_string(journal_line, "event_type", event_type, sizeof(event_type))) {
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
    if (aggregate == NULL) {
        aggregate = find_or_add_aggregate(state, claim_id, claim_id_token);
    }
    if (aggregate == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    TRY(make_fingerprint(journal_line, event_type, aggregate->key, fingerprint, sizeof(fingerprint)));
    if (aggregate_has_fingerprint(aggregate, fingerprint)) {
        return X12_OK;
    }

    if (strcmp(event_type, "ClaimObserved") == 0 ||
        strcmp(event_type, "ClaimReferencedBillingProvider") == 0 ||
        strcmp(event_type, "ClaimReferencedSubscriber") == 0 ||
        strcmp(event_type, "ClaimReferencedPatient") == 0 ||
        strcmp(event_type, "ClaimReferencedRenderingProvider") == 0 ||
        strcmp(event_type, "ClaimSubscriberInformationRecorded") == 0 ||
        strcmp(event_type, "ClaimPatientInformationRecorded") == 0 ||
        strcmp(event_type, "ClaimDemographicsRecorded") == 0 ||
        strcmp(event_type, "ClaimReferenceRecorded") == 0 ||
        strcmp(event_type, "ClaimDateRecorded") == 0 ||
        strcmp(event_type, "ClaimLineDateRecorded") == 0 ||
        strcmp(event_type, "ClaimDiagnosesRecorded") == 0 ||
        strcmp(event_type, "ClaimHealthcareCodeRecorded") == 0 ||
        strcmp(event_type, "ClaimProviderTaxonomyRecorded") == 0 ||
        strcmp(event_type, "ClaimInstitutionalInformationRecorded") == 0 ||
        strcmp(event_type, "ClaimServiceLineRecorded") == 0) {
        aggregate->has_837 = 1;
        TRY(capture_reference_patient_context(state, aggregate, journal_line, event_type));
        if (strcmp(event_type, "ClaimObserved") == 0) {
            TRY(apply_claim_observed(aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimSubscriberInformationRecorded") == 0) {
            TRY(apply_subscriber_information(state, aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimPatientInformationRecorded") == 0) {
            TRY(apply_patient_information(aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimDemographicsRecorded") == 0) {
            TRY(apply_demographics(state, aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimReferenceRecorded") == 0) {
            TRY(apply_claim_reference(state, aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimDateRecorded") == 0) {
            TRY(apply_claim_date(aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimDiagnosesRecorded") == 0) {
            TRY(apply_diagnoses(aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimHealthcareCodeRecorded") == 0) {
            TRY(apply_healthcare_code(aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimProviderTaxonomyRecorded") == 0) {
            TRY(apply_provider_taxonomy(aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimInstitutionalInformationRecorded") == 0) {
            TRY(apply_institutional_information(aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimServiceLineRecorded") == 0) {
            aggregate->submitted_service_line_count++;
            TRY(apply_submitted_service_line(aggregate, journal_line));
        } else if (strcmp(event_type, "ClaimLineDateRecorded") == 0) {
            TRY(apply_submitted_line_date(aggregate, journal_line));
        }
    } else if (strcmp(event_type, "RemittanceClaimPaymentObserved") == 0 ||
               strcmp(event_type, "RemittanceClaimReferencedPatient") == 0 ||
               strcmp(event_type, "RemittanceClaimReferencedSubscriber") == 0 ||
               strcmp(event_type, "RemittanceServiceLinePaymentObserved") == 0 ||
               strcmp(event_type, "RemittanceAdjustmentObserved") == 0 ||
               strcmp(event_type, "RemittanceDateRecorded") == 0) {
        aggregate->has_835 = 1;
        TRY(capture_reference_patient_context(state, aggregate, journal_line, event_type));
        if (json_get_string(journal_line, "claim_status_code", value, sizeof(value))) {
            scribe_copy_cstr(aggregate->claim_status_code, sizeof(aggregate->claim_status_code), value);
        }
        if (json_get_string(journal_line, "payer_claim_control_number", value, sizeof(value))) {
            scribe_copy_cstr(
                aggregate->payer_claim_control_number,
                sizeof(aggregate->payer_claim_control_number),
                value
            );
        }
        if (json_get_string(journal_line, "payer_claim_control_number_token", value, sizeof(value))) {
            scribe_copy_cstr(
                aggregate->payer_claim_control_number_token,
                sizeof(aggregate->payer_claim_control_number_token),
                value
            );
        }
        if (strcmp(event_type, "RemittanceServiceLinePaymentObserved") == 0) {
            aggregate->remittance_service_line_count++;
            TRY(apply_remittance_service_line(aggregate, journal_line));
        } else if (strcmp(event_type, "RemittanceAdjustmentObserved") == 0) {
            aggregate->adjustment_count++;
            TRY(apply_service_line_adjustment(aggregate, journal_line));
        } else if (strcmp(event_type, "RemittanceDateRecorded") == 0) {
            TRY(apply_remittance_line_date(aggregate, journal_line));
        }
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
    char drop_key[STITCH_FINGERPRINT_MAX];
    char generated_run_id[96];
    size_t event_id = 0u;
    int owns_out = 0;
    int owns_read_store = 0;
    int owns_phi_vault = 0;
    int reduce_pass;
    int shuffle_txn_open = 0;
    int rc = X12_OK;

    if (input == NULL || input->journal_path == NULL || input->out_path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (input->incremental && input->read_store_path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    journal_reader_init(&journal);
    TRY(journal_reader_open(&journal, input->journal_path));

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

    state = (stitch_state_t *)calloc(1u, sizeof(*state));
    if (state == NULL) {
        (void)journal_reader_close(&journal);
        if (owns_out) {
            (void)fclose(out);
        }
        return X12_ERR_NO_MEMORY;
    }

    state->out = out;
    state->include_phi = input->include_phi;
    state->incremental = input->incremental;
    fprintf(
        stderr,
        "scribe stitch claims: start mode=%s journal=%s read_store=%s out=%s\n",
        input->incremental ? "incremental" : "replay",
        input->journal_path,
        input->read_store_path == NULL ? "(none)" : input->read_store_path,
        input->out_path
    );
    if (input->run_id != NULL && input->run_id[0] != '\0') {
        scribe_copy_cstr(state->run_id, sizeof(state->run_id), input->run_id);
    } else {
        rc = scribe_run_id_generate(generated_run_id, sizeof(generated_run_id));
        if (rc != X12_OK) {
            (void)journal_reader_close(&journal);
            if (owns_out) {
                (void)fclose(out);
            }
            free(state);
            return rc;
        }
        scribe_copy_cstr(state->run_id, sizeof(state->run_id), generated_run_id);
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
            free(state);
            return rc;
        }
        state->phi_vault = &phi_vault;
        owns_phi_vault = 1;
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
            if (owns_phi_vault) {
                (void)phi_vault_close(&phi_vault);
            }
            free(state);
            return rc;
        }
        state->read_store = &read_store;
        owns_read_store = 1;
    }

    reduce_pass = input->incremental ? 0 : 1;
    while (rc == X12_OK) {
        if (input->incremental) {
            fprintf(
                stderr,
                "scribe stitch claims: %s pass journal=%s\n",
                reduce_pass ? "reduce" : "shuffle",
                input->journal_path
            );
        }
        if (input->incremental && !reduce_pass && state->read_store != NULL) {
            rc = scribe_store_begin_immediate(state->read_store);
            if (rc != X12_OK) {
                break;
            }
            shuffle_txn_open = 1;
        }

        while (rc == X12_OK) {
            size_t stable_id;

            rc = journal_reader_next(&journal, &record);
            if (rc != X12_OK || record.record_len == 0u) {
                break;
            }
            event_id++;
            rc = stable_numeric_event_id(&record, event_id, &stable_id);
            if (rc != X12_OK) {
                break;
            }
            rc = make_drop_key(&record, drop_key, sizeof(drop_key));
            if (rc != X12_OK) {
                break;
            }
            if (drop_key[0] != '\0' && strcmp(state->current_drop_key, drop_key) != 0) {
                rc = claim_stitch_flush_update_batches(state);
                if (rc != X12_OK) {
                    break;
                }
                rc = set_current_source_drop(state, drop_key, &record);
                if (rc != X12_OK) {
                    break;
                }
                capture_current_source_run_id(state, &record);
                fprintf(
                    stderr,
                    "scribe stitch claims: source_drop=%s source_run=%s\n",
                    state->current_source_drop_id,
                    state->current_source_run_id[0] == '\0' ? "(none)" : state->current_source_run_id
                );
            }

            if (!input->incremental || !reduce_pass) {
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
            }

            if (!input->incremental || reduce_pass) {
                rc = apply_claim_event(
                    state,
                    &record,
                    stable_id,
                    record.offset,
                    record.stored_len,
                    drop_key
                );
                if (rc != X12_OK) {
                    break;
                }
            }
        }

        if (rc != X12_OK) {
            if (shuffle_txn_open) {
                (void)scribe_store_rollback(state->read_store);
                shuffle_txn_open = 0;
            }
            break;
        }
        if (shuffle_txn_open) {
            rc = scribe_store_commit(state->read_store);
            if (rc != X12_OK) {
                (void)scribe_store_rollback(state->read_store);
                shuffle_txn_open = 0;
                break;
            }
            shuffle_txn_open = 0;
        }
        if (!input->incremental || reduce_pass) {
            break;
        }

        if (journal_reader_close(&journal) != X12_OK) {
            rc = X12_ERR_IO;
            break;
        }
        journal_reader_init(&journal);
        rc = journal_reader_open(&journal, input->journal_path);
        if (rc != X12_OK) {
            break;
        }
        state->current_drop_key[0] = '\0';
        state->current_source_drop_id[0] = '\0';
        state->current_source_run_id[0] = '\0';
        state->source_drop_count = 0u;
        event_id = 0u;
        reduce_pass = 1;
    }

    if (rc == X12_OK) {
        rc = claim_stitch_flush_update_batches(state);
    }

    if (journal_reader_close(&journal) != X12_OK && rc == X12_OK) {
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
    if (owns_phi_vault && phi_vault_close(&phi_vault) != X12_OK && rc == X12_OK) {
        rc = X12_ERR_IO;
    }

    fprintf(
        stderr,
        "scribe stitch claims: done events=%zu dirty_routes=%zu aggregates=%zu status=%s\n",
        event_id,
        state->dirty_route_count,
        state->aggregate_count,
        x12_error_message(rc)
    );
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
        } else if (strcmp(argv[i], "--phi-vault") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.phi_vault_path = argv[++i];
        } else if (strcmp(argv[i], "--include-phi") == 0) {
            input.include_phi = 1;
        } else if (strcmp(argv[i], "--incremental") == 0) {
            input.incremental = 1;
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
