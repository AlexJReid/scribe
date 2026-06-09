#include "x12_mapper_835.h"

#include "tokenise.h"

#include <stdio.h>
#include <string.h>

static x12_str_t empty_str(void)
{
    x12_str_t value;

    value.ptr = "";
    value.len = 0;
    return value;
}

static x12_str_t element_or_empty(const x12_segment_t *seg, size_t index)
{
    if (seg->element_count <= index) {
        return empty_str();
    }

    return seg->elements[index];
}

static int write_phi_string_field(
    FILE *fp,
    const char *name,
    x12_str_t value,
    int prefix_comma,
    int include_phi
)
{
    if (!include_phi) {
        return X12_OK;
    }

    return event_writer_write_string_field(fp, name, value, prefix_comma);
}

static int write_tokenized_or_phi_field(
    event_writer_t *writer,
    FILE *fp,
    const char *name,
    token_type_t type,
    x12_str_t raw,
    int prefix_comma,
    int include_phi
)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t token_value;
    int rc;

    if (include_phi) {
        rc = event_writer_record_phi_mapping(writer, type, raw);
        if (rc != X12_OK) {
            return rc;
        }
        return event_writer_write_string_field(fp, name, raw, prefix_comma);
    }

    if (raw.len == 0u) {
        return event_writer_write_string_field(fp, name, empty_str(), prefix_comma);
    }

    rc = tokenise_value(type, raw, token, sizeof(token));
    if (rc != X12_OK) {
        return rc;
    }
    rc = event_writer_record_phi_mapping(writer, type, raw);
    if (rc != X12_OK) {
        return rc;
    }

    token_value.ptr = token;
    token_value.len = strlen(token);
    return event_writer_write_string_field(fp, name, token_value, prefix_comma);
}

static int write_phi_token_field(
    FILE *fp,
    const char *name,
    token_type_t type,
    x12_str_t raw,
    int prefix_comma,
    int include_phi
)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t token_value;
    int rc;

    if (!include_phi) {
        return X12_OK;
    }

    if (raw.len == 0u) {
        return event_writer_write_string_field(fp, name, empty_str(), prefix_comma);
    }

    rc = tokenise_value(type, raw, token, sizeof(token));
    if (rc != X12_OK) {
        return rc;
    }

    token_value.ptr = token;
    token_value.len = strlen(token);
    return event_writer_write_string_field(fp, name, token_value, prefix_comma);
}

static void split_first_component(
    x12_str_t value,
    char component_sep,
    x12_str_t *qualifier,
    x12_str_t *code
)
{
    size_t i;

    *qualifier = empty_str();
    *code = empty_str();

    for (i = 0; i < value.len; i++) {
        if (value.ptr[i] == component_sep) {
            qualifier->ptr = value.ptr;
            qualifier->len = i;
            code->ptr = value.ptr + i + 1u;
            code->len = value.len - i - 1u;
            return;
        }
    }

    *code = value;
}

static const char *procedure_code_set(x12_str_t qualifier)
{
    if (x12_str_eq_cstr(qualifier, "HC")) {
        return "CPT/HCPCS";
    }

    return "";
}

static int write_payload_start(FILE *fp)
{
    return fputc('{', fp) == EOF ? X12_ERR_IO : X12_OK;
}

static int write_payload_end(FILE *fp)
{
    return fputc('}', fp) == EOF ? X12_ERR_IO : X12_OK;
}

static token_type_t name_token_type_for_id(token_type_t id_token_type)
{
    switch (id_token_type) {
    case TOK_PATIENT_ID:
        return TOK_PATIENT_NAME;
    case TOK_MEMBER_ID:
        return TOK_MEMBER_NAME;
    case TOK_PROVIDER_ID:
        return TOK_PROVIDER_NAME;
    case TOK_PAYER_ID:
        return TOK_PAYER_NAME;
    default:
        return TOK_ENTITY_NAME;
    }
}

static int write_common_claim_fields(FILE *fp, x12_mapper_835_t *mapper)
{
    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            "claim_id",
            TOK_CLAIM_ID,
            mapper->current_claim_id,
            0,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            "claim_id_token",
            TOK_CLAIM_ID,
            mapper->current_claim_id,
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "remittance_id", mapper->remittance_id, 1) != X12_OK) {
        return X12_ERR_IO;
    }

    return X12_OK;
}

static const char *current_date_scope(const x12_mapper_835_t *mapper)
{
    if (mapper->in_service_line) {
        return "service_line";
    }
    if (mapper->in_claim) {
        return "claim";
    }

    return "transaction";
}

static void clear_claim_position(x12_mapper_835_t *mapper)
{
    mapper->current_claim_id = empty_str();
    mapper->current_service_line_index = 0u;
    mapper->current_service_line_number[0] = '\0';
    mapper->in_claim = 0;
    mapper->in_service_line = 0;
}

static int set_next_service_line_number(x12_mapper_835_t *mapper)
{
    int written;

    mapper->current_service_line_index++;
    written = snprintf(
        mapper->current_service_line_number,
        sizeof(mapper->current_service_line_number),
        "%zu",
        mapper->current_service_line_index
    );
    if (written < 0 ||
        (size_t)written >= sizeof(mapper->current_service_line_number)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return X12_OK;
}

static x12_str_t current_service_line_number(x12_mapper_835_t *mapper)
{
    x12_str_t value;

    if (!mapper->in_service_line) {
        return empty_str();
    }

    value.ptr = mapper->current_service_line_number;
    value.len = strlen(mapper->current_service_line_number);
    return value;
}

static token_type_t n1_id_token_type(x12_str_t entity_code)
{
    if (x12_str_eq_cstr(entity_code, "PE")) {
        return TOK_PROVIDER_ID;
    }
    if (x12_str_eq_cstr(entity_code, "PR")) {
        return TOK_PAYER_ID;
    }

    return TOK_UNKNOWN;
}

static int write_remittance_advice_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    const x12_segment_t *bpr = &mapper->bpr.segment;
    int rc;

    mapper->remittance_id = element_or_empty(seg, 1);

    rc = event_writer_begin_event(mapper->writer, "RemittanceAdviceObserved", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "remittance_id", mapper->remittance_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "trace_type_code", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "trace_number", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "originating_company_id", element_or_empty(seg, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "transaction_handling_code", element_or_empty(bpr, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "payment_amount", element_or_empty(bpr, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "credit_debit_flag", element_or_empty(bpr, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "payment_method_code", element_or_empty(bpr, 3), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "payment_date", element_or_empty(bpr, 15), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_remittance_party_referenced(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    token_type_t token_type = n1_id_token_type(element_or_empty(seg, 0));
    int rc;

    rc = event_writer_begin_event(mapper->writer, "RemittancePartyReferenced", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "remittance_id", mapper->remittance_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "entity_identifier_code", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_string_field(
            fp,
            "name",
            element_or_empty(seg, 1),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_record_phi_name(
            mapper->writer,
            name_token_type_for_id(token_type),
            element_or_empty(seg, 1),
            empty_str(),
            token_type,
            element_or_empty(seg, 3)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "id_qualifier", element_or_empty(seg, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            "id_value",
            token_type,
            element_or_empty(seg, 3),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            "id_value_token",
            token_type,
            element_or_empty(seg, 3),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_claim_payment_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    mapper->current_claim_id = element_or_empty(seg, 0);
    mapper->current_service_line_index = 0u;
    mapper->current_service_line_number[0] = '\0';
    mapper->in_claim = 1;
    mapper->in_service_line = 0;

    rc = event_writer_begin_event(mapper->writer, "RemittanceClaimPaymentObserved", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_common_claim_fields(fp, mapper) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "claim_status_code", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "total_charge_amount", element_or_empty(seg, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "paid_amount", element_or_empty(seg, 3), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "patient_responsibility_amount", element_or_empty(seg, 4), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "claim_filing_indicator_code", element_or_empty(seg, 5), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            "payer_claim_control_number",
            TOK_PAYER_CLAIM_CONTROL_NUMBER,
            element_or_empty(seg, 6),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            "payer_claim_control_number_token",
            TOK_PAYER_CLAIM_CONTROL_NUMBER,
            element_or_empty(seg, 6),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "facility_type_code", element_or_empty(seg, 7), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "claim_frequency_type_code", element_or_empty(seg, 8), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_nm1_reference(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg,
    const char *event_type,
    token_type_t id_token_type
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, event_type, seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_common_claim_fields(fp, mapper) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "entity_type", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_string_field(
            fp,
            "last_name_or_org",
            element_or_empty(seg, 2),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_string_field(
            fp,
            "first_name",
            element_or_empty(seg, 3),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_record_phi_name(
            mapper->writer,
            name_token_type_for_id(id_token_type),
            element_or_empty(seg, 2),
            element_or_empty(seg, 3),
            id_token_type,
            element_or_empty(seg, 8)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "id_qualifier", element_or_empty(seg, 7), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            "id_value",
            id_token_type,
            element_or_empty(seg, 8),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            "id_value_token",
            id_token_type,
            element_or_empty(seg, 8),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_service_line_payment_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    x12_str_t procedure_qualifier;
    x12_str_t procedure_code;
    int rc;

    rc = set_next_service_line_number(mapper);
    if (rc != X12_OK) {
        return rc;
    }
    mapper->in_service_line = 1;

    split_first_component(element_or_empty(seg, 0), mapper->component_sep, &procedure_qualifier, &procedure_code);

    rc = event_writer_begin_event(mapper->writer, "RemittanceServiceLinePaymentObserved", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_common_claim_fields(fp, mapper) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_line_number", current_service_line_number(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "procedure_code_qualifier", procedure_qualifier, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "procedure_code_set", procedure_code_set(procedure_qualifier), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "procedure_code", procedure_code, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "line_charge_amount", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "line_paid_amount", element_or_empty(seg, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "paid_service_unit_count", element_or_empty(seg, 4), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_adjustment_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    x12_str_t reason_codes[32];
    x12_str_t amounts[32];
    x12_str_t quantities[32];
    size_t adjustment_count = 0u;
    size_t i;
    int rc;

    for (i = 1u; i < seg->element_count && adjustment_count < 32u; i += 3u) {
        reason_codes[adjustment_count] = element_or_empty(seg, i);
        amounts[adjustment_count] = element_or_empty(seg, i + 1u);
        quantities[adjustment_count] = element_or_empty(seg, i + 2u);
        adjustment_count++;
    }

    rc = event_writer_begin_event(mapper->writer, "RemittanceAdjustmentObserved", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_common_claim_fields(fp, mapper) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "adjustment_scope", current_date_scope(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_line_number", current_service_line_number(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "adjustment_group_code", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(fp, "reason_codes", reason_codes, adjustment_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(fp, "amounts", amounts, adjustment_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(fp, "quantities", quantities, adjustment_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_date_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, "RemittanceDateRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "remittance_id", mapper->remittance_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            "claim_id",
            TOK_CLAIM_ID,
            mapper->current_claim_id,
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            "claim_id_token",
            TOK_CLAIM_ID,
            mapper->current_claim_id,
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "date_scope", current_date_scope(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_line_number", current_service_line_number(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "date_qualifier", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "date_value", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

void x12_mapper_835_init(x12_mapper_835_t *mapper, event_writer_t *writer)
{
    if (mapper == NULL) {
        return;
    }

    memset(mapper, 0, sizeof(*mapper));

    mapper->writer = writer;
    mapper->remittance_id = empty_str();
    mapper->current_claim_id = empty_str();
    mapper->component_sep = ':';
}

int x12_mapper_835_on_segment(const x12_segment_t *seg, void *user)
{
    x12_mapper_835_t *mapper = (x12_mapper_835_t *)user;

    if (seg == NULL || mapper == NULL || mapper->writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    event_writer_observe_control(mapper->writer, seg);

    if (x12_str_eq_cstr(seg->tag, "ISA") &&
        seg->element_count > 15u &&
        seg->elements[15].len > 0u) {
        mapper->component_sep = seg->elements[15].ptr[0];
    }

    if (x12_str_eq_cstr(seg->tag, "BPR")) {
        mapper->bpr.present = 1;
        mapper->bpr.segment = *seg;
        clear_claim_position(mapper);
        return X12_OK;
    }

    if (x12_str_eq_cstr(seg->tag, "TRN")) {
        return write_remittance_advice_observed(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "N1")) {
        return write_remittance_party_referenced(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "CLP")) {
        return write_claim_payment_observed(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "NM1") && seg->element_count > 0u) {
        if (x12_str_eq_cstr(seg->elements[0], "QC")) {
            return write_nm1_reference(mapper, seg, "RemittanceClaimReferencedPatient", TOK_PATIENT_ID);
        }
        if (x12_str_eq_cstr(seg->elements[0], "IL")) {
            return write_nm1_reference(mapper, seg, "RemittanceClaimReferencedSubscriber", TOK_MEMBER_ID);
        }
    }

    if (x12_str_eq_cstr(seg->tag, "SVC")) {
        return write_service_line_payment_observed(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "CAS")) {
        return write_adjustment_observed(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "DTM")) {
        return write_date_observed(mapper, seg);
    }

    return X12_OK;
}

int x12_map_835_document(x12_document_t *doc, event_writer_t *writer)
{
    x12_mapper_835_t mapper;

    if (doc == NULL || writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    x12_mapper_835_init(&mapper, writer);
    return x12_document_each_segment(doc, x12_mapper_835_on_segment, &mapper);
}
