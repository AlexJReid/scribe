#include "balance_projector.h"

#include "event_writer.h"
#include "journal.h"
#include "tokenise.h"

#include <stdio.h>
#include <string.h>

#define json_get_string journal_event_get_string
#define json_get_array_string_at journal_event_get_array_string_at

#define BALANCE_LINE_MAX 8192u
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

static balance_claim_t *find_claim(balance_state_t *state, const char *key)
{
    size_t i;

    if (state == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }

    for (i = 0u; i < state->claim_count; i++) {
        if (strcmp(state->claims[i].key, key) == 0) {
            return &state->claims[i];
        }
    }

    return NULL;
}

static balance_claim_t *find_or_add_claim(
    balance_state_t *state,
    const char *claim_id,
    const char *claim_id_token
)
{
    const char *key = claim_id_token != NULL && claim_id_token[0] != '\0' ? claim_id_token : claim_id;
    balance_claim_t *claim;

    claim = find_claim(state, key);
    if (claim != NULL) {
        if (claim->claim_id[0] == '\0' && claim_id != NULL) {
            copy_cstr(claim->claim_id, sizeof(claim->claim_id), claim_id);
        }
        if (claim->claim_id_token[0] == '\0' && claim_id_token != NULL) {
            copy_cstr(claim->claim_id_token, sizeof(claim->claim_id_token), claim_id_token);
        }
        return claim;
    }

    if (state->claim_count >= BALANCE_MAX_CLAIMS) {
        return NULL;
    }

    claim = &state->claims[state->claim_count++];
    memset(claim, 0, sizeof(*claim));
    copy_cstr(claim->key, sizeof(claim->key), key);
    copy_cstr(claim->claim_id, sizeof(claim->claim_id), claim_id);
    copy_cstr(claim->claim_id_token, sizeof(claim->claim_id_token), claim_id_token);
    return claim;
}

static balance_service_line_t *find_line_by_number(
    balance_claim_t *claim,
    const char *service_line_number
)
{
    size_t i;

    if (claim == NULL || service_line_number == NULL || service_line_number[0] == '\0') {
        return NULL;
    }

    for (i = 0u; i < claim->line_count; i++) {
        if (strcmp(claim->lines[i].service_line_number, service_line_number) == 0 ||
            strcmp(claim->lines[i].remit_service_line_number, service_line_number) == 0) {
            return &claim->lines[i];
        }
    }

    return NULL;
}

static balance_service_line_t *find_line_by_procedure_charge(
    balance_claim_t *claim,
    const char *procedure_code,
    long long billed
)
{
    size_t i;

    if (claim == NULL || procedure_code == NULL || procedure_code[0] == '\0') {
        return NULL;
    }

    for (i = 0u; i < claim->line_count; i++) {
        if (strcmp(claim->lines[i].procedure_code, procedure_code) == 0 &&
            (billed == 0 || claim->lines[i].billed == billed)) {
            return &claim->lines[i];
        }
    }

    return NULL;
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

static balance_service_line_t *find_or_add_line_by_number(
    balance_claim_t *claim,
    const char *service_line_number
)
{
    balance_service_line_t *line = find_line_by_number(claim, service_line_number);

    if (line != NULL) {
        return line;
    }

    line = add_line(claim);
    if (line != NULL) {
        copy_cstr(line->service_line_number, sizeof(line->service_line_number), service_line_number);
    }
    return line;
}

static void event_claim_key(const journal_event_t *journal_line, char *claim_id, size_t claim_id_len, char *claim_token, size_t token_len)
{
    claim_id[0] = '\0';
    claim_token[0] = '\0';
    (void)json_get_string(journal_line, "claim_id", claim_id, claim_id_len);
    (void)json_get_string(journal_line, "claim_id_token", claim_token, token_len);
}

static int apply_claim_observed(balance_state_t *state, const journal_event_t *journal_line)
{
    char claim_id[BALANCE_ID_MAX];
    char claim_token[TOKENISE_MAX_TOKEN_LEN];
    char amount_text[64];
    balance_claim_t *claim;

    event_claim_key(journal_line, claim_id, sizeof(claim_id), claim_token, sizeof(claim_token));
    if (claim_id[0] == '\0') {
        return X12_OK;
    }
    claim = find_claim(state, claim_token[0] != '\0' ? claim_token : claim_id);
    if (claim == NULL) {
        claim = find_or_add_claim(state, claim_id, claim_token);
    }
    if (claim == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    if (json_get_string(journal_line, "total_charge_amount", amount_text, sizeof(amount_text)) &&
        parse_money(amount_text, &claim->claim_total_billed) != X12_OK) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return X12_OK;
}

static int apply_claim_service_line(balance_state_t *state, const journal_event_t *journal_line)
{
    char claim_id[BALANCE_ID_MAX];
    char claim_token[TOKENISE_MAX_TOKEN_LEN];
    char line_no[32];
    char procedure_code[32];
    char amount_text[64];
    long long amount = 0;
    balance_claim_t *claim;
    balance_service_line_t *line;

    event_claim_key(journal_line, claim_id, sizeof(claim_id), claim_token, sizeof(claim_token));
    claim = find_claim(state, claim_token[0] != '\0' ? claim_token : claim_id);
    if (claim == NULL) {
        claim = find_or_add_claim(state, claim_id, claim_token);
    }
    if (claim == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    (void)json_get_string(journal_line, "service_line_number", line_no, sizeof(line_no));
    line = find_or_add_line_by_number(claim, line_no);
    if (line == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    if (json_get_string(journal_line, "procedure_code", procedure_code, sizeof(procedure_code))) {
        copy_cstr(line->procedure_code, sizeof(line->procedure_code), procedure_code);
    }
    if (line->billed == 0 &&
        json_get_string(journal_line, "charge_amount", amount_text, sizeof(amount_text))) {
        if (parse_money(amount_text, &amount) != X12_OK) {
            return X12_ERR_INVALID_ARGUMENT;
        }
        line->billed = amount;
    }

    return X12_OK;
}

static int apply_claim_line_date(balance_state_t *state, const journal_event_t *journal_line)
{
    char claim_id[BALANCE_ID_MAX];
    char claim_token[TOKENISE_MAX_TOKEN_LEN];
    char line_no[32];
    char service_date[32];
    balance_claim_t *claim;
    balance_service_line_t *line;

    event_claim_key(journal_line, claim_id, sizeof(claim_id), claim_token, sizeof(claim_token));
    claim = find_claim(state, claim_token[0] != '\0' ? claim_token : claim_id);
    if (claim == NULL) {
        return X12_OK;
    }

    if (!json_get_string(journal_line, "service_line_number", line_no, sizeof(line_no)) ||
        !json_get_string(journal_line, "date_value", service_date, sizeof(service_date))) {
        return X12_OK;
    }

    line = find_line_by_number(claim, line_no);
    if (line != NULL && line->service_date[0] == '\0') {
        copy_cstr(line->service_date, sizeof(line->service_date), service_date);
    }

    return X12_OK;
}

static int apply_remittance_claim(balance_state_t *state, const journal_event_t *journal_line)
{
    char claim_id[BALANCE_ID_MAX];
    char claim_token[TOKENISE_MAX_TOKEN_LEN];
    char amount_text[64];
    char payer_control[BALANCE_ID_MAX];
    char payer_control_token[TOKENISE_MAX_TOKEN_LEN];
    balance_claim_t *claim;

    event_claim_key(journal_line, claim_id, sizeof(claim_id), claim_token, sizeof(claim_token));
    claim = find_claim(state, claim_token[0] != '\0' ? claim_token : claim_id);
    if (claim == NULL) {
        claim = find_or_add_claim(state, claim_id, claim_token);
    }
    if (claim == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    if (json_get_string(journal_line, "claim_status_code", amount_text, sizeof(amount_text))) {
        copy_cstr(claim->claim_status_code, sizeof(claim->claim_status_code), amount_text);
    }
    if (json_get_string(journal_line, "total_charge_amount", amount_text, sizeof(amount_text)) &&
        parse_money(amount_text, &claim->claim_total_billed) != X12_OK) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (json_get_string(journal_line, "paid_amount", amount_text, sizeof(amount_text)) &&
        parse_money(amount_text, &claim->claim_paid) != X12_OK) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (json_get_string(journal_line, "patient_responsibility_amount", amount_text, sizeof(amount_text)) &&
        parse_money(amount_text, &claim->claim_patient_responsibility) != X12_OK) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (json_get_string(journal_line, "payer_claim_control_number", payer_control, sizeof(payer_control))) {
        copy_cstr(claim->payer_claim_control_number, sizeof(claim->payer_claim_control_number), payer_control);
    }
    if (json_get_string(journal_line, "payer_claim_control_number_token", payer_control_token, sizeof(payer_control_token))) {
        copy_cstr(
            claim->payer_claim_control_number_token,
            sizeof(claim->payer_claim_control_number_token),
            payer_control_token
        );
    }

    return X12_OK;
}

static int apply_remittance_service_line(balance_state_t *state, const journal_event_t *journal_line)
{
    char claim_id[BALANCE_ID_MAX];
    char claim_token[TOKENISE_MAX_TOKEN_LEN];
    char remit_line_no[32];
    char procedure_code[32];
    char amount_text[64];
    long long charge_amount = 0;
    long long paid_amount = 0;
    balance_claim_t *claim;
    balance_service_line_t *line;

    event_claim_key(journal_line, claim_id, sizeof(claim_id), claim_token, sizeof(claim_token));
    claim = find_claim(state, claim_token[0] != '\0' ? claim_token : claim_id);
    if (claim == NULL) {
        claim = find_or_add_claim(state, claim_id, claim_token);
    }
    if (claim == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    (void)json_get_string(journal_line, "service_line_number", remit_line_no, sizeof(remit_line_no));
    (void)json_get_string(journal_line, "procedure_code", procedure_code, sizeof(procedure_code));
    if (json_get_string(journal_line, "line_charge_amount", amount_text, sizeof(amount_text)) &&
        parse_money(amount_text, &charge_amount) != X12_OK) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (json_get_string(journal_line, "line_paid_amount", amount_text, sizeof(amount_text)) &&
        parse_money(amount_text, &paid_amount) != X12_OK) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    line = find_line_by_procedure_charge(claim, procedure_code, charge_amount);
    if (line != NULL) {
        copy_cstr(line->match_method, sizeof(line->match_method), "procedure_charge");
    } else {
        line = find_line_by_number(claim, remit_line_no);
        if (line != NULL) {
            copy_cstr(line->match_method, sizeof(line->match_method), "line_order");
        }
    }
    if (line == NULL) {
        line = find_or_add_line_by_number(claim, remit_line_no);
        if (line == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        copy_cstr(line->match_method, sizeof(line->match_method), "created_from_remittance");
    }

    copy_cstr(line->remit_service_line_number, sizeof(line->remit_service_line_number), remit_line_no);
    if (line->procedure_code[0] == '\0') {
        copy_cstr(line->procedure_code, sizeof(line->procedure_code), procedure_code);
    }
    if (line->billed == 0) {
        line->billed = charge_amount;
    }
    line->payer_paid += paid_amount;
    return X12_OK;
}

static int apply_remittance_line_date(balance_state_t *state, const journal_event_t *journal_line)
{
    char claim_id[BALANCE_ID_MAX];
    char claim_token[TOKENISE_MAX_TOKEN_LEN];
    char remit_line_no[32];
    char service_date[32];
    balance_claim_t *claim;
    balance_service_line_t *line;

    event_claim_key(journal_line, claim_id, sizeof(claim_id), claim_token, sizeof(claim_token));
    claim = find_claim(state, claim_token[0] != '\0' ? claim_token : claim_id);
    if (claim == NULL) {
        return X12_OK;
    }
    if (!json_get_string(journal_line, "service_line_number", remit_line_no, sizeof(remit_line_no)) ||
        !json_get_string(journal_line, "date_value", service_date, sizeof(service_date))) {
        return X12_OK;
    }

    line = find_line_by_number(claim, remit_line_no);
    if (line != NULL) {
        if (line->service_date[0] == '\0') {
            copy_cstr(line->service_date, sizeof(line->service_date), service_date);
        }
        if (strcmp(line->service_date, service_date) == 0 &&
            strcmp(line->match_method, "procedure_charge") == 0) {
            copy_cstr(line->match_method, sizeof(line->match_method), "procedure_charge_date");
        }
    }

    return X12_OK;
}

static int apply_adjustment(balance_state_t *state, const journal_event_t *journal_line)
{
    char claim_id[BALANCE_ID_MAX];
    char claim_token[TOKENISE_MAX_TOKEN_LEN];
    char line_no[32];
    char group_code[32];
    char amount_text[64];
    long long amount = 0;
    balance_claim_t *claim;
    balance_service_line_t *line;

    event_claim_key(journal_line, claim_id, sizeof(claim_id), claim_token, sizeof(claim_token));
    claim = find_claim(state, claim_token[0] != '\0' ? claim_token : claim_id);
    if (claim == NULL) {
        return X12_OK;
    }

    if (!json_get_string(journal_line, "service_line_number", line_no, sizeof(line_no)) ||
        line_no[0] == '\0') {
        return X12_OK;
    }
    line = find_line_by_number(claim, line_no);
    if (line == NULL) {
        return X12_OK;
    }

    (void)json_get_string(journal_line, "adjustment_group_code", group_code, sizeof(group_code));
    if (!json_get_array_string_at(journal_line, "amounts", 0u, amount_text, sizeof(amount_text))) {
        return X12_OK;
    }
    if (parse_money(amount_text, &amount) != X12_OK) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (strcmp(group_code, "CO") == 0) {
        line->contractual_adjustments += amount;
    } else if (strcmp(group_code, "PR") == 0) {
        line->patient_responsibility += amount;
    }

    return X12_OK;
}

static int apply_event(balance_state_t *state, const journal_event_t *journal_line)
{
    char event_type[96];

    if (!json_get_string(journal_line, "event_type", event_type, sizeof(event_type))) {
        return X12_OK;
    }

    if (strcmp(event_type, "ClaimObserved") == 0) {
        return apply_claim_observed(state, journal_line);
    }
    if (strcmp(event_type, "ClaimServiceLineRecorded") == 0) {
        return apply_claim_service_line(state, journal_line);
    }
    if (strcmp(event_type, "ClaimLineDateRecorded") == 0) {
        return apply_claim_line_date(state, journal_line);
    }
    if (strcmp(event_type, "RemittanceClaimPaymentObserved") == 0) {
        return apply_remittance_claim(state, journal_line);
    }
    if (strcmp(event_type, "RemittanceServiceLinePaymentObserved") == 0) {
        return apply_remittance_service_line(state, journal_line);
    }
    if (strcmp(event_type, "RemittanceDateRecorded") == 0) {
        return apply_remittance_line_date(state, journal_line);
    }
    if (strcmp(event_type, "RemittanceAdjustmentObserved") == 0) {
        return apply_adjustment(state, journal_line);
    }

    return X12_OK;
}

static int read_journal_pass(balance_state_t *state, const char *path)
{
    journal_reader_t reader;
    journal_event_t record;
    int rc = X12_OK;

    journal_reader_init(&reader);
    rc = journal_reader_open(&reader, path);
    if (rc != X12_OK) {
        return rc;
    }

    while (rc == X12_OK) {
        rc = journal_reader_next(&reader, &record);
        if (rc != X12_OK || record.record_len == 0u) {
            break;
        }
        rc = apply_event(state, &record);
    }

    if (journal_reader_close(&reader) != X12_OK && rc == X12_OK) {
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
    if (claim->payer_claim_control_number[0] == '\0') {
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

    return event_writer_write_cstring_field(fp, "payer_claim_control_number", claim->payer_claim_control_number, 1);
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

    if (input == NULL || input->journal_path == NULL || out_path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(&state, 0, sizeof(state));
    state.include_phi = input->include_phi;

    rc = read_journal_pass(&state, input->journal_path);
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
        if (strcmp(argv[i], "--projection") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            i++;
        } else if (strcmp(argv[i], "--journal") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.journal_path = argv[++i];
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

    if (input.journal_path == NULL) {
        return -1;
    }

    return balance_projector_project(&input, out_path);
}
