#include "balance_projector.h"

#include "event_writer.h"
#include "store.h"
#include "tokenise.h"
#include "yyjson.h"

#include <stdio.h>
#include <string.h>

#define BALANCE_ID_MAX 128u
#define BALANCE_DESC_MAX 256u
#define BALANCE_MAX_CLAIMS 64u
#define BALANCE_MAX_LINES_PER_CLAIM 64u

typedef struct {
    char service_line_number[32];
    char remit_service_line_number[32];
    char procedure_code[32];
    char description[BALANCE_DESC_MAX];
    char service_date[32];
    char match_method[48];
    long long billed;
    long long payer_paid;
    long long contractual_adjustments;
    long long patient_responsibility;
} balance_service_line_t;

typedef struct {
    char key[BALANCE_ID_MAX];
    char claim_id[BALANCE_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char claim_type[64];
    char payer_claim_control_number[BALANCE_ID_MAX];
    char payer_claim_control_number_token[TOKENISE_MAX_TOKEN_LEN];
    char claim_status_code[32];
    long long claim_total_billed;
    long long claim_paid;
    long long claim_patient_responsibility;
    balance_service_line_t lines[BALANCE_MAX_LINES_PER_CLAIM];
    size_t line_count;
} balance_claim_t;

typedef struct {
    int include_phi;
    balance_claim_t claims[BALANCE_MAX_CLAIMS];
    size_t claim_count;
} balance_state_t;

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

static const char *aggregate_key_from_id(const char *aggregate_id)
{
    if (aggregate_id != NULL && strncmp(aggregate_id, "claim:", 6u) == 0) {
        return aggregate_id + 6u;
    }
    return aggregate_id == NULL ? "" : aggregate_id;
}

static int parse_money(const char *value, long long *out)
{
    long long dollars = 0;
    long long cents = 0;
    int negative = 0;
    int cent_digits = 0;
    const char *cursor;

    if (out == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out = 0;
    if (value == NULL || value[0] == '\0') {
        return X12_OK;
    }

    cursor = value;
    if (*cursor == '-') {
        negative = 1;
        cursor++;
    }

    while (*cursor >= '0' && *cursor <= '9') {
        dollars = dollars * 10 + (long long)(*cursor - '0');
        cursor++;
    }
    if (*cursor == '.') {
        cursor++;
        while (*cursor >= '0' && *cursor <= '9' && cent_digits < 2) {
            cents = cents * 10 + (long long)(*cursor - '0');
            cent_digits++;
            cursor++;
        }
    }
    while (cent_digits < 2) {
        cents *= 10;
        cent_digits++;
    }

    *out = dollars * 100 + cents;
    if (negative) {
        *out = -*out;
    }
    return X12_OK;
}

static void format_money(long long cents, char *out, size_t out_len)
{
    long long abs_cents = cents;
    const char *sign = "";

    if (cents < 0) {
        sign = "-";
        abs_cents = -cents;
    }

    (void)snprintf(out, out_len, "%s%lld.%02lld", sign, abs_cents / 100, abs_cents % 100);
}

static void snapshot_get_string(yyjson_val *obj, const char *key, char *out, size_t out_len)
{
    yyjson_val *value;

    if (out == NULL || out_len == 0u) {
        return;
    }
    if (obj == NULL || !yyjson_is_obj(obj)) {
        out[0] = '\0';
        return;
    }

    value = yyjson_obj_get(obj, key);
    if (value == NULL || !yyjson_is_str(value)) {
        out[0] = '\0';
        return;
    }
    copy_cstr(out, out_len, yyjson_get_str(value));
}

static int snapshot_money_field(yyjson_val *obj, const char *key, long long *out)
{
    yyjson_val *value;

    if (obj == NULL || !yyjson_is_obj(obj)) {
        return X12_OK;
    }

    value = yyjson_obj_get(obj, key);
    if (value == NULL || !yyjson_is_str(value)) {
        return X12_OK;
    }
    return parse_money(yyjson_get_str(value), out);
}

static int snapshot_money_array_sum(yyjson_val *arr, long long *out)
{
    yyjson_val *value;
    size_t idx;
    size_t max;
    long long amount;
    int rc;

    if (out == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out = 0;
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    yyjson_arr_foreach(arr, idx, max, value) {
        if (yyjson_is_str(value)) {
            rc = parse_money(yyjson_get_str(value), &amount);
            if (rc != X12_OK) {
                return rc;
            }
            *out += amount;
        }
    }

    return X12_OK;
}

static balance_claim_t *add_claim(balance_state_t *state, const char *aggregate_id)
{
    balance_claim_t *claim;
    const char *key;

    if (state == NULL || state->claim_count >= BALANCE_MAX_CLAIMS) {
        return NULL;
    }

    claim = &state->claims[state->claim_count++];
    memset(claim, 0, sizeof(*claim));
    key = aggregate_key_from_id(aggregate_id);
    copy_cstr(claim->key, sizeof(claim->key), key);
    copy_cstr(claim->claim_id, sizeof(claim->claim_id), key);
    return claim;
}

static balance_service_line_t *add_line(balance_claim_t *claim)
{
    balance_service_line_t *line;

    if (claim == NULL || claim->line_count >= BALANCE_MAX_LINES_PER_CLAIM) {
        return NULL;
    }

    line = &claim->lines[claim->line_count++];
    memset(line, 0, sizeof(*line));
    copy_cstr(line->match_method, sizeof(line->match_method), "unmatched");
    return line;
}

static int apply_snapshot_adjustments(balance_service_line_t *line, yyjson_val *line_obj)
{
    yyjson_val *adjustments;
    yyjson_val *adjustment;
    yyjson_val *amounts;
    char group_code[32];
    size_t idx;
    size_t max;
    long long amount;
    int rc;

    adjustments = yyjson_obj_get(line_obj, "adjustments");
    if (adjustments == NULL || !yyjson_is_arr(adjustments)) {
        return X12_OK;
    }

    yyjson_arr_foreach(adjustments, idx, max, adjustment) {
        if (!yyjson_is_obj(adjustment)) {
            continue;
        }
        snapshot_get_string(
            adjustment,
            "adjustment_group_code",
            group_code,
            sizeof(group_code)
        );
        amounts = yyjson_obj_get(adjustment, "amounts");
        rc = snapshot_money_array_sum(amounts, &amount);
        if (rc != X12_OK) {
            return rc;
        }

        if (strcmp(group_code, "CO") == 0) {
            line->contractual_adjustments += amount;
        } else if (strcmp(group_code, "PR") == 0) {
            line->patient_responsibility += amount;
        }
    }

    return X12_OK;
}

static int apply_snapshot_line(balance_claim_t *claim, yyjson_val *line_obj)
{
    balance_service_line_t *line;
    yyjson_val *submitted;
    yyjson_val *remittance;
    long long amount = 0;
    int rc;

    if (claim == NULL || line_obj == NULL || !yyjson_is_obj(line_obj)) {
        return X12_OK;
    }

    line = add_line(claim);
    if (line == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    snapshot_get_string(
        line_obj,
        "service_line_number",
        line->service_line_number,
        sizeof(line->service_line_number)
    );
    snapshot_get_string(
        line_obj,
        "remittance_service_line_number",
        line->remit_service_line_number,
        sizeof(line->remit_service_line_number)
    );
    snapshot_get_string(line_obj, "procedure_code", line->procedure_code, sizeof(line->procedure_code));
    snapshot_get_string(line_obj, "description", line->description, sizeof(line->description));
    snapshot_get_string(line_obj, "service_date", line->service_date, sizeof(line->service_date));
    snapshot_get_string(line_obj, "match_method", line->match_method, sizeof(line->match_method));
    if (line->match_method[0] == '\0') {
        copy_cstr(line->match_method, sizeof(line->match_method), "unmatched");
    }

    submitted = yyjson_obj_get(line_obj, "submitted");
    rc = snapshot_money_field(submitted, "charge_amount", &line->billed);
    if (rc != X12_OK) {
        return rc;
    }

    remittance = yyjson_obj_get(line_obj, "remittance");
    if (line->billed == 0) {
        rc = snapshot_money_field(remittance, "line_charge_amount", &amount);
        if (rc != X12_OK) {
            return rc;
        }
        line->billed = amount;
    }
    rc = snapshot_money_field(remittance, "line_paid_amount", &line->payer_paid);
    if (rc != X12_OK) {
        return rc;
    }

    return apply_snapshot_adjustments(line, line_obj);
}

static int apply_claim_snapshot(
    balance_state_t *state,
    const char *aggregate_id,
    const char *state_json
)
{
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *keys;
    yyjson_val *snapshot_state;
    yyjson_val *claim_envelope;
    yyjson_val *service_lines;
    yyjson_val *line_obj;
    balance_claim_t *claim;
    size_t idx;
    size_t max;
    int rc = X12_OK;

    if (state == NULL || aggregate_id == NULL || state_json == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    doc = yyjson_read(state_json, strlen(state_json), 0);
    if (doc == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    root = yyjson_doc_get_root(doc);
    if (root == NULL || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return X12_ERR_INVALID_ARGUMENT;
    }

    claim = add_claim(state, aggregate_id);
    if (claim == NULL) {
        yyjson_doc_free(doc);
        return X12_ERR_NO_MEMORY;
    }

    keys = yyjson_obj_get(root, "keys");
    if (keys != NULL && yyjson_is_obj(keys)) {
        snapshot_get_string(keys, "claim_id", claim->claim_id, sizeof(claim->claim_id));
        snapshot_get_string(
            keys,
            "claim_id_token",
            claim->claim_id_token,
            sizeof(claim->claim_id_token)
        );
        snapshot_get_string(
            keys,
            "payer_claim_control_number",
            claim->payer_claim_control_number,
            sizeof(claim->payer_claim_control_number)
        );
        snapshot_get_string(
            keys,
            "payer_claim_control_number_token",
            claim->payer_claim_control_number_token,
            sizeof(claim->payer_claim_control_number_token)
        );
    }
    if (claim->claim_id[0] == '\0') {
        copy_cstr(claim->claim_id, sizeof(claim->claim_id), claim->key);
    }

    snapshot_state = yyjson_obj_get(root, "state");
    if (snapshot_state != NULL && yyjson_is_obj(snapshot_state)) {
        snapshot_get_string(
            snapshot_state,
            "claim_type",
            claim->claim_type,
            sizeof(claim->claim_type)
        );
        snapshot_get_string(
            snapshot_state,
            "claim_status_code",
            claim->claim_status_code,
            sizeof(claim->claim_status_code)
        );
        claim_envelope = yyjson_obj_get(snapshot_state, "claim_envelope");
        rc = snapshot_money_field(
            claim_envelope,
            "total_charge_amount",
            &claim->claim_total_billed
        );
    }
    if (rc == X12_OK) {
        service_lines = yyjson_obj_get(root, "service_lines");
        if (service_lines != NULL && yyjson_is_arr(service_lines)) {
            yyjson_arr_foreach(service_lines, idx, max, line_obj) {
                rc = apply_snapshot_line(claim, line_obj);
                if (rc != X12_OK) {
                    break;
                }
            }
        }
    }

    yyjson_doc_free(doc);
    return rc;
}

static int latest_claim_callback(
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    void *user
)
{
    (void)version;
    return apply_claim_snapshot((balance_state_t *)user, aggregate_id, state_json);
}

static int read_store_pass(balance_state_t *state, const char *path)
{
    scribe_store_t store;
    int opened = 0;
    int rc;

    if (state == NULL || path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    scribe_store_init(&store);
    rc = scribe_store_open(&store, path);
    if (rc == X12_OK) {
        opened = 1;
        rc = scribe_store_init_schema(&store);
    }
    if (rc == X12_OK) {
        rc = scribe_store_each_latest_claim_aggregate(&store, latest_claim_callback, state);
    }
    if (opened && scribe_store_close(&store) != X12_OK && rc == X12_OK) {
        rc = X12_ERR_IO;
    }

    return rc;
}

static long long line_current_balance(const balance_service_line_t *line)
{
    return line->billed -
           line->payer_paid -
           line->contractual_adjustments;
}

static void claim_totals(
    const balance_claim_t *claim,
    long long *billed,
    long long *paid,
    long long *contractual,
    long long *patient_resp
)
{
    size_t i;

    *billed = 0;
    *paid = 0;
    *contractual = 0;
    *patient_resp = 0;

    for (i = 0u; i < claim->line_count; i++) {
        *billed += claim->lines[i].billed;
        *paid += claim->lines[i].payer_paid;
        *contractual += claim->lines[i].contractual_adjustments;
        *patient_resp += claim->lines[i].patient_responsibility;
    }

    if (*billed == 0) {
        *billed = claim->claim_total_billed;
    }
    if (*paid == 0) {
        *paid = claim->claim_paid;
    }
    if (*patient_resp == 0) {
        *patient_resp = claim->claim_patient_responsibility;
    }
}

static int write_money_field(FILE *fp, const char *name, long long value, int prefix_comma)
{
    char formatted[32];

    format_money(value, formatted, sizeof(formatted));
    return event_writer_write_cstring_field(fp, name, formatted, prefix_comma);
}

static int write_ledger_entry(
    FILE *fp,
    const char *entry_type,
    const char *source,
    long long amount,
    long long balance_effect,
    int prefix_comma
)
{
    if (prefix_comma && fputc(',', fp) == EOF) {
        return X12_ERR_IO;
    }
    if (fputc('{', fp) == EOF) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "entry_type", entry_type, 0) != X12_OK ||
        event_writer_write_cstring_field(fp, "source", source, 1) != X12_OK ||
        write_money_field(fp, "amount", amount, 1) != X12_OK ||
        write_money_field(fp, "balance_effect", balance_effect, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputc('}', fp) == EOF) {
        return X12_ERR_IO;
    }
    return X12_OK;
}

static int write_line_ledger_entries(FILE *fp, const balance_service_line_t *line)
{
    int wrote = 0;

    if (fputs(",\"ledger_entries\":[", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (line->billed != 0) {
        if (write_ledger_entry(fp, "billed_charge", "837", line->billed, line->billed, wrote) != X12_OK) {
            return X12_ERR_IO;
        }
        wrote = 1;
    }
    if (line->payer_paid != 0) {
        if (write_ledger_entry(fp, "payer_payment", "835", line->payer_paid, -line->payer_paid, wrote) != X12_OK) {
            return X12_ERR_IO;
        }
        wrote = 1;
    }
    if (line->contractual_adjustments != 0) {
        if (write_ledger_entry(
                fp,
                "contractual_adjustment",
                "835 CAS CO",
                line->contractual_adjustments,
                -line->contractual_adjustments,
                wrote
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        wrote = 1;
    }
    if (line->patient_responsibility != 0) {
        if (write_ledger_entry(
                fp,
                "patient_responsibility",
                "835 CAS PR",
                line->patient_responsibility,
                0,
                wrote
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        wrote = 1;
    }

    if (fputc(']', fp) == EOF) {
        return X12_ERR_IO;
    }
    return X12_OK;
}

static int write_claim_identifier(FILE *fp, const balance_claim_t *claim, int include_phi)
{
    if (include_phi) {
        if (event_writer_write_cstring_field(fp, "claim_id", claim->claim_id, 0) != X12_OK) {
            return X12_ERR_IO;
        }
        if (claim->claim_id_token[0] != '\0' &&
            event_writer_write_cstring_field(fp, "claim_id_token", claim->claim_id_token, 1) != X12_OK) {
            return X12_ERR_IO;
        }
        return X12_OK;
    }

    return event_writer_write_cstring_field(fp, "claim_id", claim->key, 0);
}

static int write_payer_control(FILE *fp, const balance_claim_t *claim, int include_phi)
{
    const char *tokenised_value;

    if (claim->payer_claim_control_number[0] == '\0' &&
        claim->payer_claim_control_number_token[0] == '\0') {
        return X12_OK;
    }

    if (include_phi) {
        if (event_writer_write_cstring_field(
                fp,
                "payer_claim_control_number",
                claim->payer_claim_control_number,
                1
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        if (claim->payer_claim_control_number_token[0] != '\0' &&
            event_writer_write_cstring_field(
                fp,
                "payer_claim_control_number_token",
                claim->payer_claim_control_number_token,
                1
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        return X12_OK;
    }

    tokenised_value = claim->payer_claim_control_number_token[0] != '\0' ?
        claim->payer_claim_control_number_token :
        claim->payer_claim_control_number;
    return event_writer_write_cstring_field(fp, "payer_claim_control_number", tokenised_value, 1);
}

static int write_service_line(FILE *fp, const balance_service_line_t *line, int prefix_comma)
{
    long long current_balance = line_current_balance(line);

    if (prefix_comma && fputc(',', fp) == EOF) {
        return X12_ERR_IO;
    }
    if (fputc('{', fp) == EOF) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "service_line_number", line->service_line_number, 0) != X12_OK ||
        event_writer_write_cstring_field(fp, "procedure_code", line->procedure_code, 1) != X12_OK ||
        event_writer_write_cstring_field(fp, "description", line->description, 1) != X12_OK ||
        event_writer_write_cstring_field(fp, "service_date", line->service_date, 1) != X12_OK ||
        event_writer_write_cstring_field(fp, "match_method", line->match_method, 1) != X12_OK ||
        write_money_field(fp, "billed", line->billed, 1) != X12_OK ||
        write_money_field(fp, "payer_paid", line->payer_paid, 1) != X12_OK ||
        write_money_field(fp, "contractual_adjustments", line->contractual_adjustments, 1) != X12_OK ||
        write_money_field(fp, "patient_responsibility", line->patient_responsibility, 1) != X12_OK ||
        write_money_field(fp, "current_balance", current_balance, 1) != X12_OK ||
        write_line_ledger_entries(fp, line) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputc('}', fp) == EOF) {
        return X12_ERR_IO;
    }
    return X12_OK;
}

static int write_projection(FILE *fp, const balance_state_t *state)
{
    long long total_billed = 0;
    long long total_paid = 0;
    long long total_contractual = 0;
    long long total_patient_resp = 0;
    size_t i;

    if (fputc('{', fp) == EOF) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "event_type", "ClaimBalanceProjected", 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs(",\"claims\":[", fp) == EOF) {
        return X12_ERR_IO;
    }

    for (i = 0u; i < state->claim_count; i++) {
        const balance_claim_t *claim = &state->claims[i];
        long long billed;
        long long paid;
        long long contractual;
        long long patient_resp;
        long long current_balance;
        size_t j;

        claim_totals(claim, &billed, &paid, &contractual, &patient_resp);
        current_balance = billed - paid - contractual;
        total_billed += billed;
        total_paid += paid;
        total_contractual += contractual;
        total_patient_resp += patient_resp;

        if (i > 0u && fputc(',', fp) == EOF) {
            return X12_ERR_IO;
        }
        if (fputc('{', fp) == EOF) {
            return X12_ERR_IO;
        }
        if (write_claim_identifier(fp, claim, state->include_phi) != X12_OK ||
            write_payer_control(fp, claim, state->include_phi) != X12_OK ||
            event_writer_write_cstring_field(fp, "claim_type", claim->claim_type, 1) != X12_OK ||
            event_writer_write_cstring_field(fp, "claim_status_code", claim->claim_status_code, 1) != X12_OK ||
            write_money_field(fp, "total_billed", billed, 1) != X12_OK ||
            write_money_field(fp, "payer_paid", paid, 1) != X12_OK ||
            write_money_field(fp, "contractual_adjustments", contractual, 1) != X12_OK ||
            write_money_field(fp, "patient_responsibility", patient_resp, 1) != X12_OK ||
            write_money_field(fp, "current_balance", current_balance, 1) != X12_OK) {
            return X12_ERR_IO;
        }
        if (fputs(",\"service_lines\":[", fp) == EOF) {
            return X12_ERR_IO;
        }
        for (j = 0u; j < claim->line_count; j++) {
            if (write_service_line(fp, &claim->lines[j], j > 0u) != X12_OK) {
                return X12_ERR_IO;
            }
        }
        if (fputs("]}", fp) == EOF) {
            return X12_ERR_IO;
        }
    }

    if (fputs("],\"totals\":{", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (write_money_field(fp, "total_billed", total_billed, 0) != X12_OK ||
        write_money_field(fp, "payer_paid", total_paid, 1) != X12_OK ||
        write_money_field(fp, "contractual_adjustments", total_contractual, 1) != X12_OK ||
        write_money_field(fp, "patient_responsibility", total_patient_resp, 1) != X12_OK ||
        write_money_field(
            fp,
            "current_balance",
            total_billed - total_paid - total_contractual,
            1
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs("}}\n", fp) == EOF) {
        return X12_ERR_IO;
    }

    return X12_OK;
}

void balance_projector_input_init(balance_projector_input_t *input)
{
    if (input != NULL) {
        memset(input, 0, sizeof(*input));
    }
}

int balance_projector_project(
    const balance_projector_input_t *input,
    const char *out_path
)
{
    balance_state_t state;
    FILE *fp;
    int owns_file = 0;
    int rc;

    if (input == NULL || input->read_store_path == NULL || out_path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(&state, 0, sizeof(state));
    state.include_phi = input->include_phi;

    rc = read_store_pass(&state, input->read_store_path);
    if (rc != X12_OK) {
        return rc;
    }

    if (strcmp(out_path, "-") == 0) {
        fp = stdout;
    } else {
        fp = fopen(out_path, "wb");
        owns_file = 1;
    }
    if (fp == NULL) {
        return X12_ERR_IO;
    }

    rc = write_projection(fp, &state);
    if (fflush(fp) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (owns_file && fclose(fp) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }

    return rc;
}

int balance_projector_run_cli(int argc, char **argv)
{
    balance_projector_input_t input;
    const char *out_path = "-";
    int i;

    balance_projector_input_init(&input);

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--read-store") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.read_store_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--include-phi") == 0) {
            input.include_phi = 1;
        } else {
            return -1;
        }
    }

    if (input.read_store_path == NULL) {
        return -1;
    }

    return balance_projector_project(&input, out_path);
}
