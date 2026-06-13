#include "balance_projector.h"

#include "json_read.h"
#include "json_write.h"
#include "money.h"
#include "store.h"
#include "str_util.h"
#include "try.h"
#include "tokenise.h"

#include <stdio.h>
#include <string.h>

#define BALANCE_ID_MAX 128u
#define BALANCE_DESC_MAX 256u
/*
 * Kept in step with the claim stitcher's capacities (STITCH_MAX_AGGREGATES and
 * STITCH_MAX_LINES_PER_CLAIM in src/stitch/claims/private.h). The projector can
 * never receive more claims or lines than the stitcher stored, so with these
 * values the projection-side caps only ever fire on a genuine stitcher overflow
 * (already warned upstream), never silently dropping data the stitcher kept. If
 * the stitcher caps grow, grow these to match.
 */
#define BALANCE_MAX_CLAIMS 128u
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

static int snapshot_money_field(json_value_t obj, const char *key, long long *out)
{
    char text[32];

    if (!json_is_object(obj)) {
        return X12_OK;
    }

    /* A money field is stored as a formatted string; an absent or non-string
     * member leaves text empty, which scribe_money_parse maps to 0 + X12_OK. */
    (void)JSON_GET_FIELD(obj, key, text);
    return scribe_money_parse(text, out);
}

static balance_claim_t *add_claim(balance_state_t *state, const char *aggregate_id)
{
    balance_claim_t *claim;
    const char *key;

    if (state == NULL) {
        return NULL;
    }
    if (state->claim_count >= BALANCE_MAX_CLAIMS) {
        fprintf(
            stderr,
            "balance projection: claim cap of %u reached; "
            "remaining claims not projected\n",
            BALANCE_MAX_CLAIMS
        );
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

    if (claim == NULL) {
        return NULL;
    }
    if (claim->line_count >= BALANCE_MAX_LINES_PER_CLAIM) {
        fprintf(
            stderr,
            "balance projection: service-line cap of %u reached for claim "
            "'%s'; remaining lines not projected\n",
            BALANCE_MAX_LINES_PER_CLAIM,
            claim->key
        );
        return NULL;
    }

    line = &claim->lines[claim->line_count++];
    memset(line, 0, sizeof(*line));
    scribe_copy_cstr(line->match_method, sizeof(line->match_method), "unmatched");
    return line;
}

static int apply_snapshot_line(balance_claim_t *claim, json_value_t line_obj)
{
    balance_service_line_t *line;
    json_value_t balance;

    if (claim == NULL || !json_is_object(line_obj)) {
        return X12_OK;
    }

    line = add_line(claim);
    if (line == NULL) {
        /* Line cap reached (add_line has warned); skip the remaining lines. */
        return X12_OK;
    }

    (void)JSON_GET_FIELD(line_obj, "service_line_number", line->service_line_number);
    (void)JSON_GET_FIELD(line_obj, "remittance_service_line_number", line->remit_service_line_number);
    (void)JSON_GET_FIELD(line_obj, "procedure_code", line->procedure_code);
    (void)JSON_GET_FIELD(line_obj, "description", line->description);
    (void)JSON_GET_FIELD(line_obj, "service_date", line->service_date);
    (void)JSON_GET_FIELD(line_obj, "match_method", line->match_method);
    if (line->match_method[0] == '\0') {
        scribe_copy_cstr(line->match_method, sizeof(line->match_method), "unmatched");
    }

    /*
     * Money comes straight from the aggregate-computed balance block. The
     * claim aggregate owns the CO/PR bucketing and the billed/paid derivation;
     * the projection only reshapes those numbers.
     */
    balance = json_object_get(line_obj, "balance");
    TRY(snapshot_money_field(balance, "billed", &line->billed));
    TRY(snapshot_money_field(balance, "payer_paid", &line->payer_paid));
    TRY(snapshot_money_field(balance, "contractual_adjustments", &line->contractual_adjustments));
    TRY(snapshot_money_field(balance, "patient_responsibility", &line->patient_responsibility));

    return X12_OK;
}

static int apply_claim_snapshot(
    balance_state_t *state,
    const char *aggregate_id,
    const char *state_json
)
{
    json_reader_t *reader;
    json_value_t root;
    json_value_t keys;
    json_value_t snapshot_state;
    json_value_t claim_balance;
    json_value_t service_lines;
    balance_claim_t *claim;
    size_t count;
    size_t idx;
    int rc = X12_OK;

    if (state == NULL || aggregate_id == NULL || state_json == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = json_reader_open(&reader, state_json, strlen(state_json), &root);
    if (rc != X12_OK) {
        return rc;
    }
    if (!json_is_object(root)) {
        json_reader_close(reader);
        return X12_ERR_INVALID_ARGUMENT;
    }

    claim = add_claim(state, aggregate_id);
    if (claim == NULL) {
        /*
         * The claim cap has been reached (add_claim has already warned). Skip
         * this aggregate and keep iterating the remaining ones rather than
         * aborting the whole projection.
         */
        json_reader_close(reader);
        return X12_OK;
    }

    keys = json_object_get(root, "keys");
    if (json_is_object(keys)) {
        (void)JSON_GET_FIELD(keys, "claim_id", claim->claim_id);
        (void)JSON_GET_FIELD(keys, "claim_id_token", claim->claim_id_token);
        (void)JSON_GET_FIELD(keys, "payer_claim_control_number", claim->payer_claim_control_number);
        (void)JSON_GET_FIELD(keys, "payer_claim_control_number_token", claim->payer_claim_control_number_token);
    }
    if (claim->claim_id[0] == '\0') {
        scribe_copy_cstr(claim->claim_id, sizeof(claim->claim_id), claim->key);
    }

    snapshot_state = json_object_get(root, "state");
    if (json_is_object(snapshot_state)) {
        (void)JSON_GET_FIELD(snapshot_state, "claim_type", claim->claim_type);
        (void)JSON_GET_FIELD(snapshot_state, "claim_status_code", claim->claim_status_code);
        /*
         * Claim-level totals come from the aggregate-computed balance block,
         * which already applies the no-service-line envelope fallback.
         */
        claim_balance = json_object_get(snapshot_state, "balance");
        if (rc == X12_OK) {
            rc = snapshot_money_field(claim_balance, "total_billed", &claim->claim_total_billed);
        }
        if (rc == X12_OK) {
            rc = snapshot_money_field(claim_balance, "payer_paid", &claim->claim_paid);
        }
        if (rc == X12_OK) {
            rc = snapshot_money_field(
                claim_balance, "patient_responsibility", &claim->claim_patient_responsibility);
        }
    }
    if (rc == X12_OK) {
        service_lines = json_object_get(root, "service_lines");
        count = json_array_count(service_lines);
        for (idx = 0u; idx < count; idx++) {
            rc = apply_snapshot_line(claim, json_array_get(service_lines, idx));
            if (rc != X12_OK) {
                break;
            }
        }
    }

    json_reader_close(reader);
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

    /*
     * Only fall back to the claim-envelope totals when there are no service
     * lines to sum. A claim whose lines legitimately net to zero must keep
     * its zero rather than be overwritten by the envelope figure.
     */
    if (claim->line_count == 0u) {
        *billed = claim->claim_total_billed;
        *paid = claim->claim_paid;
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

    scribe_money_format(value, formatted, sizeof(formatted));
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
