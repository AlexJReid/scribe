#include "private.h"

#include "json_write.h"
#include "money.h"
#include "str_util.h"
#include "try.h"

#include <string.h>
#include <stdlib.h>

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

    arr = json_writer_add_array(writer, obj, key);
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < value_count; i++) {
        TRY(json_writer_array_add_string(writer, arr, values[i]));
    }

    return X12_OK;
}

static int snapshot_add_string_array64(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    const char values[][64],
    size_t value_count
)
{
    yyjson_mut_val *arr;
    size_t i;

    arr = json_writer_add_array(writer, obj, key);
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < value_count; i++) {
        TRY(json_writer_array_add_string(writer, arr, values[i]));
    }

    return X12_OK;
}

static int snapshot_add_reference(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const stitch_state_t *state,
    const stitched_claim_reference_t *reference
)
{
    int rc;

    rc = json_writer_add_string(writer, obj, "reference_scope", reference->reference_scope);
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "service_line_number", reference->service_line_number);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "reference_qualifier", reference->reference_qualifier);
    }
    if (rc == X12_OK && state != NULL && state->include_phi &&
        reference->reference_identification[0] != '\0') {
        rc = json_writer_add_string(
            writer,
            obj,
            "reference_identification",
            reference->reference_identification
        );
        if (rc == X12_OK && reference->reference_identification_token[0] != '\0') {
            rc = json_writer_add_string(
                writer,
                obj,
                "reference_identification_token",
                reference->reference_identification_token
            );
        }
    } else if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "reference_identification",
            reference->reference_identification_token[0] != '\0' ?
                reference->reference_identification_token :
                reference->reference_identification
        );
    }

    return rc;
}

static int snapshot_add_references(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    const stitch_state_t *state,
    const stitched_claim_reference_t *references,
    size_t reference_count
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *ref_obj;
    size_t i;

    arr = json_writer_add_array(writer, obj, key);
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < reference_count; i++) {
        ref_obj = json_writer_array_add_object(writer, arr);
        if (ref_obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        TRY(snapshot_add_reference(writer, ref_obj, state, &references[i]));
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

/*
 * A service line's balance, in signed cents. This is the canonical balance
 * model: the claim aggregate owns how billed/paid/contractual/patient figures
 * are derived from the matched 837/835 facts, so downstream projections can
 * reshape these numbers without re-bucketing CAS adjustments themselves.
 */
typedef struct {
    long long billed;
    long long payer_paid;
    long long contractual_adjustments;
    long long patient_responsibility;
    long long current_balance;
} snapshot_line_balance_t;

static int snapshot_sum_adjustment_amounts(
    const stitched_line_adjustment_t *adjustment,
    long long *out
)
{
    long long amount;
    size_t i;

    *out = 0;
    for (i = 0u; i < adjustment->value_count; i++) {
        TRY(scribe_money_parse(adjustment->amounts[i], &amount));
        *out += amount;
    }
    return X12_OK;
}

static int snapshot_compute_line_balance(
    const stitched_service_line_t *line,
    snapshot_line_balance_t *balance
)
{
    long long amount;
    size_t i;

    memset(balance, 0, sizeof(*balance));

    /* Billed comes from the submitted charge, falling back to the remittance
     * line charge when only an 835 created the line. */
    TRY(scribe_money_parse(line->submitted_charge_amount, &balance->billed));
    if (balance->billed == 0) {
        TRY(scribe_money_parse(line->remittance_line_charge_amount, &balance->billed));
    }
    TRY(scribe_money_parse(line->remittance_line_paid_amount, &balance->payer_paid));

    for (i = 0u; i < line->adjustment_count; i++) {
        const stitched_line_adjustment_t *adjustment = &line->adjustments[i];
        TRY(snapshot_sum_adjustment_amounts(adjustment, &amount));
        if (strcmp(adjustment->adjustment_group_code, "CO") == 0) {
            balance->contractual_adjustments += amount;
        } else if (strcmp(adjustment->adjustment_group_code, "PR") == 0) {
            balance->patient_responsibility += amount;
        }
    }

    balance->current_balance =
        balance->billed - balance->payer_paid - balance->contractual_adjustments;
    return X12_OK;
}

static int snapshot_add_money_field(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *name,
    long long cents
)
{
    char formatted[32];

    scribe_money_format(cents, formatted, sizeof(formatted));
    return json_writer_add_string(writer, obj, name, formatted);
}

static int snapshot_add_line_balance(
    json_writer_t *writer,
    yyjson_mut_val *line_obj,
    const snapshot_line_balance_t *balance
)
{
    yyjson_mut_val *obj = json_writer_add_object(writer, line_obj, "balance");

    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    TRY(snapshot_add_money_field(writer, obj, "billed", balance->billed));
    TRY(snapshot_add_money_field(writer, obj, "payer_paid", balance->payer_paid));
    TRY(snapshot_add_money_field(
        writer, obj, "contractual_adjustments", balance->contractual_adjustments));
    TRY(snapshot_add_money_field(
        writer, obj, "patient_responsibility", balance->patient_responsibility));
    TRY(snapshot_add_money_field(writer, obj, "current_balance", balance->current_balance));
    return X12_OK;
}

/*
 * Emit the claim-level balance into the state object. When the claim has no
 * service lines to sum, the envelope's total charge stands in for billed (a
 * claim whose lines legitimately net to zero keeps its zero). Paid and
 * patient responsibility have no claim-level fallback today, so they stay at
 * the line-sum value.
 */
static int snapshot_add_claim_balance(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const claim_aggregate_t *aggregate,
    const snapshot_line_balance_t *line_sum
)
{
    snapshot_line_balance_t balance = *line_sum;
    yyjson_mut_val *obj;

    if (aggregate->service_line_count == 0u) {
        TRY(scribe_money_parse(
            aggregate->claim_envelope.total_charge_amount, &balance.billed));
        balance.current_balance =
            balance.billed - balance.payer_paid - balance.contractual_adjustments;
    }

    obj = json_writer_add_object(writer, state_obj, "balance");
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }
    TRY(snapshot_add_money_field(writer, obj, "total_billed", balance.billed));
    TRY(snapshot_add_money_field(writer, obj, "payer_paid", balance.payer_paid));
    TRY(snapshot_add_money_field(
        writer, obj, "contractual_adjustments", balance.contractual_adjustments));
    TRY(snapshot_add_money_field(
        writer, obj, "patient_responsibility", balance.patient_responsibility));
    TRY(snapshot_add_money_field(writer, obj, "current_balance", balance.current_balance));
    return X12_OK;
}

static int snapshot_add_service_lines(
    json_writer_t *writer,
    yyjson_mut_val *root,
    const stitch_state_t *state,
    const claim_aggregate_t *aggregate,
    snapshot_line_balance_t *claim_balance
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *line_obj;
    const stitched_service_line_t *line;
    snapshot_line_balance_t line_balance;
    size_t i;
    int rc;

    memset(claim_balance, 0, sizeof(*claim_balance));

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
            rc = snapshot_add_submitted_line(writer, line_obj, line);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_remittance_line(writer, line_obj, line);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_references(
                writer,
                line_obj,
                "references",
                state,
                line->references,
                line->reference_count
            );
        }
        if (rc == X12_OK) {
            rc = snapshot_add_adjustments(writer, line_obj, line);
        }
        if (rc == X12_OK) {
            rc = snapshot_compute_line_balance(line, &line_balance);
        }
        if (rc == X12_OK) {
            rc = snapshot_add_line_balance(writer, line_obj, &line_balance);
        }
        if (rc != X12_OK) {
            return rc;
        }

        claim_balance->billed += line_balance.billed;
        claim_balance->payer_paid += line_balance.payer_paid;
        claim_balance->contractual_adjustments += line_balance.contractual_adjustments;
        claim_balance->patient_responsibility += line_balance.patient_responsibility;
    }

    claim_balance->current_balance =
        claim_balance->billed - claim_balance->payer_paid -
        claim_balance->contractual_adjustments;
    return X12_OK;
}

static int snapshot_add_claim_envelope(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *obj;
    const stitched_claim_envelope_t *envelope = &aggregate->claim_envelope;
    int rc;

    obj = json_writer_add_object(writer, state_obj, "claim_envelope");
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_string(writer, obj, "total_charge_amount", envelope->total_charge_amount);
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "facility_type_code", envelope->facility_type_code);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "facility_code_qualifier", envelope->facility_code_qualifier);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "claim_frequency_type_code", envelope->claim_frequency_type_code);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "provider_signature_indicator", envelope->provider_signature_indicator);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "assignment_or_plan_participation_code",
            envelope->assignment_or_plan_participation_code
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "benefits_assignment_certification_indicator",
            envelope->benefits_assignment_certification_indicator
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "release_of_information_code", envelope->release_of_information_code);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "patient_signature_source_code", envelope->patient_signature_source_code);
    }

    return rc;
}

static int snapshot_add_party_context(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const char *key,
    const stitch_state_t *state,
    const stitched_party_context_t *party
)
{
    yyjson_mut_val *obj;
    int rc;

    obj = json_writer_add_object(writer, state_obj, key);
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_string(
        writer,
        obj,
        "payer_responsibility_sequence_number_code",
        party->payer_responsibility_sequence_number_code
    );
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "individual_relationship_code",
            party->individual_relationship_code
        );
    }
    if (rc == X12_OK && state != NULL && state->include_phi &&
        party->insured_group_or_policy_number[0] != '\0') {
        rc = json_writer_add_string(
            writer,
            obj,
            "insured_group_or_policy_number",
            party->insured_group_or_policy_number
        );
        if (rc == X12_OK && party->insured_group_or_policy_number_token[0] != '\0') {
            rc = json_writer_add_string(
                writer,
                obj,
                "insured_group_or_policy_number_token",
                party->insured_group_or_policy_number_token
            );
        }
    } else if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "insured_group_or_policy_number",
            party->insured_group_or_policy_number_token[0] != '\0' ?
                party->insured_group_or_policy_number_token :
                party->insured_group_or_policy_number
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "claim_filing_indicator_code", party->claim_filing_indicator_code);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "date_format", party->date_format);
    }
    if (rc == X12_OK && state != NULL && state->include_phi && party->date_of_birth[0] != '\0') {
        rc = json_writer_add_string(writer, obj, "date_of_birth", party->date_of_birth);
        if (rc == X12_OK && party->date_of_birth_token[0] != '\0') {
            rc = json_writer_add_string(writer, obj, "date_of_birth_token", party->date_of_birth_token);
        }
    } else if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "date_of_birth",
            party->date_of_birth_token[0] != '\0' ? party->date_of_birth_token : party->date_of_birth
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "gender_code", party->gender_code);
    }

    return rc;
}

static int snapshot_add_claim_dates(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *obj;
    const stitched_claim_date_t *date;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, state_obj, "claim_dates");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < aggregate->claim_date_count; i++) {
        date = &aggregate->claim_dates[i];
        obj = json_writer_array_add_object(writer, arr);
        if (obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        rc = json_writer_add_string(writer, obj, "date_qualifier", date->date_qualifier);
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, obj, "date_format", date->date_format);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, obj, "date_value", date->date_value);
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_diagnoses(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *obj;
    int rc;

    obj = json_writer_add_object(writer, state_obj, "diagnoses");
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_string(
        writer,
        obj,
        "principal_diagnosis_code",
        aggregate->principal_diagnosis_code
    );
    if (rc == X12_OK) {
        rc = snapshot_add_string_array64(
            writer,
            obj,
            "other_diagnosis_codes",
            aggregate->other_diagnosis_codes,
            aggregate->other_diagnosis_count
        );
    }

    return rc;
}

static int snapshot_add_healthcare_codes(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *obj;
    const stitched_healthcare_code_t *code;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, state_obj, "healthcare_codes");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < aggregate->healthcare_code_count; i++) {
        code = &aggregate->healthcare_codes[i];
        obj = json_writer_array_add_object(writer, arr);
        if (obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        rc = json_writer_add_string(writer, obj, "healthcare_code_kind", code->healthcare_code_kind);
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, obj, "healthcare_code_qualifier", code->healthcare_code_qualifier);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, obj, "healthcare_code", code->healthcare_code);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(
                writer,
                obj,
                "healthcare_code_date_format",
                code->healthcare_code_date_format
            );
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(
                writer,
                obj,
                "healthcare_code_date_value",
                code->healthcare_code_date_value
            );
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(
                writer,
                obj,
                "healthcare_code_amount",
                code->healthcare_code_amount
            );
        }
        if (rc == X12_OK) {
            rc = snapshot_add_string_array64(
                writer,
                obj,
                "healthcare_code_components",
                code->healthcare_code_components,
                code->healthcare_code_component_count
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_provider_taxonomies(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *arr;
    yyjson_mut_val *obj;
    const stitched_provider_taxonomy_t *taxonomy;
    size_t i;
    int rc;

    arr = json_writer_add_array(writer, state_obj, "provider_taxonomies");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < aggregate->provider_taxonomy_count; i++) {
        taxonomy = &aggregate->provider_taxonomies[i];
        obj = json_writer_array_add_object(writer, arr);
        if (obj == NULL) {
            return X12_ERR_NO_MEMORY;
        }

        rc = json_writer_add_string(writer, obj, "reference_scope", taxonomy->reference_scope);
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, obj, "service_line_number", taxonomy->service_line_number);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, obj, "provider_context", taxonomy->provider_context);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, obj, "provider_role_code", taxonomy->provider_role_code);
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(
                writer,
                obj,
                "reference_identification_qualifier",
                taxonomy->reference_identification_qualifier
            );
        }
        if (rc == X12_OK) {
            rc = json_writer_add_string(writer, obj, "provider_taxonomy_code", taxonomy->provider_taxonomy_code);
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int snapshot_add_institutional_claim(
    json_writer_t *writer,
    yyjson_mut_val *state_obj,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *obj;
    const stitched_institutional_claim_t *institutional = &aggregate->institutional_claim;
    int rc;

    obj = json_writer_add_object(writer, state_obj, "institutional_claim");
    if (obj == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    rc = json_writer_add_string(writer, obj, "admission_type_code", institutional->admission_type_code);
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "admission_source_code", institutional->admission_source_code);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "patient_status_code", institutional->patient_status_code);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "admission_date", institutional->admission_date);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(writer, obj, "discharge_date", institutional->discharge_date);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_string(
            writer,
            obj,
            "diagnosis_related_group_code",
            institutional->diagnosis_related_group_code
        );
    }

    return rc;
}

static int snapshot_add_source_event_ids(
    json_writer_t *writer,
    yyjson_mut_val *root,
    const claim_aggregate_t *aggregate
)
{
    yyjson_mut_val *arr;
    size_t i;

    arr = json_writer_add_array(writer, root, "applied_event_ids");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    for (i = 0u; i < claim_aggregate_source_event_count(aggregate); i++) {
        TRY(json_writer_array_add_size(
            writer, arr, claim_aggregate_source_event(aggregate, i)->event_id));
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

    if (batch == NULL || batch->aggregate == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    arr = json_writer_add_array(writer, root, "update_event_ids");
    if (arr == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    end = batch->first_source_event_index + batch->source_event_count;
    for (i = batch->first_source_event_index; i < end; i++) {
        TRY(json_writer_array_add_size(
            writer, arr, claim_aggregate_source_event(batch->aggregate, i)->event_id));
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
    snapshot_line_balance_t claim_balance;
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

    TRY(json_writer_init_object(writer));

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

    TRY(claim_stitch_resolve_identifier_output_pair(
        state,
        "claim_id",
        aggregate->claim_id,
        aggregate->claim_id_token,
        claim_id,
        sizeof(claim_id),
        claim_id_token,
        sizeof(claim_id_token)
    ));
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

    TRY(claim_stitch_resolve_identifier_output_pair(
        state,
        "payer_claim_control_number",
        aggregate->payer_claim_control_number,
        aggregate->payer_claim_control_number_token,
        payer_control,
        sizeof(payer_control),
        payer_control_token,
        sizeof(payer_control_token)
    ));
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

        claim_stitch_split_patient_name(
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

    rc = json_writer_add_bool(writer, snapshot_state, "has_837", aggregate->has_837);
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
            claim_aggregate_source_event_count(aggregate)
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
        rc = snapshot_add_claim_envelope(writer, snapshot_state, aggregate);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_party_context(writer, snapshot_state, "subscriber", state, &aggregate->subscriber);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_party_context(writer, snapshot_state, "patient", state, &aggregate->patient);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_claim_dates(writer, snapshot_state, aggregate);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_references(
            writer,
            snapshot_state,
            "claim_references",
            state,
            aggregate->claim_references,
            aggregate->claim_reference_count
        );
    }
    if (rc == X12_OK) {
        rc = snapshot_add_diagnoses(writer, snapshot_state, aggregate);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_healthcare_codes(writer, snapshot_state, aggregate);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_provider_taxonomies(writer, snapshot_state, aggregate);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_institutional_claim(writer, snapshot_state, aggregate);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_service_lines(writer, root, state, aggregate, &claim_balance);
    }
    if (rc == X12_OK) {
        rc = snapshot_add_claim_balance(writer, snapshot_state, aggregate, &claim_balance);
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
            claim_aggregate_source_event_count(aggregate)
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
        1
    );
    if (rc == X12_OK) {
        rc = json_writer_write_cstring(&writer, out, out_len);
    }
    json_writer_free(&writer);
    return rc;
}

static int persist_claim_aggregate_keys(
    stitch_state_t *state,
    const claim_aggregate_t *aggregate,
    const char *aggregate_id
)
{
    char claim_id_raw[STITCH_ID_MAX];
    char claim_id_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_control_raw[STITCH_ID_MAX];
    char payer_control_token[TOKENISE_MAX_TOKEN_LEN];
    const char *claim_key_value;
    const char *payer_key_value;

    if (state == NULL || state->read_store == NULL || aggregate == NULL ||
        aggregate_id == NULL) {
        return X12_OK;
    }

    TRY(claim_stitch_resolve_identifier_output_pair(
        state,
        "claim_id",
        aggregate->claim_id,
        aggregate->claim_id_token,
        claim_id_raw,
        sizeof(claim_id_raw),
        claim_id_token,
        sizeof(claim_id_token)
    ));
    claim_key_value = claim_id_token[0] != '\0' ? claim_id_token : aggregate->key;
    TRY(scribe_store_put_claim_aggregate_key(
        state->read_store,
        "claim_id",
        claim_key_value,
        aggregate_id
    ));
    if (state->include_phi && claim_id_raw[0] != '\0' &&
        claim_id_token[0] != '\0' && strcmp(claim_id_raw, claim_id_token) != 0) {
        TRY(scribe_store_put_claim_aggregate_key(
            state->read_store,
            "claim_id_raw",
            claim_id_raw,
            aggregate_id
        ));
    }

    TRY(claim_stitch_resolve_identifier_output_pair(
        state,
        "payer_claim_control_number",
        aggregate->payer_claim_control_number,
        aggregate->payer_claim_control_number_token,
        payer_control_raw,
        sizeof(payer_control_raw),
        payer_control_token,
        sizeof(payer_control_token)
    ));
    payer_key_value = payer_control_token[0] != '\0' ?
        payer_control_token :
        (aggregate->payer_claim_control_number_token[0] != '\0' ?
            aggregate->payer_claim_control_number_token :
            aggregate->payer_claim_control_number);
    if (payer_key_value[0] != '\0') {
        TRY(scribe_store_put_claim_aggregate_key(
            state->read_store,
            "payer_claim_control_number",
            payer_key_value,
            aggregate_id
        ));
    }
    if (state->include_phi && payer_control_raw[0] != '\0' &&
        payer_control_token[0] != '\0' &&
        strcmp(payer_control_raw, payer_control_token) != 0) {
        TRY(scribe_store_put_claim_aggregate_key(
            state->read_store,
            "payer_claim_control_number_raw",
            payer_control_raw,
            aggregate_id
        ));
    }

    return X12_OK;
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
    if (rc == X12_OK) {
        rc = persist_claim_aggregate_keys(state, batch->aggregate, aggregate_id);
    }
    free(state_json);
    return rc;
}

static int write_notification(
    stitch_state_t *state,
    const stitch_update_batch_t *batch,
    size_t aggregate_version_count
)
{
    json_writer_t writer = {0};
    yyjson_mut_val *root;
    char notification_id[SCRIBE_STORE_NOTIFICATION_ID_MAX];
    char payload_json[SCRIBE_STORE_PAYLOAD_MAX];
    int written;
    int rc;

    if (state == NULL || batch == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (state->read_store == NULL) {
        return X12_OK;
    }

    written = snprintf(
        notification_id,
        sizeof(notification_id),
        "claim:%s:%s",
        batch->source_drop_id,
        state->run_id
    );
    if (written < 0 || (size_t)written >= sizeof(notification_id)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    TRY(json_writer_init_object(&writer));
    root = json_writer_root(&writer);

    rc = json_writer_add_string(&writer, root, "event_type", "SourceDropAggregatesRecorded");
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
        rc = json_writer_add_string(&writer, root, "source_drop_id", batch->source_drop_id);
    }
    if (rc == X12_OK) {
        rc = json_writer_add_size(
            &writer,
            root,
            "aggregate_version_count",
            aggregate_version_count
        );
    }
    if (rc == X12_OK) {
        rc = json_writer_write_cstring(&writer, payload_json, sizeof(payload_json));
    }
    json_writer_free(&writer);
    if (rc != X12_OK) {
        return rc;
    }

    return scribe_store_put_outbox_notification(
        state->read_store,
        notification_id,
        "SourceDropAggregatesRecorded",
        "claim",
        batch->source_drop_id,
        state->run_id,
        batch->source_run_id,
        aggregate_version_count,
        payload_json
    );
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

    fprintf(
        stderr,
        "scribe stitch claims: emit aggregate=%s version=%zu source_drop=%s update_events=%zu\n",
        aggregate_id,
        batch->aggregate->version,
        batch->source_drop_id,
        batch->source_event_count
    );

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

    TRY(persist_snapshot_to_read_store(state, batch, aggregate_id));
    if (state->incremental && state->read_store != NULL) {
        TRY(scribe_store_clear_dirty_aggregate(
            state->read_store,
            "claim",
            aggregate_id,
            batch->source_drop_id
        ));
    }

    return X12_OK;
}


static void snapshot_get_bool(yyjson_val *obj, const char *key, int *out)
{
    yyjson_val *value;

    if (out == NULL || obj == NULL || key == NULL) {
        return;
    }

    value = yyjson_obj_get(obj, key);
    if (value != NULL && yyjson_is_bool(value)) {
        *out = yyjson_get_bool(value) ? 1 : 0;
    }
}

static void snapshot_get_size(yyjson_val *obj, const char *key, size_t *out)
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

static int hydrate_applied_event_ids(claim_aggregate_t *aggregate, yyjson_val *root)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;

    if (aggregate == NULL || root == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    arr = yyjson_obj_get(root, "applied_event_ids");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    scribe_grow_vec_clear(&aggregate->source_events);
    yyjson_arr_foreach(arr, idx, max, item) {
        stitched_source_event_t *source_event;

        if (!yyjson_is_uint(item)) {
            continue;
        }

        source_event = claim_aggregate_add_source_event(aggregate);
        if (source_event == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        source_event->event_id = (size_t)yyjson_get_uint(item);
    }

    return X12_OK;
}

static int hydrate_reference(stitched_claim_reference_t *reference, yyjson_val *obj)
{
    int rc;

    if (reference == NULL || obj == NULL || !yyjson_is_obj(obj)) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = json_read_string(obj, "reference_scope", reference->reference_scope, sizeof(reference->reference_scope));
    if (rc == X12_OK) {
        rc = json_read_string(
            obj,
            "service_line_number",
            reference->service_line_number,
            sizeof(reference->service_line_number)
        );
    }
    if (rc == X12_OK) {
        rc = json_read_string(
            obj,
            "reference_qualifier",
            reference->reference_qualifier,
            sizeof(reference->reference_qualifier)
        );
    }
    if (rc == X12_OK) {
        rc = json_read_string(
            obj,
            "reference_identification",
            reference->reference_identification,
            sizeof(reference->reference_identification)
        );
    }
    if (rc == X12_OK) {
        rc = json_read_string(
            obj,
            "reference_identification_token",
            reference->reference_identification_token,
            sizeof(reference->reference_identification_token)
        );
    }
    if (rc == X12_OK && reference->reference_identification_token[0] == '\0') {
        scribe_copy_cstr(
            reference->reference_identification_token,
            sizeof(reference->reference_identification_token),
            reference->reference_identification
        );
        reference->reference_identification[0] = '\0';
    }

    return rc;
}

static int hydrate_reference_array(
    stitched_claim_reference_t *references,
    size_t reference_cap,
    size_t *reference_count,
    yyjson_val *parent,
    const char *key
)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;

    if (references == NULL || reference_count == NULL || parent == NULL || key == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    arr = yyjson_obj_get(parent, key);
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    *reference_count = 0u;
    yyjson_arr_foreach(arr, idx, max, item) {
        if (!yyjson_is_obj(item)) {
            continue;
        }
        if (*reference_count >= reference_cap) {
            return X12_ERR_NO_MEMORY;
        }
        TRY(hydrate_reference(&references[*reference_count], item));
        (*reference_count)++;
    }

    return X12_OK;
}

static int hydrate_claim_envelope(claim_aggregate_t *aggregate, yyjson_val *state_obj)
{
    yyjson_val *obj;

    obj = yyjson_obj_get(state_obj, "claim_envelope");
    if (obj == NULL || !yyjson_is_obj(obj)) {
        return X12_OK;
    }

    (void)json_read_string(obj, "total_charge_amount", aggregate->claim_envelope.total_charge_amount, sizeof(aggregate->claim_envelope.total_charge_amount));
    (void)json_read_string(obj, "facility_type_code", aggregate->claim_envelope.facility_type_code, sizeof(aggregate->claim_envelope.facility_type_code));
    (void)json_read_string(obj, "facility_code_qualifier", aggregate->claim_envelope.facility_code_qualifier, sizeof(aggregate->claim_envelope.facility_code_qualifier));
    (void)json_read_string(obj, "claim_frequency_type_code", aggregate->claim_envelope.claim_frequency_type_code, sizeof(aggregate->claim_envelope.claim_frequency_type_code));
    (void)json_read_string(obj, "provider_signature_indicator", aggregate->claim_envelope.provider_signature_indicator, sizeof(aggregate->claim_envelope.provider_signature_indicator));
    (void)json_read_string(obj, "assignment_or_plan_participation_code", aggregate->claim_envelope.assignment_or_plan_participation_code, sizeof(aggregate->claim_envelope.assignment_or_plan_participation_code));
    (void)json_read_string(obj, "benefits_assignment_certification_indicator", aggregate->claim_envelope.benefits_assignment_certification_indicator, sizeof(aggregate->claim_envelope.benefits_assignment_certification_indicator));
    (void)json_read_string(obj, "release_of_information_code", aggregate->claim_envelope.release_of_information_code, sizeof(aggregate->claim_envelope.release_of_information_code));
    (void)json_read_string(obj, "patient_signature_source_code", aggregate->claim_envelope.patient_signature_source_code, sizeof(aggregate->claim_envelope.patient_signature_source_code));

    return X12_OK;
}

static int hydrate_party_context(stitched_party_context_t *party, yyjson_val *state_obj, const char *key)
{
    yyjson_val *obj;

    obj = yyjson_obj_get(state_obj, key);
    if (obj == NULL || !yyjson_is_obj(obj)) {
        return X12_OK;
    }

    (void)json_read_string(obj, "payer_responsibility_sequence_number_code", party->payer_responsibility_sequence_number_code, sizeof(party->payer_responsibility_sequence_number_code));
    (void)json_read_string(obj, "individual_relationship_code", party->individual_relationship_code, sizeof(party->individual_relationship_code));
    (void)json_read_string(obj, "insured_group_or_policy_number", party->insured_group_or_policy_number, sizeof(party->insured_group_or_policy_number));
    (void)json_read_string(obj, "insured_group_or_policy_number_token", party->insured_group_or_policy_number_token, sizeof(party->insured_group_or_policy_number_token));
    if (party->insured_group_or_policy_number_token[0] == '\0') {
        scribe_copy_cstr(
            party->insured_group_or_policy_number_token,
            sizeof(party->insured_group_or_policy_number_token),
            party->insured_group_or_policy_number
        );
        party->insured_group_or_policy_number[0] = '\0';
    }
    (void)json_read_string(obj, "claim_filing_indicator_code", party->claim_filing_indicator_code, sizeof(party->claim_filing_indicator_code));
    (void)json_read_string(obj, "date_format", party->date_format, sizeof(party->date_format));
    (void)json_read_string(obj, "date_of_birth", party->date_of_birth, sizeof(party->date_of_birth));
    (void)json_read_string(obj, "date_of_birth_token", party->date_of_birth_token, sizeof(party->date_of_birth_token));
    if (party->date_of_birth_token[0] == '\0') {
        scribe_copy_cstr(
            party->date_of_birth_token,
            sizeof(party->date_of_birth_token),
            party->date_of_birth
        );
        party->date_of_birth[0] = '\0';
    }
    (void)json_read_string(obj, "gender_code", party->gender_code, sizeof(party->gender_code));

    return X12_OK;
}

static int hydrate_claim_dates(claim_aggregate_t *aggregate, yyjson_val *state_obj)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;

    arr = yyjson_obj_get(state_obj, "claim_dates");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    aggregate->claim_date_count = 0u;
    yyjson_arr_foreach(arr, idx, max, item) {
        stitched_claim_date_t *date;

        if (!yyjson_is_obj(item)) {
            continue;
        }
        if (aggregate->claim_date_count >= STITCH_MAX_CLAIM_DATES) {
            return X12_ERR_NO_MEMORY;
        }
        date = &aggregate->claim_dates[aggregate->claim_date_count++];
        (void)json_read_string(item, "date_qualifier", date->date_qualifier, sizeof(date->date_qualifier));
        (void)json_read_string(item, "date_format", date->date_format, sizeof(date->date_format));
        (void)json_read_string(item, "date_value", date->date_value, sizeof(date->date_value));
    }

    return X12_OK;
}

static int hydrate_diagnoses(claim_aggregate_t *aggregate, yyjson_val *state_obj)
{
    yyjson_val *obj;
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;

    obj = yyjson_obj_get(state_obj, "diagnoses");
    if (obj == NULL || !yyjson_is_obj(obj)) {
        return X12_OK;
    }

    (void)json_read_string(
        obj,
        "principal_diagnosis_code",
        aggregate->principal_diagnosis_code,
        sizeof(aggregate->principal_diagnosis_code)
    );
    arr = yyjson_obj_get(obj, "other_diagnosis_codes");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    aggregate->other_diagnosis_count = 0u;
    yyjson_arr_foreach(arr, idx, max, item) {
        if (!yyjson_is_str(item)) {
            continue;
        }
        if (aggregate->other_diagnosis_count >= STITCH_MAX_DIAGNOSES) {
            return X12_ERR_NO_MEMORY;
        }
        scribe_copy_cstr(
            aggregate->other_diagnosis_codes[aggregate->other_diagnosis_count],
            sizeof(aggregate->other_diagnosis_codes[aggregate->other_diagnosis_count]),
            yyjson_get_str(item)
        );
        aggregate->other_diagnosis_count++;
    }

    return X12_OK;
}

static int hydrate_healthcare_codes(claim_aggregate_t *aggregate, yyjson_val *state_obj)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;

    arr = yyjson_obj_get(state_obj, "healthcare_codes");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    aggregate->healthcare_code_count = 0u;
    yyjson_arr_foreach(arr, idx, max, item) {
        stitched_healthcare_code_t *code;
        yyjson_val *components;
        yyjson_val *component;
        size_t component_idx;
        size_t component_max;

        if (!yyjson_is_obj(item)) {
            continue;
        }
        if (aggregate->healthcare_code_count >= STITCH_MAX_HEALTHCARE_CODES) {
            return X12_ERR_NO_MEMORY;
        }

        code = &aggregate->healthcare_codes[aggregate->healthcare_code_count++];
        (void)json_read_string(item, "healthcare_code_kind", code->healthcare_code_kind, sizeof(code->healthcare_code_kind));
        (void)json_read_string(item, "healthcare_code_qualifier", code->healthcare_code_qualifier, sizeof(code->healthcare_code_qualifier));
        (void)json_read_string(item, "healthcare_code", code->healthcare_code, sizeof(code->healthcare_code));
        (void)json_read_string(item, "healthcare_code_date_format", code->healthcare_code_date_format, sizeof(code->healthcare_code_date_format));
        (void)json_read_string(item, "healthcare_code_date_value", code->healthcare_code_date_value, sizeof(code->healthcare_code_date_value));
        (void)json_read_string(item, "healthcare_code_amount", code->healthcare_code_amount, sizeof(code->healthcare_code_amount));

        components = yyjson_obj_get(item, "healthcare_code_components");
        if (components != NULL && yyjson_is_arr(components)) {
            yyjson_arr_foreach(components, component_idx, component_max, component) {
                if (!yyjson_is_str(component)) {
                    continue;
                }
                if (code->healthcare_code_component_count >= STITCH_MAX_HEALTHCARE_CODE_COMPONENTS) {
                    break;
                }
                scribe_copy_cstr(
                    code->healthcare_code_components[code->healthcare_code_component_count],
                    sizeof(code->healthcare_code_components[code->healthcare_code_component_count]),
                    yyjson_get_str(component)
                );
                code->healthcare_code_component_count++;
            }
        }
    }

    return X12_OK;
}

static int hydrate_provider_taxonomies(claim_aggregate_t *aggregate, yyjson_val *state_obj)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;

    arr = yyjson_obj_get(state_obj, "provider_taxonomies");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    aggregate->provider_taxonomy_count = 0u;
    yyjson_arr_foreach(arr, idx, max, item) {
        stitched_provider_taxonomy_t *taxonomy;

        if (!yyjson_is_obj(item)) {
            continue;
        }
        if (aggregate->provider_taxonomy_count >= STITCH_MAX_PROVIDER_TAXONOMIES) {
            return X12_ERR_NO_MEMORY;
        }

        taxonomy = &aggregate->provider_taxonomies[aggregate->provider_taxonomy_count++];
        (void)json_read_string(item, "reference_scope", taxonomy->reference_scope, sizeof(taxonomy->reference_scope));
        (void)json_read_string(item, "service_line_number", taxonomy->service_line_number, sizeof(taxonomy->service_line_number));
        (void)json_read_string(item, "provider_context", taxonomy->provider_context, sizeof(taxonomy->provider_context));
        (void)json_read_string(item, "provider_role_code", taxonomy->provider_role_code, sizeof(taxonomy->provider_role_code));
        (void)json_read_string(item, "reference_identification_qualifier", taxonomy->reference_identification_qualifier, sizeof(taxonomy->reference_identification_qualifier));
        (void)json_read_string(item, "provider_taxonomy_code", taxonomy->provider_taxonomy_code, sizeof(taxonomy->provider_taxonomy_code));
    }

    return X12_OK;
}

static int hydrate_institutional_claim(claim_aggregate_t *aggregate, yyjson_val *state_obj)
{
    yyjson_val *obj;

    obj = yyjson_obj_get(state_obj, "institutional_claim");
    if (obj == NULL || !yyjson_is_obj(obj)) {
        return X12_OK;
    }

    (void)json_read_string(obj, "admission_type_code", aggregate->institutional_claim.admission_type_code, sizeof(aggregate->institutional_claim.admission_type_code));
    (void)json_read_string(obj, "admission_source_code", aggregate->institutional_claim.admission_source_code, sizeof(aggregate->institutional_claim.admission_source_code));
    (void)json_read_string(obj, "patient_status_code", aggregate->institutional_claim.patient_status_code, sizeof(aggregate->institutional_claim.patient_status_code));
    (void)json_read_string(obj, "admission_date", aggregate->institutional_claim.admission_date, sizeof(aggregate->institutional_claim.admission_date));
    (void)json_read_string(obj, "discharge_date", aggregate->institutional_claim.discharge_date, sizeof(aggregate->institutional_claim.discharge_date));
    (void)json_read_string(obj, "diagnosis_related_group_code", aggregate->institutional_claim.diagnosis_related_group_code, sizeof(aggregate->institutional_claim.diagnosis_related_group_code));

    return X12_OK;
}

static int hydrate_adjustments(
    stitched_service_line_t *line,
    yyjson_val *line_obj
)
{
    yyjson_val *arr;
    yyjson_val *item;
    yyjson_val *values;
    yyjson_val *value;
    size_t idx;
    size_t max;
    size_t value_idx;
    size_t value_max;

    arr = yyjson_obj_get(line_obj, "adjustments");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    yyjson_arr_foreach(arr, idx, max, item) {
        stitched_line_adjustment_t *adjustment;

        if (!yyjson_is_obj(item)) {
            continue;
        }
        if (line->adjustment_count >= STITCH_MAX_ADJUSTMENTS_PER_LINE) {
            return X12_ERR_NO_MEMORY;
        }

        adjustment = &line->adjustments[line->adjustment_count++];
        (void)json_read_string(
            item,
            "adjustment_group_code",
            adjustment->adjustment_group_code,
            sizeof(adjustment->adjustment_group_code)
        );

        values = yyjson_obj_get(item, "reason_codes");
        if (values != NULL && yyjson_is_arr(values)) {
            yyjson_arr_foreach(values, value_idx, value_max, value) {
                if (adjustment->value_count >= STITCH_MAX_ADJUSTMENT_VALUES) {
                    break;
                }
                if (yyjson_is_str(value)) {
                    scribe_copy_cstr(
                        adjustment->reason_codes[adjustment->value_count],
                        sizeof(adjustment->reason_codes[adjustment->value_count]),
                        yyjson_get_str(value)
                    );
                    adjustment->value_count++;
                }
            }
        }

        values = yyjson_obj_get(item, "amounts");
        if (values != NULL && yyjson_is_arr(values)) {
            yyjson_arr_foreach(values, value_idx, value_max, value) {
                if (value_idx >= STITCH_MAX_ADJUSTMENT_VALUES) {
                    break;
                }
                if (yyjson_is_str(value)) {
                    scribe_copy_cstr(
                        adjustment->amounts[value_idx],
                        sizeof(adjustment->amounts[value_idx]),
                        yyjson_get_str(value)
                    );
                    if (value_idx >= adjustment->value_count) {
                        adjustment->value_count = value_idx + 1u;
                    }
                }
            }
        }

        values = yyjson_obj_get(item, "quantities");
        if (values != NULL && yyjson_is_arr(values)) {
            yyjson_arr_foreach(values, value_idx, value_max, value) {
                if (value_idx >= STITCH_MAX_ADJUSTMENT_VALUES) {
                    break;
                }
                if (yyjson_is_str(value)) {
                    scribe_copy_cstr(
                        adjustment->quantities[value_idx],
                        sizeof(adjustment->quantities[value_idx]),
                        yyjson_get_str(value)
                    );
                    if (value_idx >= adjustment->value_count) {
                        adjustment->value_count = value_idx + 1u;
                    }
                }
            }
        }
    }

    return X12_OK;
}

static int hydrate_service_lines(claim_aggregate_t *aggregate, yyjson_val *root)
{
    yyjson_val *arr;
    yyjson_val *item;
    size_t idx;
    size_t max;
    int rc;

    arr = yyjson_obj_get(root, "service_lines");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return X12_OK;
    }

    yyjson_arr_foreach(arr, idx, max, item) {
        stitched_service_line_t *line;
        yyjson_val *submitted;
        yyjson_val *remittance;

        if (!yyjson_is_obj(item)) {
            continue;
        }

        line = claim_aggregate_add_service_line(aggregate);
        if (line == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        rc = json_read_string(
            item,
            "service_line_number",
            line->service_line_number,
            sizeof(line->service_line_number)
        );
        if (rc == X12_OK) {
            rc = json_read_string(
                item,
                "remittance_service_line_number",
                line->remit_service_line_number,
                sizeof(line->remit_service_line_number)
            );
        }
        if (rc == X12_OK) {
            rc = json_read_string(item, "procedure_code", line->procedure_code, sizeof(line->procedure_code));
        }
        if (rc == X12_OK) {
            rc = json_read_string(item, "description", line->description, sizeof(line->description));
        }
        if (rc == X12_OK) {
            rc = json_read_string(item, "service_date", line->service_date, sizeof(line->service_date));
        }
        if (rc == X12_OK) {
            rc = json_read_string(item, "match_method", line->match_method, sizeof(line->match_method));
        }
        if (rc != X12_OK) {
            return rc;
        }

        submitted = yyjson_obj_get(item, "submitted");
        if (submitted != NULL && yyjson_is_obj(submitted)) {
            line->has_submitted = 1;
            (void)json_read_string(submitted, "line_type", line->submitted_line_type, sizeof(line->submitted_line_type));
            (void)json_read_string(submitted, "procedure_code_qualifier", line->submitted_procedure_code_qualifier, sizeof(line->submitted_procedure_code_qualifier));
            (void)json_read_string(submitted, "procedure_code_set", line->submitted_procedure_code_set, sizeof(line->submitted_procedure_code_set));
            (void)json_read_string(submitted, "charge_amount", line->submitted_charge_amount, sizeof(line->submitted_charge_amount));
            (void)json_read_string(submitted, "unit_measure_code", line->submitted_unit_measure_code, sizeof(line->submitted_unit_measure_code));
            (void)json_read_string(submitted, "unit_count", line->submitted_unit_count, sizeof(line->submitted_unit_count));
            (void)json_read_string(submitted, "service_date", line->submitted_service_date, sizeof(line->submitted_service_date));
        }

        remittance = yyjson_obj_get(item, "remittance");
        if (remittance != NULL && yyjson_is_obj(remittance)) {
            line->has_remittance = 1;
            (void)json_read_string(remittance, "procedure_code_qualifier", line->remittance_procedure_code_qualifier, sizeof(line->remittance_procedure_code_qualifier));
            (void)json_read_string(remittance, "procedure_code_set", line->remittance_procedure_code_set, sizeof(line->remittance_procedure_code_set));
            (void)json_read_string(remittance, "line_charge_amount", line->remittance_line_charge_amount, sizeof(line->remittance_line_charge_amount));
            (void)json_read_string(remittance, "line_paid_amount", line->remittance_line_paid_amount, sizeof(line->remittance_line_paid_amount));
            (void)json_read_string(remittance, "paid_service_unit_count", line->remittance_paid_unit_count, sizeof(line->remittance_paid_unit_count));
            (void)json_read_string(remittance, "service_date", line->remittance_service_date, sizeof(line->remittance_service_date));
        }

        rc = hydrate_reference_array(
            line->references,
            STITCH_MAX_REFERENCES_PER_LINE,
            &line->reference_count,
            item,
            "references"
        );
        if (rc == X12_OK) {
            rc = hydrate_adjustments(line, item);
        }
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

int claim_stitch_hydrate_snapshot(
    stitch_state_t *state,
    const char *aggregate_id,
    const char *state_json
)
{
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *keys;
    yyjson_val *snapshot_state;
    claim_aggregate_t *aggregate;
    const char *key;
    int rc = X12_OK;

    if (state == NULL || aggregate_id == NULL || state_json == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (state->aggregate_count >= STITCH_MAX_AGGREGATES) {
        return X12_ERR_NO_MEMORY;
    }

    key = strncmp(aggregate_id, "claim:", 6u) == 0 ? aggregate_id + 6u : aggregate_id;
    aggregate = &state->aggregates[state->aggregate_count++];
    memset(aggregate, 0, sizeof(*aggregate));
    claim_aggregate_init(aggregate);
    scribe_copy_cstr(aggregate->key, sizeof(aggregate->key), key);

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

    snapshot_get_size(root, "version", &aggregate->version);
    keys = yyjson_obj_get(root, "keys");
    if (keys != NULL && yyjson_is_obj(keys)) {
        (void)json_read_string(keys, "claim_id", aggregate->claim_id, sizeof(aggregate->claim_id));
        (void)json_read_string(keys, "claim_id_token", aggregate->claim_id_token, sizeof(aggregate->claim_id_token));
        (void)json_read_string(keys, "payer_claim_control_number", aggregate->payer_claim_control_number, sizeof(aggregate->payer_claim_control_number));
        (void)json_read_string(keys, "payer_claim_control_number_token", aggregate->payer_claim_control_number_token, sizeof(aggregate->payer_claim_control_number_token));
        (void)json_read_string(keys, "patient_id", aggregate->patient_id, sizeof(aggregate->patient_id));
        (void)json_read_string(keys, "patient_id_token", aggregate->patient_id_token, sizeof(aggregate->patient_id_token));
        (void)json_read_string(keys, "patient_name_token", aggregate->patient_name_token, sizeof(aggregate->patient_name_token));
    }
    if (aggregate->claim_id_token[0] == '\0') {
        scribe_copy_cstr(aggregate->claim_id_token, sizeof(aggregate->claim_id_token), aggregate->key);
    }

    snapshot_state = yyjson_obj_get(root, "state");
    if (snapshot_state != NULL && yyjson_is_obj(snapshot_state)) {
        snapshot_get_bool(snapshot_state, "has_837", &aggregate->has_837);
        snapshot_get_bool(snapshot_state, "has_835", &aggregate->has_835);
        (void)json_read_string(snapshot_state, "claim_type", aggregate->claim_type, sizeof(aggregate->claim_type));
        (void)json_read_string(snapshot_state, "claim_status_code", aggregate->claim_status_code, sizeof(aggregate->claim_status_code));
        snapshot_get_size(snapshot_state, "submitted_service_line_count", &aggregate->submitted_service_line_count);
        snapshot_get_size(snapshot_state, "remittance_service_line_count", &aggregate->remittance_service_line_count);
        snapshot_get_size(snapshot_state, "adjustment_count", &aggregate->adjustment_count);
        rc = hydrate_claim_envelope(aggregate, snapshot_state);
        if (rc == X12_OK) {
            rc = hydrate_party_context(&aggregate->subscriber, snapshot_state, "subscriber");
        }
        if (rc == X12_OK) {
            rc = hydrate_party_context(&aggregate->patient, snapshot_state, "patient");
        }
        if (rc == X12_OK) {
            rc = hydrate_claim_dates(aggregate, snapshot_state);
        }
        if (rc == X12_OK) {
            rc = hydrate_reference_array(
                aggregate->claim_references,
                STITCH_MAX_REFERENCES_PER_CLAIM,
                &aggregate->claim_reference_count,
                snapshot_state,
                "claim_references"
            );
        }
        if (rc == X12_OK) {
            rc = hydrate_diagnoses(aggregate, snapshot_state);
        }
        if (rc == X12_OK) {
            rc = hydrate_healthcare_codes(aggregate, snapshot_state);
        }
        if (rc == X12_OK) {
            rc = hydrate_provider_taxonomies(aggregate, snapshot_state);
        }
        if (rc == X12_OK) {
            rc = hydrate_institutional_claim(aggregate, snapshot_state);
        }
        if (rc != X12_OK) {
            yyjson_doc_free(doc);
            state->aggregate_count--;
            return rc;
        }
    }

    rc = hydrate_applied_event_ids(aggregate, root);
    if (rc == X12_OK) {
        rc = hydrate_service_lines(aggregate, root);
    }
    yyjson_doc_free(doc);
    if (rc != X12_OK) {
        state->aggregate_count--;
    }
    return rc;
}

int claim_stitch_flush_update_batches(stitch_state_t *state)
{
    stitch_update_batch_t *notification_batch = NULL;
    size_t aggregate_version_count = 0u;
    size_t i;
    int txn_open = 0;
    int rc;

    if (state == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    for (i = 0u; i < state->update_batch_count; i++) {
        if (state->update_batches[i].source_event_count == 0u) {
            continue;
        }
        if (notification_batch == NULL) {
            notification_batch = &state->update_batches[i];
        }
        aggregate_version_count++;
    }

    if (aggregate_version_count > 0u && state->read_store != NULL) {
        TRY(scribe_store_begin_immediate(state->read_store));
        txn_open = 1;
    }

    for (i = 0u; i < state->update_batch_count; i++) {
        if (state->update_batches[i].source_event_count == 0u) {
            continue;
        }
        rc = write_snapshot(state, &state->update_batches[i]);
        if (rc != X12_OK) {
            if (txn_open) {
                (void)scribe_store_rollback(state->read_store);
            }
            return rc;
        }
    }

    if (aggregate_version_count > 0u) {
        rc = write_notification(state, notification_batch, aggregate_version_count);
        if (rc != X12_OK) {
            if (txn_open) {
                (void)scribe_store_rollback(state->read_store);
            }
            return rc;
        }
    }

    if (txn_open) {
        rc = scribe_store_commit(state->read_store);
        if (rc != X12_OK) {
            (void)scribe_store_rollback(state->read_store);
            return rc;
        }
    }

    state->update_batch_count = 0u;
    return X12_OK;
}
