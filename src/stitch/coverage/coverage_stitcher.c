#include "coverage_stitcher.h"

#include "journal.h"
#include "json_write.h"
#include "phi_vault.h"
#include "run_id.h"
#include "store.h"
#include "tokenise.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define json_get_string journal_event_get_string
#define json_get_number_text journal_event_get_number_text

#define COVERAGE_ID_MAX 128u
#define COVERAGE_VALUE_MAX 256u
#define COVERAGE_FINGERPRINT_MAX 384u
#define COVERAGE_STATE_JSON_MAX 65536u
#define COVERAGE_MAX_AGGREGATES 128u
#define COVERAGE_MAX_SOURCE_EVENTS 512u
#define COVERAGE_MAX_UPDATE_BATCHES 128u
#define COVERAGE_MAX_SERVICE_REQUESTS 64u
#define COVERAGE_MAX_BENEFITS 64u

typedef struct {
    size_t event_id;
    char event_type[96];
    long long journal_offset;
    long long journal_length;
} coverage_source_event_t;

typedef struct {
    char eligibility_id[COVERAGE_ID_MAX];
    char payer_id[COVERAGE_ID_MAX];
    char payer_id_token[TOKENISE_MAX_TOKEN_LEN];
    char service_type_code[32];
    char inquiry_date[32];
} coverage_service_request_t;

typedef struct {
    char eligibility_id[COVERAGE_ID_MAX];
    char payer_id[COVERAGE_ID_MAX];
    char payer_id_token[TOKENISE_MAX_TOKEN_LEN];
    char service_type_code[32];
    char eligibility_or_benefit_information_code[32];
    char coverage_level_code[32];
    char insurance_type_code[32];
    char plan_coverage_description[COVERAGE_VALUE_MAX];
    char time_period_qualifier[32];
    char monetary_amount[32];
    char percent[32];
    char quantity_qualifier[32];
    char quantity[32];
    char authorization_or_certification_indicator[16];
    char in_plan_network_indicator[16];
    char response_as_of_date[32];
    char effective_date[32];
    char termination_date[32];
} coverage_benefit_t;

typedef struct {
    char key[COVERAGE_ID_MAX];
    char member_id[COVERAGE_ID_MAX];
    char member_id_token[TOKENISE_MAX_TOKEN_LEN];
    char member_name[COVERAGE_VALUE_MAX];
    char member_name_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_id[COVERAGE_ID_MAX];
    char payer_id_token[TOKENISE_MAX_TOKEN_LEN];
    char date_of_birth[COVERAGE_ID_MAX];
    char date_of_birth_token[TOKENISE_MAX_TOKEN_LEN];
    char gender_code[16];
    char relationship_code[32];
    char enrollment_maintenance_type_code[32];
    char benefit_status_code[32];
    char coverage_effective_date[32];
    char coverage_termination_date[32];
    char last_coverage_date_qualifier[32];
    char last_coverage_date[32];
    char health_coverage_maintenance_type_code[32];
    char insurance_line_code[32];
    char plan_coverage_description[COVERAGE_VALUE_MAX];
    char coverage_level_code[32];
    coverage_service_request_t service_requests[COVERAGE_MAX_SERVICE_REQUESTS];
    size_t service_request_count;
    coverage_benefit_t benefits[COVERAGE_MAX_BENEFITS];
    size_t benefit_count;
    coverage_source_event_t source_events[COVERAGE_MAX_SOURCE_EVENTS];
    size_t source_event_count;
    size_t version;
} member_coverage_t;

typedef struct {
    member_coverage_t *coverage;
    char source_drop_id[SCRIBE_STORE_ID_MAX];
    char source_run_id[96];
    size_t first_source_event_index;
    size_t source_event_count;
    coverage_source_event_t updated_by;
} coverage_update_batch_t;

typedef struct {
    FILE *out;
    scribe_store_t *read_store;
    phi_vault_t *phi_vault;
    member_coverage_t aggregates[COVERAGE_MAX_AGGREGATES];
    size_t aggregate_count;
    coverage_update_batch_t update_batches[COVERAGE_MAX_UPDATE_BATCHES];
    size_t update_batch_count;
    char run_id[96];
    char current_drop_key[COVERAGE_FINGERPRINT_MAX];
    char current_source_drop_id[SCRIBE_STORE_ID_MAX];
    char current_source_run_id[96];
    char current_member_id[COVERAGE_ID_MAX];
    char current_member_id_token[TOKENISE_MAX_TOKEN_LEN];
    char current_payer_id[COVERAGE_ID_MAX];
    char current_payer_id_token[TOKENISE_MAX_TOKEN_LEN];
    char current_eligibility_id[COVERAGE_ID_MAX];
    char current_inquiry_date[32];
    char current_response_as_of_date[32];
    size_t current_benefit_index;
    int has_current_benefit;
    char pending_relationship_code[32];
    char pending_maintenance_type_code[32];
    char pending_benefit_status_code[32];
    int has_pending_enrollment;
    size_t source_drop_count;
    size_t dirty_route_count;
    int include_phi;
    int incremental;
} coverage_state_t;

static void copy_cstr(char *dst, size_t dst_len, const char *src)
{
    size_t len;

    if (dst == NULL || dst_len == 0u) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }
    len = strlen(src);
    if (len >= dst_len) {
        len = dst_len - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int add_string_if_present(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    const char *value
)
{
    if (value == NULL || value[0] == '\0') {
        return X12_OK;
    }

    return json_writer_add_string(writer, obj, key, value);
}

static int tokenise_cstring(token_type_t type, const char *value, char *out, size_t out_len)
{
    x12_str_t raw;

    if (out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    if (value == NULL || value[0] == '\0') {
        return X12_OK;
    }
    raw.ptr = (char *)value;
    raw.len = strlen(value);
    return tokenise_value(type, raw, out, out_len);
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
    const journal_event_t *event,
    size_t fallback_event_id,
    char *out,
    size_t out_len
)
{
    char segment_id[SCRIBE_STORE_ID_MAX];
    uint64_t hash = 1469598103934665603ull;
    int written;

    if (event == NULL || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    segment_id[0] = '\0';
    if (event->segment_path != NULL && event->segment_path[0] != '\0') {
        copy_cstr(segment_id, sizeof(segment_id), event->segment_path);
    } else if (!json_get_number_text(event, "source_segment_index", segment_id, sizeof(segment_id))) {
        written = snprintf(segment_id, sizeof(segment_id), "%zu", fallback_event_id);
        if (written < 0 || (size_t)written >= sizeof(segment_id)) {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
    }

    hash = stable_hash_update(hash, segment_id);
    hash = stable_hash_update(hash, ":");
    hash = stable_hash_update_i64(hash, event->offset);
    hash = stable_hash_update(hash, ":");
    hash = stable_hash_update_i64(hash, event->stored_len);

    written = snprintf(out, out_len, "ev:%016llx", (unsigned long long)hash);
    if (written < 0 || (size_t)written >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    return X12_OK;
}

static int stable_numeric_event_id(
    const journal_event_t *event,
    size_t fallback_event_id,
    size_t *out
)
{
    char event_id[SCRIBE_STORE_ID_MAX];
    char *end;
    unsigned long long value;
    int rc;

    if (out == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = stable_event_id(event, fallback_event_id, event_id, sizeof(event_id));
    if (rc != X12_OK) {
        return rc;
    }
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

static int member_coverage_id_from_key(
    const char *key,
    char *out,
    size_t out_len
)
{
    int written;

    if (key == NULL || key[0] == '\0' || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    written = snprintf(out, out_len, "member_coverage:%s", key);
    if (written < 0 || (size_t)written >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    return X12_OK;
}

static int resolve_phi_token(
    const coverage_state_t *state,
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
        "coverage",
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
    const coverage_state_t *state,
    const journal_event_t *event,
    const char *value_field,
    const char *token_field,
    const char *namespace_name,
    char *raw_out,
    size_t raw_out_len,
    char *token_out,
    size_t token_out_len
)
{
    char value[COVERAGE_VALUE_MAX];
    char token[TOKENISE_MAX_TOKEN_LEN];
    int has_value;
    int has_token;
    int rc;

    if (raw_out == NULL || raw_out_len == 0u || token_out == NULL || token_out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    raw_out[0] = '\0';
    token_out[0] = '\0';

    has_value = json_get_string(event, value_field, value, sizeof(value));
    has_token = json_get_string(event, token_field, token, sizeof(token));
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

static int make_member_name(
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

static void split_member_name(
    const char *name,
    char *last_name_or_org,
    size_t last_len,
    char *first_name,
    size_t first_len
)
{
    const char *separator;
    size_t len;

    if (last_name_or_org != NULL && last_len > 0u) {
        last_name_or_org[0] = '\0';
    }
    if (first_name != NULL && first_len > 0u) {
        first_name[0] = '\0';
    }
    if (name == NULL || name[0] == '\0') {
        return;
    }

    separator = strchr(name, '|');
    if (separator == NULL) {
        copy_cstr(last_name_or_org, last_len, name);
        return;
    }

    len = (size_t)(separator - name);
    if (last_name_or_org != NULL && last_len > 0u) {
        if (len >= last_len) {
            len = last_len - 1u;
        }
        memcpy(last_name_or_org, name, len);
        last_name_or_org[len] = '\0';
    }
    copy_cstr(first_name, first_len, separator + 1);
}

static int set_member_name(
    coverage_state_t *state,
    member_coverage_t *coverage,
    const char *raw_name
)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    int rc;

    if (coverage == NULL || raw_name == NULL || raw_name[0] == '\0') {
        return X12_OK;
    }

    rc = tokenise_cstring(TOK_MEMBER_NAME, raw_name, token, sizeof(token));
    if (rc != X12_OK) {
        return rc;
    }
    if (state != NULL && state->include_phi) {
        copy_cstr(coverage->member_name, sizeof(coverage->member_name), raw_name);
    }
    copy_cstr(coverage->member_name_token, sizeof(coverage->member_name_token), token);
    return X12_OK;
}

static int resolve_member_name_by_id(
    coverage_state_t *state,
    member_coverage_t *coverage
)
{
    char raw_name[COVERAGE_VALUE_MAX];
    int rc;

    if (state == NULL || coverage == NULL || coverage->member_id_token[0] == '\0') {
        return X12_OK;
    }
    if (!state->include_phi || coverage->member_name[0] != '\0') {
        return X12_OK;
    }

    rc = resolve_phi_token(
        state,
        "member_id_name",
        coverage->member_id_token,
        raw_name,
        sizeof(raw_name)
    );
    if (rc != X12_OK) {
        return rc;
    }
    return set_member_name(state, coverage, raw_name);
}

static member_coverage_t *find_coverage(coverage_state_t *state, const char *key)
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

static int hydrate_get_string(yyjson_val *obj, const char *key, char *out, size_t out_len)
{
    yyjson_val *value;
    const char *str;
    size_t len;

    if (out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    if (obj == NULL || key == NULL) {
        return X12_OK;
    }

    value = yyjson_obj_get(obj, key);
    if (value == NULL || !yyjson_is_str(value)) {
        return X12_OK;
    }

    str = yyjson_get_str(value);
    len = yyjson_get_len(value);
    if (len >= out_len) {
        len = out_len - 1u;
    }
    memcpy(out, str, len);
    out[len] = '\0';
    return X12_OK;
}

static void hydrate_get_size(yyjson_val *obj, const char *key, size_t *out)
{
    yyjson_val *value;

    if (out == NULL || obj == NULL || key == NULL) {
        return;
    }

    value = yyjson_obj_get(obj, key);
    if (value != NULL && yyjson_is_uint(value)) {
        *out = (size_t)yyjson_get_uint(value);
    }
}

static int hydrate_service_requests(member_coverage_t *coverage, yyjson_val *state_obj)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;

    arr = yyjson_obj_get(state_obj, "service_requests");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    yyjson_arr_foreach(arr, idx, max, item) {
        coverage_service_request_t *request;

        if (!yyjson_is_obj(item)) {
            continue;
        }
        if (coverage->service_request_count >= COVERAGE_MAX_SERVICE_REQUESTS) {
            return X12_ERR_NO_MEMORY;
        }

        request = &coverage->service_requests[coverage->service_request_count++];
        (void)hydrate_get_string(item, "eligibility_id", request->eligibility_id, sizeof(request->eligibility_id));
        (void)hydrate_get_string(item, "payer_id", request->payer_id, sizeof(request->payer_id));
        (void)hydrate_get_string(item, "payer_id_token", request->payer_id_token, sizeof(request->payer_id_token));
        (void)hydrate_get_string(item, "service_type_code", request->service_type_code, sizeof(request->service_type_code));
        (void)hydrate_get_string(item, "inquiry_date", request->inquiry_date, sizeof(request->inquiry_date));
    }

    return X12_OK;
}

static int hydrate_benefits(member_coverage_t *coverage, yyjson_val *state_obj)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;

    arr = yyjson_obj_get(state_obj, "benefits");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    yyjson_arr_foreach(arr, idx, max, item) {
        coverage_benefit_t *benefit;

        if (!yyjson_is_obj(item)) {
            continue;
        }
        if (coverage->benefit_count >= COVERAGE_MAX_BENEFITS) {
            return X12_ERR_NO_MEMORY;
        }

        benefit = &coverage->benefits[coverage->benefit_count++];
        (void)hydrate_get_string(item, "eligibility_id", benefit->eligibility_id, sizeof(benefit->eligibility_id));
        (void)hydrate_get_string(item, "payer_id", benefit->payer_id, sizeof(benefit->payer_id));
        (void)hydrate_get_string(item, "payer_id_token", benefit->payer_id_token, sizeof(benefit->payer_id_token));
        (void)hydrate_get_string(item, "service_type_code", benefit->service_type_code, sizeof(benefit->service_type_code));
        (void)hydrate_get_string(item, "eligibility_or_benefit_information_code", benefit->eligibility_or_benefit_information_code, sizeof(benefit->eligibility_or_benefit_information_code));
        (void)hydrate_get_string(item, "coverage_level_code", benefit->coverage_level_code, sizeof(benefit->coverage_level_code));
        (void)hydrate_get_string(item, "insurance_type_code", benefit->insurance_type_code, sizeof(benefit->insurance_type_code));
        (void)hydrate_get_string(item, "plan_coverage_description", benefit->plan_coverage_description, sizeof(benefit->plan_coverage_description));
        (void)hydrate_get_string(item, "time_period_qualifier", benefit->time_period_qualifier, sizeof(benefit->time_period_qualifier));
        (void)hydrate_get_string(item, "monetary_amount", benefit->monetary_amount, sizeof(benefit->monetary_amount));
        (void)hydrate_get_string(item, "percent", benefit->percent, sizeof(benefit->percent));
        (void)hydrate_get_string(item, "quantity_qualifier", benefit->quantity_qualifier, sizeof(benefit->quantity_qualifier));
        (void)hydrate_get_string(item, "quantity", benefit->quantity, sizeof(benefit->quantity));
        (void)hydrate_get_string(item, "authorization_or_certification_indicator", benefit->authorization_or_certification_indicator, sizeof(benefit->authorization_or_certification_indicator));
        (void)hydrate_get_string(item, "in_plan_network_indicator", benefit->in_plan_network_indicator, sizeof(benefit->in_plan_network_indicator));
        (void)hydrate_get_string(item, "response_as_of_date", benefit->response_as_of_date, sizeof(benefit->response_as_of_date));
        (void)hydrate_get_string(item, "effective_date", benefit->effective_date, sizeof(benefit->effective_date));
        (void)hydrate_get_string(item, "termination_date", benefit->termination_date, sizeof(benefit->termination_date));
    }

    return X12_OK;
}

static int hydrate_applied_event_ids(member_coverage_t *coverage, yyjson_val *root)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;

    if (coverage == NULL || root == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    arr = yyjson_obj_get(root, "applied_event_ids");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    coverage->source_event_count = 0u;
    yyjson_arr_foreach(arr, idx, max, item) {
        coverage_source_event_t *source_event;

        if (!yyjson_is_uint(item)) {
            continue;
        }
        if (coverage->source_event_count >= COVERAGE_MAX_SOURCE_EVENTS) {
            return X12_ERR_NO_MEMORY;
        }

        source_event = &coverage->source_events[coverage->source_event_count++];
        memset(source_event, 0, sizeof(*source_event));
        source_event->event_id = (size_t)yyjson_get_uint(item);
    }

    return X12_OK;
}

static int hydrate_member_coverage_snapshot(
    coverage_state_t *state,
    const char *aggregate_id,
    const char *state_json
)
{
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *keys;
    yyjson_val *state_obj;
    yyjson_val *obj;
    member_coverage_t *coverage;
    const char *key;
    int rc = X12_OK;

    if (state == NULL || aggregate_id == NULL || state_json == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (state->aggregate_count >= COVERAGE_MAX_AGGREGATES) {
        return X12_ERR_NO_MEMORY;
    }

    key = strncmp(aggregate_id, "member_coverage:", 16u) == 0 ?
        aggregate_id + 16u :
        aggregate_id;
    coverage = &state->aggregates[state->aggregate_count++];
    memset(coverage, 0, sizeof(*coverage));
    copy_cstr(coverage->key, sizeof(coverage->key), key);

    doc = yyjson_read(state_json, strlen(state_json), 0);
    if (doc == NULL) {
        state->aggregate_count--;
        return X12_ERR_INVALID_ARGUMENT;
    }
    root = yyjson_doc_get_root(doc);
    if (root == NULL || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        state->aggregate_count--;
        return X12_ERR_INVALID_ARGUMENT;
    }

    hydrate_get_size(root, "version", &coverage->version);
    keys = yyjson_obj_get(root, "keys");
    if (keys != NULL && yyjson_is_obj(keys)) {
        (void)hydrate_get_string(keys, "member_id", coverage->member_id, sizeof(coverage->member_id));
        (void)hydrate_get_string(keys, "member_id_token", coverage->member_id_token, sizeof(coverage->member_id_token));
        (void)hydrate_get_string(keys, "payer_id", coverage->payer_id, sizeof(coverage->payer_id));
        (void)hydrate_get_string(keys, "payer_id_token", coverage->payer_id_token, sizeof(coverage->payer_id_token));
        (void)hydrate_get_string(keys, "member_name_token", coverage->member_name_token, sizeof(coverage->member_name_token));
    }
    if (coverage->member_id_token[0] == '\0') {
        copy_cstr(coverage->member_id_token, sizeof(coverage->member_id_token), coverage->key);
    }

    state_obj = yyjson_obj_get(root, "state");
    if (state_obj != NULL && yyjson_is_obj(state_obj)) {
        obj = yyjson_obj_get(state_obj, "enrollment");
        if (obj != NULL && yyjson_is_obj(obj)) {
            (void)hydrate_get_string(obj, "relationship_code", coverage->relationship_code, sizeof(coverage->relationship_code));
            (void)hydrate_get_string(obj, "maintenance_type_code", coverage->enrollment_maintenance_type_code, sizeof(coverage->enrollment_maintenance_type_code));
            (void)hydrate_get_string(obj, "benefit_status_code", coverage->benefit_status_code, sizeof(coverage->benefit_status_code));
            (void)hydrate_get_string(obj, "coverage_effective_date", coverage->coverage_effective_date, sizeof(coverage->coverage_effective_date));
            (void)hydrate_get_string(obj, "coverage_termination_date", coverage->coverage_termination_date, sizeof(coverage->coverage_termination_date));
            (void)hydrate_get_string(obj, "last_coverage_date_qualifier", coverage->last_coverage_date_qualifier, sizeof(coverage->last_coverage_date_qualifier));
            (void)hydrate_get_string(obj, "last_coverage_date", coverage->last_coverage_date, sizeof(coverage->last_coverage_date));
        }
        obj = yyjson_obj_get(state_obj, "demographics");
        if (obj != NULL && yyjson_is_obj(obj)) {
            (void)hydrate_get_string(obj, "date_of_birth", coverage->date_of_birth, sizeof(coverage->date_of_birth));
            (void)hydrate_get_string(obj, "date_of_birth_token", coverage->date_of_birth_token, sizeof(coverage->date_of_birth_token));
            (void)hydrate_get_string(obj, "gender_code", coverage->gender_code, sizeof(coverage->gender_code));
        }
        obj = yyjson_obj_get(state_obj, "health_coverage");
        if (obj != NULL && yyjson_is_obj(obj)) {
            (void)hydrate_get_string(obj, "maintenance_type_code", coverage->health_coverage_maintenance_type_code, sizeof(coverage->health_coverage_maintenance_type_code));
            (void)hydrate_get_string(obj, "insurance_line_code", coverage->insurance_line_code, sizeof(coverage->insurance_line_code));
            (void)hydrate_get_string(obj, "plan_coverage_description", coverage->plan_coverage_description, sizeof(coverage->plan_coverage_description));
            (void)hydrate_get_string(obj, "coverage_level_code", coverage->coverage_level_code, sizeof(coverage->coverage_level_code));
        }
        rc = hydrate_service_requests(coverage, state_obj);
        if (rc == X12_OK) {
            rc = hydrate_benefits(coverage, state_obj);
        }
    }
    if (rc == X12_OK) {
        rc = hydrate_applied_event_ids(coverage, root);
    }

    yyjson_doc_free(doc);
    if (rc != X12_OK) {
        state->aggregate_count--;
    }
    return rc;
}

static int hydrate_member_coverage_if_present(coverage_state_t *state, const char *key)
{
    char aggregate_id[COVERAGE_ID_MAX + 32u];
    char state_json[COVERAGE_STATE_JSON_MAX];
    size_t version = 0u;
    int rc;

    if (state == NULL || !state->incremental || state->read_store == NULL ||
        key == NULL || key[0] == '\0' || find_coverage(state, key) != NULL) {
        return X12_OK;
    }

    rc = member_coverage_id_from_key(key, aggregate_id, sizeof(aggregate_id));
    if (rc != X12_OK) {
        return rc;
    }

    rc = scribe_store_get_latest_member_coverage(
        state->read_store,
        aggregate_id,
        &version,
        state_json,
        sizeof(state_json)
    );
    if (rc == X12_ERR_NOT_FOUND) {
        return X12_OK;
    }
    if (rc == X12_OK) {
        rc = hydrate_member_coverage_snapshot(state, aggregate_id, state_json);
    }
    if (rc == X12_OK) {
        fprintf(
            stderr,
            "scribe stitch coverage: hydrate aggregate=%s version=%zu\n",
            aggregate_id,
            version
        );
    }
    return rc;
}

static member_coverage_t *find_or_add_coverage(
    coverage_state_t *state,
    const char *member_id,
    const char *member_id_token
)
{
    const char *key = member_id_token != NULL && member_id_token[0] != '\0' ?
        member_id_token :
        member_id;
    member_coverage_t *coverage;

    if (state == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }

    if (hydrate_member_coverage_if_present(state, key) != X12_OK) {
        return NULL;
    }

    coverage = find_coverage(state, key);
    if (coverage != NULL) {
        if (coverage->member_id_token[0] == '\0') {
            copy_cstr(coverage->member_id_token, sizeof(coverage->member_id_token), member_id_token);
        }
        if (state->include_phi && coverage->member_id[0] == '\0') {
            copy_cstr(coverage->member_id, sizeof(coverage->member_id), member_id);
        }
        return coverage;
    }

    if (state->aggregate_count >= COVERAGE_MAX_AGGREGATES) {
        return NULL;
    }

    coverage = &state->aggregates[state->aggregate_count++];
    memset(coverage, 0, sizeof(*coverage));
    copy_cstr(coverage->key, sizeof(coverage->key), key);
    copy_cstr(coverage->member_id_token, sizeof(coverage->member_id_token), member_id_token);
    if (state->include_phi) {
        copy_cstr(coverage->member_id, sizeof(coverage->member_id), member_id);
    }
    return coverage;
}

static int current_coverage(coverage_state_t *state, member_coverage_t **out)
{
    member_coverage_t *coverage;

    if (out == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    coverage = find_or_add_coverage(
        state,
        state->current_member_id,
        state->current_member_id_token
    );
    if (coverage == NULL) {
        return X12_OK;
    }

    *out = coverage;
    return X12_OK;
}

static int append_source_event(
    member_coverage_t *coverage,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    coverage_source_event_t *source_event;

    if (coverage == NULL || event_type == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (coverage->source_event_count >= COVERAGE_MAX_SOURCE_EVENTS) {
        return X12_ERR_NO_MEMORY;
    }

    source_event = &coverage->source_events[coverage->source_event_count++];
    memset(source_event, 0, sizeof(*source_event));
    source_event->event_id = event_id;
    source_event->journal_offset = journal_offset;
    source_event->journal_length = journal_length;
    copy_cstr(source_event->event_type, sizeof(source_event->event_type), event_type);
    return X12_OK;
}

static int add_identifier_pair(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *name,
    const char *token_name,
    const char *raw,
    const char *token,
    int include_phi
)
{
    int rc = X12_OK;

    if (include_phi && raw != NULL && raw[0] != '\0') {
        rc = json_writer_add_string(writer, obj, name, raw);
        if (rc == X12_OK && token != NULL && token[0] != '\0') {
            rc = json_writer_add_string(writer, obj, token_name, token);
        }
    } else if (token != NULL && token[0] != '\0') {
        rc = json_writer_add_string(writer, obj, name, token);
    } else if (raw != NULL && raw[0] != '\0') {
        rc = json_writer_add_string(writer, obj, name, raw);
    }

    return rc;
}

static int add_member_name_keys(
    json_writer_t *writer,
    yyjson_mut_val *keys,
    const member_coverage_t *coverage,
    int include_phi
)
{
    char last_name_or_org[COVERAGE_VALUE_MAX];
    char first_name[COVERAGE_VALUE_MAX];
    int rc = X12_OK;

    if (include_phi && coverage->member_name[0] != '\0') {
        split_member_name(
            coverage->member_name,
            last_name_or_org,
            sizeof(last_name_or_org),
            first_name,
            sizeof(first_name)
        );
        rc = add_string_if_present(
            writer,
            keys,
            "member_last_name_or_org",
            last_name_or_org
        );
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, keys, "member_first_name", first_name);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(
                writer,
                keys,
                "member_name_token",
                coverage->member_name_token
            );
        }
    } else {
        rc = add_string_if_present(
            writer,
            keys,
            "member_name_token",
            coverage->member_name_token
        );
    }

    return rc;
}

static int snapshot_add_service_requests(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const member_coverage_t *coverage,
    int include_phi
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *obj;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, state_obj, "service_requests");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < coverage->service_request_count; i++) {
        const coverage_service_request_t *request = &coverage->service_requests[i];

        obj = json_writer_array_add_object(writer, arr);
        if (obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }

        rc = add_string_if_present(writer, obj, "eligibility_id", request->eligibility_id);
        if (rc == X12_OK) {
            rc = add_identifier_pair(
                writer,
                obj,
                "payer_id",
                "payer_id_token",
                request->payer_id,
                request->payer_id_token,
                include_phi
            );
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "service_type_code", request->service_type_code);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "inquiry_date", request->inquiry_date);
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_benefits(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const member_coverage_t *coverage,
    int include_phi
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *obj;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, state_obj, "benefits");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < coverage->benefit_count; i++) {
        const coverage_benefit_t *benefit = &coverage->benefits[i];

        obj = json_writer_array_add_object(writer, arr);
        if (obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }

        rc = add_string_if_present(writer, obj, "eligibility_id", benefit->eligibility_id);
        if (rc == X12_OK) {
            rc = add_identifier_pair(
                writer,
                obj,
                "payer_id",
                "payer_id_token",
                benefit->payer_id,
                benefit->payer_id_token,
                include_phi
            );
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "service_type_code", benefit->service_type_code);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(
                writer,
                obj,
                "eligibility_or_benefit_information_code",
                benefit->eligibility_or_benefit_information_code
            );
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "coverage_level_code", benefit->coverage_level_code);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "insurance_type_code", benefit->insurance_type_code);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(
                writer,
                obj,
                "plan_coverage_description",
                benefit->plan_coverage_description
            );
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "time_period_qualifier", benefit->time_period_qualifier);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "monetary_amount", benefit->monetary_amount);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "percent", benefit->percent);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "quantity_qualifier", benefit->quantity_qualifier);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "quantity", benefit->quantity);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(
                writer,
                obj,
                "authorization_or_certification_indicator",
                benefit->authorization_or_certification_indicator
            );
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(
                writer,
                obj,
                "in_plan_network_indicator",
                benefit->in_plan_network_indicator
            );
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "response_as_of_date", benefit->response_as_of_date);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "effective_date", benefit->effective_date);
        }
        if (rc == X12_OK) {
            rc = add_string_if_present(writer, obj, "termination_date", benefit->termination_date);
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_applied_event_ids(
    json_writer_t *writer,
    yyjson_mut_val *root,
    const member_coverage_t *coverage
)
{
    yyjson_mut_val *arr;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, root, "applied_event_ids");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < coverage->source_event_count; i++) {
        rc = json_writer_array_add_size(writer, arr, coverage->source_events[i].event_id);
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int build_snapshot_doc(
    const coverage_state_t *state,
    const coverage_update_batch_t *batch,
    const char *aggregate_id,
    json_writer_t *writer
)
{
    const member_coverage_t *coverage;
    const coverage_source_event_t *updated_by;
    yyjson_mut_val *root;
    yyjson_mut_val *keys;
    yyjson_mut_val *state_obj;
    yyjson_mut_val *enrollment;
    yyjson_mut_val *demographics;
    yyjson_mut_val *health_coverage;
    yyjson_mut_val *lineage;
    int include_phi;
    int rc;

    if (state == NULL || batch == NULL || batch->coverage == NULL ||
        aggregate_id == NULL || writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    coverage = batch->coverage;
    updated_by = &batch->updated_by;
    include_phi = state->include_phi;

    rc = json_writer_init_object(writer);
    if (rc != X12_OK) {
        return rc;
    }
    root = json_writer_root(writer);

    rc = json_writer_add_string(writer, root, "event_type", "MemberCoverageUpdated");
    if (rc == X12_OK && state->run_id[0] != '\0') {
        rc = json_writer_add_string(writer, root, "run_id", state->run_id);
    }
    if (rc == X12_OK && batch->source_run_id[0] != '\0') {
        rc = json_writer_add_string(writer, root, "source_run_id", batch->source_run_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "aggregate_type", "member_coverage");
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "aggregate_id", aggregate_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(writer, root, "version", coverage->version);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(writer, root, "updated_by_event_id", updated_by->event_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            root,
            "updated_by_event_type",
            updated_by->event_type
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, root, "source_drop_id", batch->source_drop_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_bool(writer, root, "contains_phi", include_phi);
    }
    if (rc != X12_OK) {
        return rc;
    }

    keys = json_writer_add_object(writer, root, "keys");
    if (keys == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    rc = add_identifier_pair(
        writer,
        keys,
        "member_id",
        "member_id_token",
        coverage->member_id,
        coverage->member_id_token,
        include_phi
    );
    if (rc == X12_OK) {
        rc = add_member_name_keys(writer, keys, coverage, include_phi);
    }
    if (rc == X12_OK) {
        rc = add_identifier_pair(
            writer,
            keys,
            "payer_id",
            "payer_id_token",
            coverage->payer_id,
            coverage->payer_id_token,
            include_phi
        );
    }
    if (rc != X12_OK) {
        return rc;
    }

    state_obj = json_writer_add_object(writer, root, "state");
    if (state_obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    rc = json_writer_add_size(
        writer,
        state_obj,
        "source_event_count",
        coverage->source_event_count
    );
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            state_obj,
            "service_request_count",
            coverage->service_request_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(writer, state_obj, "benefit_count", coverage->benefit_count);
    }
    if (rc != X12_OK) {
        return rc;
    }

    enrollment = json_writer_add_object(writer, state_obj, "enrollment");
    if (enrollment == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    rc = add_string_if_present(writer, enrollment, "relationship_code", coverage->relationship_code);
    if (rc == X12_OK) {
        rc = add_string_if_present(
            writer,
            enrollment,
            "maintenance_type_code",
            coverage->enrollment_maintenance_type_code
        );
    }
    if (rc == X12_OK) {
        rc = add_string_if_present(writer, enrollment, "benefit_status_code", coverage->benefit_status_code);
    }
    if (rc == X12_OK) {
        rc = add_string_if_present(
            writer,
            enrollment,
            "coverage_effective_date",
            coverage->coverage_effective_date
        );
    }
    if (rc == X12_OK) {
        rc = add_string_if_present(
            writer,
            enrollment,
            "coverage_termination_date",
            coverage->coverage_termination_date
        );
    }
    if (rc == X12_OK) {
        rc = add_string_if_present(
            writer,
            enrollment,
            "last_coverage_date_qualifier",
            coverage->last_coverage_date_qualifier
        );
    }
    if (rc == X12_OK) {
        rc = add_string_if_present(writer, enrollment, "last_coverage_date", coverage->last_coverage_date);
    }
    if (rc != X12_OK) {
        return rc;
    }

    demographics = json_writer_add_object(writer, state_obj, "demographics");
    if (demographics == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    rc = add_identifier_pair(
        writer,
        demographics,
        "date_of_birth",
        "date_of_birth_token",
        coverage->date_of_birth,
        coverage->date_of_birth_token,
        include_phi
    );
    if (rc == X12_OK) {
        rc = add_string_if_present(writer, demographics, "gender_code", coverage->gender_code);
    }
    if (rc != X12_OK) {
        return rc;
    }

    health_coverage = json_writer_add_object(writer, state_obj, "health_coverage");
    if (health_coverage == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    rc = add_string_if_present(
        writer,
        health_coverage,
        "maintenance_type_code",
        coverage->health_coverage_maintenance_type_code
    );
    if (rc == X12_OK) {
        rc = add_string_if_present(
            writer,
            health_coverage,
            "insurance_line_code",
            coverage->insurance_line_code
        );
    }
    if (rc == X12_OK) {
        rc = add_string_if_present(
            writer,
            health_coverage,
            "plan_coverage_description",
            coverage->plan_coverage_description
        );
    }
    if (rc == X12_OK) {
        rc = add_string_if_present(
            writer,
            health_coverage,
            "coverage_level_code",
            coverage->coverage_level_code
        );
    }
    if (rc == X12_OK) {
        rc = snapshot_add_service_requests(writer, state_obj, coverage, include_phi);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_benefits(writer, state_obj, coverage, include_phi);
    }
    if (rc != X12_OK) {
        return rc;
    }

    lineage = json_writer_add_object(writer, root, "lineage");
    if (lineage == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    rc = json_writer_add_size(
        writer,
        lineage,
        "applied_event_count",
        coverage->source_event_count
    );
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            lineage,
            "updated_by_journal_offset",
            (size_t)updated_by->journal_offset
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            writer,
            lineage,
            "updated_by_journal_length",
            (size_t)updated_by->journal_length
        );
    }
    if (rc == X12_OK) {
        rc = snapshot_add_applied_event_ids(writer, root, coverage);
    }

    return rc;
}

static int put_member_coverage_key_if_present(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    const char *aggregate_id
)
{
    if (store == NULL || key_value == NULL || key_value[0] == '\0') {
        return X12_OK;
    }

    return scribe_store_put_member_coverage_key(store, key_type, key_value, aggregate_id);
}

static int persist_member_coverage_keys(
    coverage_state_t *state,
    const member_coverage_t *coverage,
    const char *aggregate_id
)
{
    size_t i;
    int rc;

    if (state == NULL || state->read_store == NULL || coverage == NULL || aggregate_id == NULL) {
        return X12_OK;
    }

    rc = put_member_coverage_key_if_present(
        state->read_store,
        "member_id",
        coverage->member_id_token[0] != '\0' ? coverage->member_id_token : coverage->member_id,
        aggregate_id
    );
    if (rc == X12_OK && state->include_phi) {
        rc = put_member_coverage_key_if_present(
            state->read_store,
            "member_id_raw",
            coverage->member_id,
            aggregate_id
        );
    }
    if (rc == X12_OK) {
        rc = put_member_coverage_key_if_present(
            state->read_store,
            "payer_id",
            coverage->payer_id_token[0] != '\0' ? coverage->payer_id_token : coverage->payer_id,
            aggregate_id
        );
    }
    if (rc == X12_OK && state->include_phi) {
        rc = put_member_coverage_key_if_present(
            state->read_store,
            "payer_id_raw",
            coverage->payer_id,
            aggregate_id
        );
    }
    if (rc == X12_OK) {
        rc = put_member_coverage_key_if_present(
            state->read_store,
            "coverage_effective_date",
            coverage->coverage_effective_date,
            aggregate_id
        );
    }
    if (rc == X12_OK) {
        rc = put_member_coverage_key_if_present(
            state->read_store,
            "coverage_termination_date",
            coverage->coverage_termination_date,
            aggregate_id
        );
    }
    for (i = 0u; rc == X12_OK && i < coverage->service_request_count; i++) {
        rc = put_member_coverage_key_if_present(
            state->read_store,
            "service_type_code",
            coverage->service_requests[i].service_type_code,
            aggregate_id
        );
        if (rc == X12_OK) {
            rc = put_member_coverage_key_if_present(
                state->read_store,
                "inquiry_date",
                coverage->service_requests[i].inquiry_date,
                aggregate_id
            );
        }
    }
    for (i = 0u; rc == X12_OK && i < coverage->benefit_count; i++) {
        rc = put_member_coverage_key_if_present(
            state->read_store,
            "service_type_code",
            coverage->benefits[i].service_type_code,
            aggregate_id
        );
        if (rc == X12_OK) {
            rc = put_member_coverage_key_if_present(
                state->read_store,
                "benefit_effective_date",
                coverage->benefits[i].effective_date,
                aggregate_id
            );
        }
        if (rc == X12_OK) {
            rc = put_member_coverage_key_if_present(
                state->read_store,
                "benefit_termination_date",
                coverage->benefits[i].termination_date,
                aggregate_id
            );
        }
    }

    return rc;
}

static int write_snapshot(coverage_state_t *state, const coverage_update_batch_t *batch)
{
    json_writer_t writer = {0};
    member_coverage_t *coverage;
    const coverage_source_event_t *updated_by;
    char aggregate_id[COVERAGE_ID_MAX + 32u];
    char state_json[COVERAGE_STATE_JSON_MAX];
    char updated_by_event_id[32];
    int written;
    int rc;

    if (state == NULL || batch == NULL || batch->coverage == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    coverage = batch->coverage;
    updated_by = &batch->updated_by;

    written = snprintf(
        aggregate_id,
        sizeof(aggregate_id),
        "member_coverage:%s",
        coverage->key
    );
    if (written < 0 || (size_t)written >= sizeof(aggregate_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    fprintf(
        stderr,
        "scribe stitch coverage: emit aggregate=%s version=%zu source_drop=%s\n",
        aggregate_id,
        coverage->version,
        batch->source_drop_id
    );

    rc = build_snapshot_doc(state, batch, aggregate_id, &writer);
    if (rc == X12_OK) {
        rc = json_writer_write_cstring(&writer, state_json, sizeof(state_json));
    }
    if (rc == X12_OK) {
        rc = json_writer_write_fp(&writer, state->out, 1);
    }
    json_writer_free(&writer);
    if (rc != X12_OK) {
        return rc;
    }

    if (state->read_store == NULL) {
        return X12_OK;
    }

    written = snprintf(
        updated_by_event_id,
        sizeof(updated_by_event_id),
        "%zu",
        updated_by->event_id
    );
    if (written < 0 || (size_t)written >= sizeof(updated_by_event_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    rc = scribe_store_put_member_coverage(
        state->read_store,
        aggregate_id,
        coverage->version,
        state_json,
        updated_by_event_id,
        batch->source_drop_id
    );
    if (rc != X12_OK) {
        return rc;
    }

    rc = persist_member_coverage_keys(state, coverage, aggregate_id);
    if (rc != X12_OK) {
        return rc;
    }
    if (state->incremental && state->read_store != NULL) {
        rc = scribe_store_clear_dirty_aggregate(
            state->read_store,
            "member_coverage",
            aggregate_id,
            batch->source_drop_id
        );
    }

    return rc;
}

static int coverage_stitch_flush_update_batches(coverage_state_t *state)
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

static int record_coverage_update(
    coverage_state_t *state,
    member_coverage_t *coverage,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    coverage_update_batch_t *batch = NULL;
    const char *source_drop_id;
    int is_new_batch = 0;
    size_t i;
    int rc;

    if (state == NULL || coverage == NULL || event_type == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    source_drop_id = state->current_source_drop_id;
    for (i = 0u; i < state->update_batch_count; i++) {
        if (state->update_batches[i].coverage == coverage &&
            strcmp(state->update_batches[i].source_drop_id, source_drop_id) == 0) {
            batch = &state->update_batches[i];
            break;
        }
    }
    if (batch == NULL) {
        if (state->update_batch_count >= COVERAGE_MAX_UPDATE_BATCHES) {
            return X12_ERR_NO_MEMORY;
        }
        batch = &state->update_batches[state->update_batch_count++];
        memset(batch, 0, sizeof(*batch));
        batch->coverage = coverage;
        batch->first_source_event_index = coverage->source_event_count;
        copy_cstr(batch->source_drop_id, sizeof(batch->source_drop_id), source_drop_id);
        copy_cstr(batch->source_run_id, sizeof(batch->source_run_id), state->current_source_run_id);
        coverage->version++;
        is_new_batch = 1;
    }

    rc = append_source_event(coverage, event_id, journal_offset, journal_length, event_type);
    if (rc != X12_OK) {
        if (is_new_batch) {
            state->update_batch_count--;
            coverage->version--;
        }
        return rc;
    }
    batch->source_event_count++;
    batch->updated_by = coverage->source_events[coverage->source_event_count - 1u];
    if (state->current_source_run_id[0] != '\0') {
        copy_cstr(batch->source_run_id, sizeof(batch->source_run_id), state->current_source_run_id);
    }

    return X12_OK;
}

static int make_drop_key(
    const journal_event_t *event,
    char *out,
    size_t out_len
)
{
    char source_drop_id[COVERAGE_VALUE_MAX] = "";
    char source_file[COVERAGE_VALUE_MAX] = "";
    char source_transaction[32] = "";
    int written;

    if (event == NULL || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';

    if (json_get_string(event, "source_drop_id", source_drop_id, sizeof(source_drop_id))) {
        copy_cstr(out, out_len, source_drop_id);
        return X12_OK;
    }

    (void)json_get_string(event, "source_file", source_file, sizeof(source_file));
    (void)json_get_string(event, "source_transaction", source_transaction, sizeof(source_transaction));
    if (source_file[0] == '\0' && source_transaction[0] == '\0') {
        return X12_OK;
    }

    written = snprintf(out, out_len, "%s|%s", source_transaction, source_file);
    if (written < 0 || (size_t)written >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return X12_OK;
}

static int set_current_source_drop(
    coverage_state_t *state,
    const char *drop_key,
    const journal_event_t *event
)
{
    const char *separator;
    size_t drop_type_len;
    char source_file[COVERAGE_VALUE_MAX] = "";
    char source_type[32];
    int written;
    int rc;

    if (state == NULL || drop_key == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    copy_cstr(state->current_drop_key, sizeof(state->current_drop_key), drop_key);
    state->source_drop_count++;

    separator = strchr(drop_key, '|');
    if (event != NULL) {
        (void)json_get_string(event, "source_file", source_file, sizeof(source_file));
    }
    if (source_file[0] == '\0' && separator != NULL && separator[1] != '\0') {
        copy_cstr(source_file, sizeof(source_file), separator + 1);
    }

    if (separator == NULL && strchr(drop_key, ':') != NULL) {
        const char *type_end = strchr(drop_key, ':');

        copy_cstr(state->current_source_drop_id, sizeof(state->current_source_drop_id), drop_key);
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

    if (state->read_store == NULL) {
        return X12_OK;
    }

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

    return X12_OK;
}

static void capture_current_source_run_id(
    coverage_state_t *state,
    const journal_event_t *event
)
{
    if (state == NULL || event == NULL) {
        return;
    }

    state->current_source_run_id[0] = '\0';
    (void)json_get_string(event, "run_id", state->current_source_run_id, sizeof(state->current_source_run_id));
}

static int put_event_key_if_present(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    const char *event_id
)
{
    if (store == NULL || key_value == NULL || key_value[0] == '\0') {
        return X12_OK;
    }

    return scribe_store_put_event_key(store, key_type, key_value, event_id);
}

static int mark_member_coverage_dirty_for_key(
    coverage_state_t *state,
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

    rc = scribe_store_find_member_coverage_ids_by_key(
        state->read_store,
        key_type,
        key_value,
        aggregate_ids,
        8u,
        &aggregate_count
    );
    if (rc != X12_OK) {
        return rc;
    }

    if (aggregate_count == 0u && strcmp(key_type, "member_id") == 0) {
        rc = member_coverage_id_from_key(key_value, aggregate_id, sizeof(aggregate_id));
        if (rc != X12_OK) {
            return rc;
        }
        rc = scribe_store_put_member_coverage_key(
            state->read_store,
            "member_id",
            key_value,
            aggregate_id
        );
        if (rc != X12_OK) {
            return rc;
        }
        copy_cstr(aggregate_ids[0], sizeof(aggregate_ids[0]), aggregate_id);
        aggregate_count = 1u;
    }

    for (i = 0u; i < aggregate_count; i++) {
        rc = scribe_store_put_aggregate_event_route(
            state->read_store,
            "member_coverage",
            aggregate_ids[i],
            event_id
        );
        if (rc == X12_OK) {
            rc = scribe_store_mark_dirty_aggregate(
                state->read_store,
                "member_coverage",
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
    coverage_state_t *state,
    const journal_event_t *event,
    size_t numeric_event_id,
    long long event_offset,
    long long event_length
)
{
    char event_id[SCRIBE_STORE_ID_MAX];
    char event_type[96];
    char segment_id[SCRIBE_STORE_ID_MAX];
    char member_id[COVERAGE_ID_MAX];
    char member_id_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_id[COVERAGE_ID_MAX];
    char payer_id_token[TOKENISE_MAX_TOKEN_LEN];
    char eligibility_id[COVERAGE_ID_MAX];
    char service_type_code[32];
    const char *member_key;
    const char *payer_key;
    int written;
    int rc;

    if (state == NULL || state->read_store == NULL) {
        return X12_OK;
    }
    if (state->current_source_drop_id[0] == '\0') {
        return X12_OK;
    }
    if (!json_get_string(event, "event_type", event_type, sizeof(event_type))) {
        return X12_OK;
    }

    rc = stable_event_id(event, numeric_event_id, event_id, sizeof(event_id));
    if (rc != X12_OK) {
        return rc;
    }
    if (event->segment_path != NULL && event->segment_path[0] != '\0') {
        copy_cstr(segment_id, sizeof(segment_id), event->segment_path);
    } else if (!json_get_number_text(event, "source_segment_index", segment_id, sizeof(segment_id))) {
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

    member_id[0] = '\0';
    member_id_token[0] = '\0';
    service_type_code[0] = '\0';
    (void)json_get_string(event, "member_id", member_id, sizeof(member_id));
    (void)json_get_string(event, "member_id_token", member_id_token, sizeof(member_id_token));
    if (member_id[0] == '\0') {
        (void)json_get_string(event, "id_value", member_id, sizeof(member_id));
        (void)json_get_string(event, "id_value_token", member_id_token, sizeof(member_id_token));
    }
    member_key = member_id_token[0] != '\0' ? member_id_token : member_id;
    rc = put_event_key_if_present(state->read_store, "member_id", member_key, event_id);
    if (rc != X12_OK) {
        return rc;
    }
    rc = mark_member_coverage_dirty_for_key(state, "member_id", member_key, event_id);
    if (rc != X12_OK) {
        return rc;
    }

    payer_id[0] = '\0';
    payer_id_token[0] = '\0';
    (void)json_get_string(event, "payer_id", payer_id, sizeof(payer_id));
    (void)json_get_string(event, "payer_id_token", payer_id_token, sizeof(payer_id_token));
    payer_key = payer_id_token[0] != '\0' ? payer_id_token : payer_id;
    rc = put_event_key_if_present(state->read_store, "payer_id", payer_key, event_id);
    if (rc != X12_OK) {
        return rc;
    }
    rc = mark_member_coverage_dirty_for_key(state, "payer_id", payer_key, event_id);
    if (rc != X12_OK) {
        return rc;
    }

    if (json_get_string(event, "eligibility_id", eligibility_id, sizeof(eligibility_id))) {
        rc = put_event_key_if_present(state->read_store, "eligibility_id", eligibility_id, event_id);
    }
    if (rc == X12_OK &&
        json_get_string(event, "service_type_code", service_type_code, sizeof(service_type_code))) {
        rc = put_event_key_if_present(
            state->read_store,
            "service_type_code",
            service_type_code,
            event_id
        );
    }
    if (rc == X12_OK) {
        rc = mark_member_coverage_dirty_for_key(
            state,
            "service_type_code",
            service_type_code,
            event_id
        );
    }

    return rc;
}

static int apply_pending_enrollment(member_coverage_t *coverage, const coverage_state_t *state)
{
    if (coverage == NULL || state == NULL || !state->has_pending_enrollment) {
        return X12_OK;
    }

    copy_cstr(coverage->relationship_code, sizeof(coverage->relationship_code), state->pending_relationship_code);
    copy_cstr(
        coverage->enrollment_maintenance_type_code,
        sizeof(coverage->enrollment_maintenance_type_code),
        state->pending_maintenance_type_code
    );
    copy_cstr(
        coverage->benefit_status_code,
        sizeof(coverage->benefit_status_code),
        state->pending_benefit_status_code
    );
    return X12_OK;
}

static int apply_member_referenced(
    coverage_state_t *state,
    const journal_event_t *event,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    member_coverage_t *coverage;
    char member_id[COVERAGE_ID_MAX];
    char member_id_token[TOKENISE_MAX_TOKEN_LEN];
    char last_name_or_org[COVERAGE_VALUE_MAX];
    char first_name[COVERAGE_VALUE_MAX];
    char member_name[COVERAGE_VALUE_MAX];
    int rc;

    rc = extract_value_token_pair(
        state,
        event,
        "id_value",
        "id_value_token",
        "member_id",
        member_id,
        sizeof(member_id),
        member_id_token,
        sizeof(member_id_token)
    );
    if (rc != X12_OK) {
        return rc;
    }
    if (member_id[0] == '\0' && member_id_token[0] == '\0') {
        return X12_OK;
    }

    copy_cstr(state->current_member_id, sizeof(state->current_member_id), member_id);
    copy_cstr(state->current_member_id_token, sizeof(state->current_member_id_token), member_id_token);
    coverage = find_or_add_coverage(state, member_id, member_id_token);
    if (coverage == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    if (state->include_phi && member_id[0] != '\0') {
        copy_cstr(coverage->member_id, sizeof(coverage->member_id), member_id);
    }
    if (member_id_token[0] != '\0') {
        copy_cstr(coverage->member_id_token, sizeof(coverage->member_id_token), member_id_token);
    }
    if (state->current_payer_id_token[0] != '\0' || state->current_payer_id[0] != '\0') {
        if (state->include_phi) {
            copy_cstr(coverage->payer_id, sizeof(coverage->payer_id), state->current_payer_id);
        }
        copy_cstr(coverage->payer_id_token, sizeof(coverage->payer_id_token), state->current_payer_id_token);
    }

    last_name_or_org[0] = '\0';
    first_name[0] = '\0';
    (void)json_get_string(event, "last_name_or_org", last_name_or_org, sizeof(last_name_or_org));
    (void)json_get_string(event, "first_name", first_name, sizeof(first_name));
    rc = make_member_name(last_name_or_org, first_name, member_name, sizeof(member_name));
    if (rc == X12_OK) {
        rc = set_member_name(state, coverage, member_name);
    }
    if (rc == X12_OK) {
        rc = resolve_member_name_by_id(state, coverage);
    }
    if (rc == X12_OK) {
        rc = apply_pending_enrollment(coverage, state);
    }
    if (rc != X12_OK) {
        return rc;
    }

    return record_coverage_update(state, coverage, event_id, journal_offset, journal_length, event_type);
}

static int apply_enrollment_changed(coverage_state_t *state, const journal_event_t *event)
{
    if (state == NULL || event == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    state->current_member_id[0] = '\0';
    state->current_member_id_token[0] = '\0';
    state->has_pending_enrollment = 1;
    (void)json_get_string(
        event,
        "relationship_code",
        state->pending_relationship_code,
        sizeof(state->pending_relationship_code)
    );
    (void)json_get_string(
        event,
        "maintenance_type_code",
        state->pending_maintenance_type_code,
        sizeof(state->pending_maintenance_type_code)
    );
    (void)json_get_string(
        event,
        "benefit_status_code",
        state->pending_benefit_status_code,
        sizeof(state->pending_benefit_status_code)
    );

    return X12_OK;
}

static int apply_coverage_date(
    coverage_state_t *state,
    const journal_event_t *event,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    member_coverage_t *coverage;
    char qualifier[32];
    char value[32];
    int rc;

    rc = current_coverage(state, &coverage);
    if (rc != X12_OK || coverage == NULL) {
        return rc;
    }
    if (!json_get_string(event, "date_qualifier", qualifier, sizeof(qualifier)) ||
        !json_get_string(event, "date_value", value, sizeof(value))) {
        return X12_OK;
    }

    if (strcmp(qualifier, "348") == 0) {
        copy_cstr(coverage->coverage_effective_date, sizeof(coverage->coverage_effective_date), value);
    } else if (strcmp(qualifier, "349") == 0) {
        copy_cstr(coverage->coverage_termination_date, sizeof(coverage->coverage_termination_date), value);
    } else {
        copy_cstr(
            coverage->last_coverage_date_qualifier,
            sizeof(coverage->last_coverage_date_qualifier),
            qualifier
        );
        copy_cstr(coverage->last_coverage_date, sizeof(coverage->last_coverage_date), value);
    }

    return record_coverage_update(state, coverage, event_id, journal_offset, journal_length, event_type);
}

static int apply_health_coverage(
    coverage_state_t *state,
    const journal_event_t *event,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    member_coverage_t *coverage;
    int rc;

    rc = current_coverage(state, &coverage);
    if (rc != X12_OK || coverage == NULL) {
        return rc;
    }

    (void)json_get_string(
        event,
        "maintenance_type_code",
        coverage->health_coverage_maintenance_type_code,
        sizeof(coverage->health_coverage_maintenance_type_code)
    );
    (void)json_get_string(
        event,
        "insurance_line_code",
        coverage->insurance_line_code,
        sizeof(coverage->insurance_line_code)
    );
    (void)json_get_string(
        event,
        "plan_coverage_description",
        coverage->plan_coverage_description,
        sizeof(coverage->plan_coverage_description)
    );
    (void)json_get_string(
        event,
        "coverage_level_code",
        coverage->coverage_level_code,
        sizeof(coverage->coverage_level_code)
    );

    return record_coverage_update(state, coverage, event_id, journal_offset, journal_length, event_type);
}

static int apply_eligibility_observed(coverage_state_t *state, const journal_event_t *event)
{
    if (state == NULL || event == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    state->current_member_id[0] = '\0';
    state->current_member_id_token[0] = '\0';
    state->current_payer_id[0] = '\0';
    state->current_payer_id_token[0] = '\0';
    state->current_inquiry_date[0] = '\0';
    state->current_response_as_of_date[0] = '\0';
    state->has_current_benefit = 0;
    (void)json_get_string(
        event,
        "eligibility_id",
        state->current_eligibility_id,
        sizeof(state->current_eligibility_id)
    );

    return X12_OK;
}

static int apply_eligibility_party(
    coverage_state_t *state,
    const journal_event_t *event,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    char entity_identifier_code[16];
    char raw_id[COVERAGE_ID_MAX];
    char id_token[TOKENISE_MAX_TOKEN_LEN];
    char last_name_or_org[COVERAGE_VALUE_MAX];
    char first_name[COVERAGE_VALUE_MAX];
    char member_name[COVERAGE_VALUE_MAX];
    member_coverage_t *coverage;
    const char *namespace_name;
    int is_member;
    int rc;

    if (!json_get_string(event, "entity_identifier_code", entity_identifier_code, sizeof(entity_identifier_code))) {
        return X12_OK;
    }
    is_member = strcmp(entity_identifier_code, "IL") == 0 || strcmp(entity_identifier_code, "QC") == 0;
    if (is_member) {
        namespace_name = "member_id";
    } else if (strcmp(entity_identifier_code, "PR") == 0) {
        namespace_name = "payer_id";
    } else {
        return X12_OK;
    }

    rc = extract_value_token_pair(
        state,
        event,
        "id_value",
        "id_value_token",
        namespace_name,
        raw_id,
        sizeof(raw_id),
        id_token,
        sizeof(id_token)
    );
    if (rc != X12_OK) {
        return rc;
    }

    if (!is_member) {
        copy_cstr(state->current_payer_id, sizeof(state->current_payer_id), raw_id);
        copy_cstr(state->current_payer_id_token, sizeof(state->current_payer_id_token), id_token);
        return X12_OK;
    }

    copy_cstr(state->current_member_id, sizeof(state->current_member_id), raw_id);
    copy_cstr(state->current_member_id_token, sizeof(state->current_member_id_token), id_token);
    coverage = find_or_add_coverage(state, raw_id, id_token);
    if (coverage == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    if (state->include_phi) {
        copy_cstr(coverage->member_id, sizeof(coverage->member_id), raw_id);
        copy_cstr(coverage->payer_id, sizeof(coverage->payer_id), state->current_payer_id);
    }
    copy_cstr(coverage->member_id_token, sizeof(coverage->member_id_token), id_token);
    copy_cstr(coverage->payer_id_token, sizeof(coverage->payer_id_token), state->current_payer_id_token);

    last_name_or_org[0] = '\0';
    first_name[0] = '\0';
    (void)json_get_string(event, "last_name_or_org", last_name_or_org, sizeof(last_name_or_org));
    (void)json_get_string(event, "first_name", first_name, sizeof(first_name));
    rc = make_member_name(last_name_or_org, first_name, member_name, sizeof(member_name));
    if (rc == X12_OK) {
        rc = set_member_name(state, coverage, member_name);
    }
    if (rc == X12_OK) {
        rc = resolve_member_name_by_id(state, coverage);
    }
    if (rc != X12_OK) {
        return rc;
    }

    return record_coverage_update(state, coverage, event_id, journal_offset, journal_length, event_type);
}

static int apply_demographics(
    coverage_state_t *state,
    const journal_event_t *event,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    member_coverage_t *coverage;
    char dob[COVERAGE_ID_MAX];
    char dob_token[TOKENISE_MAX_TOKEN_LEN];
    int rc;

    rc = current_coverage(state, &coverage);
    if (rc != X12_OK || coverage == NULL) {
        return rc;
    }

    rc = extract_value_token_pair(
        state,
        event,
        "date_of_birth",
        "date_of_birth_token",
        "member_dob",
        dob,
        sizeof(dob),
        dob_token,
        sizeof(dob_token)
    );
    if (rc != X12_OK) {
        return rc;
    }
    if (state->include_phi) {
        copy_cstr(coverage->date_of_birth, sizeof(coverage->date_of_birth), dob);
    }
    copy_cstr(coverage->date_of_birth_token, sizeof(coverage->date_of_birth_token), dob_token);
    if (state->include_phi) {
        (void)json_get_string(event, "gender_code", coverage->gender_code, sizeof(coverage->gender_code));
    }

    return record_coverage_update(state, coverage, event_id, journal_offset, journal_length, event_type);
}

static int apply_eligibility_date(
    coverage_state_t *state,
    const journal_event_t *event,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    member_coverage_t *coverage;
    coverage_benefit_t *benefit;
    char date_scope[32];
    char qualifier[32];
    char value[32];
    int rc;

    if (!json_get_string(event, "date_qualifier", qualifier, sizeof(qualifier)) ||
        !json_get_string(event, "date_value", value, sizeof(value))) {
        return X12_OK;
    }
    (void)json_get_string(event, "date_scope", date_scope, sizeof(date_scope));

    rc = current_coverage(state, &coverage);
    if (rc != X12_OK) {
        return rc;
    }
    if (coverage == NULL) {
        return X12_OK;
    }

    if (strcmp(event_type, "EligibilityInquiryDateRecorded") == 0) {
        if (strcmp(qualifier, "291") == 0) {
            copy_cstr(state->current_inquiry_date, sizeof(state->current_inquiry_date), value);
        }
    } else if (strcmp(date_scope, "benefit") == 0 && state->has_current_benefit) {
        benefit = &coverage->benefits[state->current_benefit_index];
        if (strcmp(qualifier, "346") == 0) {
            copy_cstr(benefit->effective_date, sizeof(benefit->effective_date), value);
        } else if (strcmp(qualifier, "347") == 0) {
            copy_cstr(benefit->termination_date, sizeof(benefit->termination_date), value);
        }
    } else if (strcmp(qualifier, "291") == 0) {
        copy_cstr(state->current_response_as_of_date, sizeof(state->current_response_as_of_date), value);
    }

    return record_coverage_update(state, coverage, event_id, journal_offset, journal_length, event_type);
}

static int apply_service_type_requested(
    coverage_state_t *state,
    const journal_event_t *event,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    member_coverage_t *coverage;
    coverage_service_request_t *request;
    char service_type_code[32];
    int rc;

    rc = current_coverage(state, &coverage);
    if (rc != X12_OK || coverage == NULL) {
        return rc;
    }
    if (!json_get_string(event, "service_type_code", service_type_code, sizeof(service_type_code))) {
        return X12_OK;
    }
    if (coverage->service_request_count >= COVERAGE_MAX_SERVICE_REQUESTS) {
        return X12_ERR_NO_MEMORY;
    }

    request = &coverage->service_requests[coverage->service_request_count++];
    memset(request, 0, sizeof(*request));
    copy_cstr(request->eligibility_id, sizeof(request->eligibility_id), state->current_eligibility_id);
    if (state->include_phi) {
        copy_cstr(request->payer_id, sizeof(request->payer_id), state->current_payer_id);
    }
    copy_cstr(request->payer_id_token, sizeof(request->payer_id_token), state->current_payer_id_token);
    copy_cstr(request->service_type_code, sizeof(request->service_type_code), service_type_code);
    copy_cstr(request->inquiry_date, sizeof(request->inquiry_date), state->current_inquiry_date);

    return record_coverage_update(state, coverage, event_id, journal_offset, journal_length, event_type);
}

static int apply_benefit_observed(
    coverage_state_t *state,
    const journal_event_t *event,
    size_t event_id,
    long long journal_offset,
    long long journal_length,
    const char *event_type
)
{
    member_coverage_t *coverage;
    coverage_benefit_t *benefit;
    int rc;

    rc = current_coverage(state, &coverage);
    if (rc != X12_OK || coverage == NULL) {
        return rc;
    }
    if (coverage->benefit_count >= COVERAGE_MAX_BENEFITS) {
        return X12_ERR_NO_MEMORY;
    }

    benefit = &coverage->benefits[coverage->benefit_count];
    memset(benefit, 0, sizeof(*benefit));
    copy_cstr(benefit->eligibility_id, sizeof(benefit->eligibility_id), state->current_eligibility_id);
    if (state->include_phi) {
        copy_cstr(benefit->payer_id, sizeof(benefit->payer_id), state->current_payer_id);
    }
    copy_cstr(benefit->payer_id_token, sizeof(benefit->payer_id_token), state->current_payer_id_token);
    (void)json_get_string(event, "service_type_code", benefit->service_type_code, sizeof(benefit->service_type_code));
    (void)json_get_string(
        event,
        "eligibility_or_benefit_information_code",
        benefit->eligibility_or_benefit_information_code,
        sizeof(benefit->eligibility_or_benefit_information_code)
    );
    (void)json_get_string(event, "coverage_level_code", benefit->coverage_level_code, sizeof(benefit->coverage_level_code));
    (void)json_get_string(event, "insurance_type_code", benefit->insurance_type_code, sizeof(benefit->insurance_type_code));
    (void)json_get_string(
        event,
        "plan_coverage_description",
        benefit->plan_coverage_description,
        sizeof(benefit->plan_coverage_description)
    );
    (void)json_get_string(
        event,
        "time_period_qualifier",
        benefit->time_period_qualifier,
        sizeof(benefit->time_period_qualifier)
    );
    (void)json_get_string(event, "monetary_amount", benefit->monetary_amount, sizeof(benefit->monetary_amount));
    (void)json_get_string(event, "percent", benefit->percent, sizeof(benefit->percent));
    (void)json_get_string(event, "quantity_qualifier", benefit->quantity_qualifier, sizeof(benefit->quantity_qualifier));
    (void)json_get_string(event, "quantity", benefit->quantity, sizeof(benefit->quantity));
    (void)json_get_string(
        event,
        "authorization_or_certification_indicator",
        benefit->authorization_or_certification_indicator,
        sizeof(benefit->authorization_or_certification_indicator)
    );
    (void)json_get_string(
        event,
        "in_plan_network_indicator",
        benefit->in_plan_network_indicator,
        sizeof(benefit->in_plan_network_indicator)
    );
    copy_cstr(
        benefit->response_as_of_date,
        sizeof(benefit->response_as_of_date),
        state->current_response_as_of_date
    );
    state->current_benefit_index = coverage->benefit_count;
    state->has_current_benefit = 1;
    coverage->benefit_count++;

    return record_coverage_update(state, coverage, event_id, journal_offset, journal_length, event_type);
}

static int apply_coverage_event(
    coverage_state_t *state,
    const journal_event_t *event,
    size_t event_id,
    long long journal_offset,
    long long journal_length
)
{
    char event_type[96];

    if (state == NULL || event == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (!json_get_string(event, "event_type", event_type, sizeof(event_type))) {
        return X12_OK;
    }

    if (strcmp(event_type, "MemberEnrollmentChanged") == 0) {
        return apply_enrollment_changed(state, event);
    }
    if (strcmp(event_type, "MemberReferenced") == 0) {
        return apply_member_referenced(state, event, event_id, journal_offset, journal_length, event_type);
    }
    if (strcmp(event_type, "CoverageDateObserved") == 0) {
        return apply_coverage_date(state, event, event_id, journal_offset, journal_length, event_type);
    }
    if (strcmp(event_type, "HealthCoverageObserved") == 0) {
        return apply_health_coverage(state, event, event_id, journal_offset, journal_length, event_type);
    }
    if (strcmp(event_type, "EligibilityInquiryObserved") == 0 ||
        strcmp(event_type, "EligibilityResponseObserved") == 0) {
        return apply_eligibility_observed(state, event);
    }
    if (strcmp(event_type, "EligibilityInquiryPartyReferenced") == 0 ||
        strcmp(event_type, "EligibilityResponsePartyReferenced") == 0) {
        return apply_eligibility_party(state, event, event_id, journal_offset, journal_length, event_type);
    }
    if (strcmp(event_type, "EligibilityInquiryDemographicsObserved") == 0 ||
        strcmp(event_type, "EligibilityResponseDemographicsObserved") == 0) {
        return apply_demographics(state, event, event_id, journal_offset, journal_length, event_type);
    }
    if (strcmp(event_type, "EligibilityInquiryDateRecorded") == 0 ||
        strcmp(event_type, "EligibilityResponseDateRecorded") == 0) {
        return apply_eligibility_date(state, event, event_id, journal_offset, journal_length, event_type);
    }
    if (strcmp(event_type, "EligibilityInquiryServiceTypeRequested") == 0) {
        return apply_service_type_requested(state, event, event_id, journal_offset, journal_length, event_type);
    }
    if (strcmp(event_type, "EligibilityBenefitObserved") == 0) {
        return apply_benefit_observed(state, event, event_id, journal_offset, journal_length, event_type);
    }

    return X12_OK;
}

void coverage_stitcher_input_init(coverage_stitcher_input_t *input)
{
    if (input != NULL) {
        memset(input, 0, sizeof(*input));
    }
}

int coverage_stitcher_stitch(const coverage_stitcher_input_t *input)
{
    coverage_state_t *state;
    scribe_store_t read_store;
    phi_vault_t phi_vault;
    journal_reader_t journal;
    journal_event_t record;
    FILE *out;
    char drop_key[COVERAGE_FINGERPRINT_MAX];
    char generated_run_id[96];
    size_t event_id = 0u;
    int owns_out = 0;
    int owns_read_store = 0;
    int owns_phi_vault = 0;
    int reduce_pass;
    int rc = X12_OK;

    if (input == NULL || input->journal_path == NULL || input->out_path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (input->incremental && input->read_store_path == NULL) {
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

    state = (coverage_state_t *)calloc(1u, sizeof(*state));
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
        "scribe stitch coverage: start mode=%s journal=%s read_store=%s out=%s\n",
        input->incremental ? "incremental" : "replay",
        input->journal_path,
        input->read_store_path == NULL ? "(none)" : input->read_store_path,
        input->out_path
    );
    if (input->run_id != NULL && input->run_id[0] != '\0') {
        copy_cstr(state->run_id, sizeof(state->run_id), input->run_id);
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
                "scribe stitch coverage: %s pass journal=%s\n",
                reduce_pass ? "reduce" : "shuffle",
                input->journal_path
            );
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
                rc = coverage_stitch_flush_update_batches(state);
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
                    "scribe stitch coverage: source_drop=%s source_run=%s\n",
                    state->current_source_drop_id,
                    state->current_source_run_id[0] == '\0' ? "(none)" : state->current_source_run_id
                );
            }
            if (!input->incremental || !reduce_pass) {
                rc = index_journal_event(state, &record, event_id, record.offset, record.stored_len);
                if (rc != X12_OK) {
                    break;
                }
            }
            if (!input->incremental || reduce_pass) {
                rc = apply_coverage_event(state, &record, stable_id, record.offset, record.stored_len);
                if (rc != X12_OK) {
                    break;
                }
            }
        }

        if (rc == X12_OK) {
            rc = coverage_stitch_flush_update_batches(state);
        }
        if (rc != X12_OK) {
            break;
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
        "scribe stitch coverage: done events=%zu dirty_routes=%zu aggregates=%zu status=%s\n",
        event_id,
        state->dirty_route_count,
        state->aggregate_count,
        x12_error_message(rc)
    );
    free(state);
    return rc;
}

int coverage_stitcher_run_cli(int argc, char **argv)
{
    coverage_stitcher_input_t input;
    int i;

    coverage_stitcher_input_init(&input);
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

    return coverage_stitcher_stitch(&input);
}
