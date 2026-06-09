#ifndef SCRIBE_STITCH_CLAIMS_PRIVATE_H
#define SCRIBE_STITCH_CLAIMS_PRIVATE_H

#include "journal.h"
#include "phi_vault.h"
#include "store.h"
#include "tokenise.h"

#include <stddef.h>
#include <stdio.h>

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

void claim_stitch_split_patient_name(
    const char *raw_name,
    char *last_name_or_org,
    size_t last_name_or_org_len,
    char *first_name,
    size_t first_name_len
);

int claim_stitch_resolve_identifier_output_pair(
    const stitch_state_t *state,
    const char *namespace_name,
    const char *raw_value,
    const char *token_value,
    char *raw_out,
    size_t raw_out_len,
    char *token_out,
    size_t token_out_len
);

int claim_stitch_flush_update_batches(stitch_state_t *state);

#endif
