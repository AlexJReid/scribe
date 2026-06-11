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
#define STITCH_MAX_LINES_PER_CLAIM 64u
#define STITCH_MAX_REFERENCES_PER_CLAIM 32u
#define STITCH_MAX_REFERENCES_PER_LINE 16u
#define STITCH_MAX_CLAIM_DATES 16u
#define STITCH_MAX_DIAGNOSES 32u
#define STITCH_MAX_HEALTHCARE_CODES 64u
#define STITCH_MAX_HEALTHCARE_CODE_COMPONENTS 8u
#define STITCH_MAX_PROVIDER_TAXONOMIES 32u
#define STITCH_MAX_ADJUSTMENTS_PER_LINE 8u
#define STITCH_MAX_ADJUSTMENT_VALUES 8u
#define STITCH_STATE_JSON_MAX 131072u

typedef struct {
    char reference_scope[32];
    char service_line_number[32];
    char reference_qualifier[32];
    char reference_identification[STITCH_ID_MAX];
    char reference_identification_token[TOKENISE_MAX_TOKEN_LEN];
} stitched_claim_reference_t;

typedef struct {
    char date_qualifier[32];
    char date_format[16];
    char date_value[64];
} stitched_claim_date_t;

typedef struct {
    char healthcare_code_kind[32];
    char healthcare_code_qualifier[32];
    char healthcare_code[64];
    char healthcare_code_date_format[16];
    char healthcare_code_date_value[64];
    char healthcare_code_amount[32];
    char healthcare_code_components[STITCH_MAX_HEALTHCARE_CODE_COMPONENTS][64];
    size_t healthcare_code_component_count;
} stitched_healthcare_code_t;

typedef struct {
    char reference_scope[32];
    char service_line_number[32];
    char provider_context[32];
    char provider_role_code[32];
    char reference_identification_qualifier[32];
    char provider_taxonomy_code[64];
} stitched_provider_taxonomy_t;

typedef struct {
    char admission_type_code[32];
    char admission_source_code[32];
    char patient_status_code[32];
    char admission_date[64];
    char discharge_date[64];
    char diagnosis_related_group_code[64];
} stitched_institutional_claim_t;

typedef struct {
    char total_charge_amount[32];
    char facility_type_code[32];
    char facility_code_qualifier[32];
    char claim_frequency_type_code[32];
    char provider_signature_indicator[32];
    char assignment_or_plan_participation_code[32];
    char benefits_assignment_certification_indicator[32];
    char release_of_information_code[32];
    char patient_signature_source_code[32];
} stitched_claim_envelope_t;

typedef struct {
    char payer_responsibility_sequence_number_code[32];
    char individual_relationship_code[32];
    char insured_group_or_policy_number[STITCH_ID_MAX];
    char insured_group_or_policy_number_token[TOKENISE_MAX_TOKEN_LEN];
    char claim_filing_indicator_code[32];
    char date_format[16];
    char date_of_birth[32];
    char date_of_birth_token[TOKENISE_MAX_TOKEN_LEN];
    char gender_code[32];
} stitched_party_context_t;

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
    stitched_claim_reference_t references[STITCH_MAX_REFERENCES_PER_LINE];
    size_t reference_count;
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
    char patient_id[STITCH_ID_MAX];
    char patient_id_token[TOKENISE_MAX_TOKEN_LEN];
    char patient_name[STITCH_VALUE_MAX];
    char patient_name_token[TOKENISE_MAX_TOKEN_LEN];
    char claim_type[64];
    char claim_status_code[32];
    stitched_claim_envelope_t claim_envelope;
    stitched_party_context_t subscriber;
    stitched_party_context_t patient;
    stitched_claim_date_t claim_dates[STITCH_MAX_CLAIM_DATES];
    size_t claim_date_count;
    stitched_claim_reference_t claim_references[STITCH_MAX_REFERENCES_PER_CLAIM];
    size_t claim_reference_count;
    char principal_diagnosis_code[64];
    char other_diagnosis_codes[STITCH_MAX_DIAGNOSES][64];
    size_t other_diagnosis_count;
    stitched_healthcare_code_t healthcare_codes[STITCH_MAX_HEALTHCARE_CODES];
    size_t healthcare_code_count;
    stitched_provider_taxonomy_t provider_taxonomies[STITCH_MAX_PROVIDER_TAXONOMIES];
    size_t provider_taxonomy_count;
    stitched_institutional_claim_t institutional_claim;
    size_t version;
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
    claim_aggregate_t aggregates[STITCH_MAX_AGGREGATES];
    size_t aggregate_count;
    stitch_update_batch_t update_batches[STITCH_MAX_UPDATE_BATCHES];
    size_t update_batch_count;
    char current_drop_key[STITCH_FINGERPRINT_MAX];
    char current_source_drop_id[STITCH_VALUE_MAX];
    char current_source_run_id[STITCH_ID_MAX];
    size_t source_drop_count;
    char run_id[STITCH_ID_MAX];
    size_t dirty_route_count;
    int include_phi;
    FILE *out;
    FILE *notify_out;
    scribe_store_t *read_store;
    phi_vault_t *phi_vault;
    int incremental;
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
int claim_stitch_hydrate_snapshot(
    stitch_state_t *state,
    const char *aggregate_id,
    const char *state_json
);

#endif
