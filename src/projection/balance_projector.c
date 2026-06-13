#include "balance_projector.h"

#include "json_write.h"
#include "store.h"
#include "str_util.h"
#include "try.h"
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

    if (out == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out = 0;
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    yyjson_arr_foreach(arr, idx, max, value) {
        if (yyjson_is_str(value)) {
            TRY(parse_money(yyjson_get_str(value), &amount));
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
    scribe_copy_cstr(claim->key, sizeof(claim->key), key);
    scribe_copy_cstr(claim->claim_id, sizeof(claim->claim_id), key);
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
    scribe_copy_cstr(line->match_method, sizeof(line->match_method), "unmatched");
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

    adjustments = yyjson_obj_get(line_obj, "adjustments");
    if (adjustments == NULL || !yyjson_is_arr(adjustments)) {
        return X12_OK;
    }

    yyjson_arr_foreach(adjustments, idx, max, adjustment) {
        if (!yyjson_is_obj(adjustment)) {
            continue;
        }
        (void)json_read_string(
            adjustment,
            "adjustment_group_code",
            group_code,
            sizeof(group_code)
        );
        amounts = yyjson_obj_get(adjustment, "amounts");
        TRY(snapshot_money_array_sum(amounts, &amount));

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

    if (claim == NULL || line_obj == NULL || !yyjson_is_obj(line_obj)) {
        return X12_OK;
    }

    line = add_line(claim);
    if (line == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    (void)json_read_string(
        line_obj,
        "service_line_number",
        line->service_line_number,
        sizeof(line->service_line_number)
    );
    (void)json_read_string(
        line_obj,
        "remittance_service_line_number",
        line->remit_service_line_number,
        sizeof(line->remit_service_line_number)
    );
    (void)json_read_string(line_obj, "procedure_code", line->procedure_code, sizeof(line->procedure_code));
    (void)json_read_string(line_obj, "description", line->description, sizeof(line->description));
    (void)json_read_string(line_obj, "service_date", line->service_date, sizeof(line->service_date));
    (void)json_read_string(line_obj, "match_method", line->match_method, sizeof(line->match_method));
    if (line->match_method[0] == '\0') {
        scribe_copy_cstr(line->match_method, sizeof(line->match_method), "unmatched");
    }

    submitted = yyjson_obj_get(line_obj, "submitted");
    TRY(snapshot_money_field(submitted, "charge_amount", &line->billed));

    remittance = yyjson_obj_get(line_obj, "remittance");
    if (line->billed == 0) {
        TRY(snapshot_money_field(remittance, "line_charge_amount", &amount));
        line->billed = amount;
    }
    TRY(snapshot_money_field(remittance, "line_paid_amount", &line->payer_paid));

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
        (void)json_read_string(keys, "claim_id", claim->claim_id, sizeof(claim->claim_id));
        (void)json_read_string(
            keys,
            "claim_id_token",
            claim->claim_id_token,
            sizeof(claim->claim_id_token)
        );
        (void)json_read_string(
            keys,
            "payer_claim_control_number",
            claim->payer_claim_control_number,
            sizeof(claim->payer_claim_control_number)
        );
        (void)json_read_string(
            keys,
            "payer_claim_control_number_token",
            claim->payer_claim_control_number_token,
            sizeof(claim->payer_claim_control_number_token)
        );
    }
    if (claim->claim_id[0] == '\0') {
        scribe_copy_cstr(claim->claim_id, sizeof(claim->claim_id), claim->key);
    }

    snapshot_state = yyjson_obj_get(root, "state");
    if (snapshot_state != NULL && yyjson_is_obj(snapshot_state)) {
        (void)json_read_string(
            snapshot_state,
            "claim_type",
            claim->claim_type,
            sizeof(claim->claim_type)
        );
        (void)json_read_string(
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

static int add_money_field(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *name,
    long long value
)
{
    char formatted[32];

    format_money(value, formatted, sizeof(formatted));
    return json_writer_add_string(writer, obj, name, formatted);
}

static int add_ledger_entry(
    json_writer_t *writer,
    yyjson_mut_val *arr,
    const char *entry_type,
    const char *source,
    long long amount,
    long long balance_effect
)
{
    yyjson_mut_val *entry = json_writer_array_add_object(writer, arr);

    if (entry == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    if (json_writer_add_string(writer, entry, "entry_type", entry_type) != X12_OK ||
        json_writer_add_string(writer, entry, "source", source) != X12_OK ||
        add_money_field(writer, entry, "amount", amount) != X12_OK ||
        add_money_field(writer, entry, "balance_effect", balance_effect) != X12_OK) {
        return X12_ERR_IO;
    }
    return X12_OK;
}

static int add_line_ledger_entries(
    json_writer_t *writer,
    yyjson_mut_val *line_obj,
    const balance_service_line_t *line
)
{
    yyjson_mut_val *arr = json_writer_add_array(writer, line_obj, "ledger_entries");

    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    if (line->billed != 0 &&
        add_ledger_entry(writer, arr, "billed_charge", "837", line->billed, line->billed) != X12_OK) {
        return X12_ERR_IO;
    }
    if (line->payer_paid != 0 &&
        add_ledger_entry(writer, arr, "payer_payment", "835", line->payer_paid, -line->payer_paid) != X12_OK) {
        return X12_ERR_IO;
    }
    if (line->contractual_adjustments != 0 &&
        add_ledger_entry(
            writer,
            arr,
            "contractual_adjustment",
            "835 CAS CO",
            line->contractual_adjustments,
            -line->contractual_adjustments
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (line->patient_responsibility != 0 &&
        add_ledger_entry(
            writer,
            arr,
            "patient_responsibility",
            "835 CAS PR",
            line->patient_responsibility,
            0
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    return X12_OK;
}

static int add_claim_identifier(
    json_writer_t *writer,
    yyjson_mut_val *claim_obj,
    const balance_claim_t *claim,
    int include_phi
)
{
    if (include_phi) {
        if (json_writer_add_string(writer, claim_obj, "claim_id", claim->claim_id) != X12_OK) {
            return X12_ERR_IO;
        }
        if (claim->claim_id_token[0] != '\0' &&
            json_writer_add_string(writer, claim_obj, "claim_id_token", claim->claim_id_token) != X12_OK) {
            return X12_ERR_IO;
        }
        return X12_OK;
    }

    return json_writer_add_string(writer, claim_obj, "claim_id", claim->key);
}

static int add_payer_control(
    json_writer_t *writer,
    yyjson_mut_val *claim_obj,
    const balance_claim_t *claim,
    int include_phi
)
{
    const char *tokenised_value;

    if (claim->payer_claim_control_number[0] == '\0' &&
        claim->payer_claim_control_number_token[0] == '\0') {
        return X12_OK;
    }

    if (include_phi) {
        if (json_writer_add_string(
                writer,
                claim_obj,
                "payer_claim_control_number",
                claim->payer_claim_control_number
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        if (claim->payer_claim_control_number_token[0] != '\0' &&
            json_writer_add_string(
                writer,
                claim_obj,
                "payer_claim_control_number_token",
                claim->payer_claim_control_number_token
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        return X12_OK;
    }

    tokenised_value = claim->payer_claim_control_number_token[0] != '\0' ?
        claim->payer_claim_control_number_token :
        claim->payer_claim_control_number;
    return json_writer_add_string(writer, claim_obj, "payer_claim_control_number", tokenised_value);
}

static int add_service_line(
    json_writer_t *writer,
    yyjson_mut_val *arr,
    const balance_service_line_t *line
)
{
    yyjson_mut_val *line_obj = json_writer_array_add_object(writer, arr);
    long long current_balance = line_current_balance(line);

    if (line_obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    if (json_writer_add_string(writer, line_obj, "service_line_number", line->service_line_number) != X12_OK ||
        json_writer_add_string(writer, line_obj, "procedure_code", line->procedure_code) != X12_OK ||
        json_writer_add_string(writer, line_obj, "description", line->description) != X12_OK ||
        json_writer_add_string(writer, line_obj, "service_date", line->service_date) != X12_OK ||
        json_writer_add_string(writer, line_obj, "match_method", line->match_method) != X12_OK ||
        add_money_field(writer, line_obj, "billed", line->billed) != X12_OK ||
        add_money_field(writer, line_obj, "payer_paid", line->payer_paid) != X12_OK ||
        add_money_field(writer, line_obj, "contractual_adjustments", line->contractual_adjustments) != X12_OK ||
        add_money_field(writer, line_obj, "patient_responsibility", line->patient_responsibility) != X12_OK ||
        add_money_field(writer, line_obj, "current_balance", current_balance) != X12_OK ||
        add_line_ledger_entries(writer, line_obj, line) != X12_OK) {
        return X12_ERR_IO;
    }
    return X12_OK;
}

static int build_projection(json_writer_t *writer, const balance_state_t *state)
{
    yyjson_mut_val *root = json_writer_root(writer);
    yyjson_mut_val *claims;
    yyjson_mut_val *totals;
    long long total_billed = 0;
    long long total_paid = 0;
    long long total_contractual = 0;
    long long total_patient_resp = 0;
    size_t i;

    if (json_writer_add_string(writer, root, "event_type", "ClaimBalanceProjected") != X12_OK) {
        return X12_ERR_IO;
    }
    claims = json_writer_add_array(writer, root, "claims");
    if (claims == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < state->claim_count; i++) {
        const balance_claim_t *claim = &state->claims[i];
        yyjson_mut_val *claim_obj;
        yyjson_mut_val *service_lines;
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

        claim_obj = json_writer_array_add_object(writer, claims);
        if (claim_obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        if (add_claim_identifier(writer, claim_obj, claim, state->include_phi) != X12_OK ||
            add_payer_control(writer, claim_obj, claim, state->include_phi) != X12_OK ||
            json_writer_add_string(writer, claim_obj, "claim_type", claim->claim_type) != X12_OK ||
            json_writer_add_string(writer, claim_obj, "claim_status_code", claim->claim_status_code) != X12_OK ||
            add_money_field(writer, claim_obj, "total_billed", billed) != X12_OK ||
            add_money_field(writer, claim_obj, "payer_paid", paid) != X12_OK ||
            add_money_field(writer, claim_obj, "contractual_adjustments", contractual) != X12_OK ||
            add_money_field(writer, claim_obj, "patient_responsibility", patient_resp) != X12_OK ||
            add_money_field(writer, claim_obj, "current_balance", current_balance) != X12_OK) {
            return X12_ERR_IO;
        }

        service_lines = json_writer_add_array(writer, claim_obj, "service_lines");
        if (service_lines == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        for (j = 0u; j < claim->line_count; j++) {
            if (add_service_line(writer, service_lines, &claim->lines[j]) != X12_OK) {
                return X12_ERR_IO;
            }
        }
    }

    totals = json_writer_add_object(writer, root, "totals");
    if (totals == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    if (add_money_field(writer, totals, "total_billed", total_billed) != X12_OK ||
        add_money_field(writer, totals, "payer_paid", total_paid) != X12_OK ||
        add_money_field(writer, totals, "contractual_adjustments", total_contractual) != X12_OK ||
        add_money_field(writer, totals, "patient_responsibility", total_patient_resp) != X12_OK ||
        add_money_field(
            writer,
            totals,
            "current_balance",
            total_billed - total_paid - total_contractual
        ) != X12_OK) {
        return X12_ERR_IO;
    }

    return X12_OK;
}

static int write_projection(FILE *fp, const balance_state_t *state)
{
    json_writer_t writer;
    int rc;

    rc = json_writer_init_object(&writer);
    if (rc != X12_OK) {
        return rc;
    }

    rc = build_projection(&writer, state);
    if (rc == X12_OK) {
        rc = json_writer_write_fp(&writer, fp, 1);
    }

    json_writer_free(&writer);
    return rc;
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
