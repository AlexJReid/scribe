#include "x12_mapper_835.h"

#include "tokenise.h"
#include "x12_mapper_phi.h"

#include <stdio.h>
#include <string.h>

static void split_first_component(
    x12_str_t value,
    char component_sep,
    x12_str_t *qualifier,
    x12_str_t *code)
{
    size_t i;

    *qualifier = x12_mapper_empty_str();
    *code = x12_mapper_empty_str();

    for (i = 0; i < value.len; i++)
    {
        if (value.ptr[i] == component_sep)
        {
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
    if (x12_str_eq_cstr(qualifier, "HC"))
    {
        return "CPT/HCPCS";
    }
    return "";
}

static token_type_t name_token_type_for_id(token_type_t id_token_type)
{
    switch (id_token_type)
    {
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

static int add_common_claim_fields(x12_mapper_835_t *mapper)
{
    event_writer_t *w = mapper->writer;

    TRY(x12_mapper_add_tokenized_or_phi(w, "claim_id", TOK_CLAIM_ID, mapper->current_claim_id));
    TRY(x12_mapper_add_phi_token(w, "claim_id_token", TOK_CLAIM_ID, mapper->current_claim_id));
    return event_writer_add_str(w, "remittance_id", mapper->remittance_id);
}

static const char *current_date_scope(const x12_mapper_835_t *mapper)
{
    if (mapper->in_service_line)
    {
        return "service_line";
    }
    if (mapper->in_claim)
    {
        return "claim";
    }

    return "transaction";
}

static void clear_claim_position(x12_mapper_835_t *mapper)
{
    mapper->current_claim_id = x12_mapper_empty_str();
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
        mapper->current_service_line_index);
    if (written < 0 ||
        (size_t)written >= sizeof(mapper->current_service_line_number))
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return X12_OK;
}

static x12_str_t current_service_line_number(x12_mapper_835_t *mapper)
{
    x12_str_t value;

    if (!mapper->in_service_line)
    {
        return x12_mapper_empty_str();
    }

    value.ptr = mapper->current_service_line_number;
    value.len = strlen(mapper->current_service_line_number);
    return value;
}

static token_type_t n1_id_token_type(x12_str_t entity_code)
{
    if (x12_str_eq_cstr(entity_code, "PE"))
    {
        return TOK_PROVIDER_ID;
    }
    if (x12_str_eq_cstr(entity_code, "PR"))
    {
        return TOK_PAYER_ID;
    }
    return TOK_UNKNOWN;
}

static int write_remittance_advice_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;
    const x12_segment_t *bpr = &mapper->bpr.segment;

    mapper->remittance_id = x12_mapper_element_or_empty(seg, 1);

    TRY(event_writer_begin_event(w, "RemittanceAdviceObserved", seg));
    TRY(event_writer_add_str(w, "remittance_id", mapper->remittance_id));
    TRY(event_writer_add_str(w, "trace_type_code", x12_mapper_element_or_empty(seg, 0)));
    TRY(event_writer_add_str(w, "trace_number", x12_mapper_element_or_empty(seg, 1)));
    TRY(event_writer_add_str(w, "originating_company_id", x12_mapper_element_or_empty(seg, 2)));
    TRY(event_writer_add_str(w, "transaction_handling_code", x12_mapper_element_or_empty(bpr, 0)));
    TRY(event_writer_add_str(w, "payment_amount", x12_mapper_element_or_empty(bpr, 1)));
    TRY(event_writer_add_str(w, "credit_debit_flag", x12_mapper_element_or_empty(bpr, 2)));
    TRY(event_writer_add_str(w, "payment_method_code", x12_mapper_element_or_empty(bpr, 3)));
    TRY(event_writer_add_str(w, "payment_date", x12_mapper_element_or_empty(bpr, 15)));
    return event_writer_end_event(w);
}

static int write_remittance_party_referenced(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;
    token_type_t token_type = n1_id_token_type(x12_mapper_element_or_empty(seg, 0));

    TRY(event_writer_begin_event(w, "RemittancePartyReferenced", seg));
    TRY(event_writer_add_str(w, "remittance_id", mapper->remittance_id));
    TRY(event_writer_add_str(w, "entity_identifier_code", x12_mapper_element_or_empty(seg, 0)));
    TRY(x12_mapper_add_phi_str(w, "name", x12_mapper_element_or_empty(seg, 1)));
    TRY(event_writer_record_phi_name(
        w,
        name_token_type_for_id(token_type),
        x12_mapper_element_or_empty(seg, 1),
        x12_mapper_empty_str(),
        token_type,
        x12_mapper_element_or_empty(seg, 3)));
    TRY(event_writer_add_str(w, "id_qualifier", x12_mapper_element_or_empty(seg, 2)));
    TRY(x12_mapper_add_tokenized_or_phi(w, "id_value", token_type, x12_mapper_element_or_empty(seg, 3)));
    TRY(x12_mapper_add_phi_token(w, "id_value_token", token_type, x12_mapper_element_or_empty(seg, 3)));
    return event_writer_end_event(w);
}

static int write_claim_payment_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    mapper->current_claim_id = x12_mapper_element_or_empty(seg, 0);
    mapper->current_service_line_index = 0u;
    mapper->current_service_line_number[0] = '\0';
    mapper->in_claim = 1;
    mapper->in_service_line = 0;

    TRY(event_writer_begin_event(w, "RemittanceClaimPaymentObserved", seg));
    TRY(add_common_claim_fields(mapper));
    TRY(event_writer_add_str(w, "claim_status_code", x12_mapper_element_or_empty(seg, 1)));
    TRY(event_writer_add_str(w, "total_charge_amount", x12_mapper_element_or_empty(seg, 2)));
    TRY(event_writer_add_str(w, "paid_amount", x12_mapper_element_or_empty(seg, 3)));
    TRY(event_writer_add_str(w, "patient_responsibility_amount", x12_mapper_element_or_empty(seg, 4)));
    TRY(event_writer_add_str(w, "claim_filing_indicator_code", x12_mapper_element_or_empty(seg, 5)));
    TRY(x12_mapper_add_tokenized_or_phi(
        w, "payer_claim_control_number", TOK_PAYER_CLAIM_CONTROL_NUMBER, x12_mapper_element_or_empty(seg, 6)));
    TRY(x12_mapper_add_phi_token(
        w, "payer_claim_control_number_token", TOK_PAYER_CLAIM_CONTROL_NUMBER, x12_mapper_element_or_empty(seg, 6)));
    TRY(event_writer_add_str(w, "facility_type_code", x12_mapper_element_or_empty(seg, 7)));
    TRY(event_writer_add_str(w, "claim_frequency_type_code", x12_mapper_element_or_empty(seg, 8)));
    return event_writer_end_event(w);
}

static int write_nm1_reference(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg,
    const char *event_type,
    token_type_t id_token_type)
{
    event_writer_t *w = mapper->writer;

    TRY(event_writer_begin_event(w, event_type, seg));
    TRY(add_common_claim_fields(mapper));
    TRY(event_writer_add_str(w, "entity_type", x12_mapper_element_or_empty(seg, 1)));
    TRY(x12_mapper_add_phi_str(w, "last_name_or_org", x12_mapper_element_or_empty(seg, 2)));
    TRY(x12_mapper_add_phi_str(w, "first_name", x12_mapper_element_or_empty(seg, 3)));
    TRY(event_writer_record_phi_name(
        w,
        name_token_type_for_id(id_token_type),
        x12_mapper_element_or_empty(seg, 2),
        x12_mapper_element_or_empty(seg, 3),
        id_token_type,
        x12_mapper_element_or_empty(seg, 8)));
    TRY(event_writer_add_str(w, "id_qualifier", x12_mapper_element_or_empty(seg, 7)));
    TRY(x12_mapper_add_tokenized_or_phi(w, "id_value", id_token_type, x12_mapper_element_or_empty(seg, 8)));
    TRY(x12_mapper_add_phi_token(w, "id_value_token", id_token_type, x12_mapper_element_or_empty(seg, 8)));
    return event_writer_end_event(w);
}

static int write_service_line_payment_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;
    x12_str_t procedure_qualifier;
    x12_str_t procedure_code;

    TRY(set_next_service_line_number(mapper));
    mapper->in_service_line = 1;

    split_first_component(
        x12_mapper_element_or_empty(seg, 0), mapper->component_sep, &procedure_qualifier, &procedure_code);

    TRY(event_writer_begin_event(w, "RemittanceServiceLinePaymentObserved", seg));
    TRY(add_common_claim_fields(mapper));
    TRY(event_writer_add_str(w, "service_line_number", current_service_line_number(mapper)));
    TRY(event_writer_add_str(w, "procedure_code_qualifier", procedure_qualifier));
    TRY(event_writer_add_cstr(w, "procedure_code_set", procedure_code_set(procedure_qualifier)));
    TRY(event_writer_add_str(w, "procedure_code", procedure_code));
    TRY(event_writer_add_str(w, "line_charge_amount", x12_mapper_element_or_empty(seg, 1)));
    TRY(event_writer_add_str(w, "line_paid_amount", x12_mapper_element_or_empty(seg, 2)));
    TRY(event_writer_add_str(w, "paid_service_unit_count", x12_mapper_element_or_empty(seg, 4)));
    return event_writer_end_event(w);
}

static int write_adjustment_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;
    x12_str_t reason_codes[32];
    x12_str_t amounts[32];
    x12_str_t quantities[32];
    size_t adjustment_count = 0u;
    size_t i;

    for (i = 1u; i < seg->element_count && adjustment_count < 32u; i += 3u)
    {
        reason_codes[adjustment_count] = x12_mapper_element_or_empty(seg, i);
        amounts[adjustment_count] = x12_mapper_element_or_empty(seg, i + 1u);
        quantities[adjustment_count] = x12_mapper_element_or_empty(seg, i + 2u);
        adjustment_count++;
    }

    TRY(event_writer_begin_event(w, "RemittanceAdjustmentObserved", seg));
    TRY(add_common_claim_fields(mapper));
    TRY(event_writer_add_cstr(w, "adjustment_scope", current_date_scope(mapper)));
    TRY(event_writer_add_str(w, "service_line_number", current_service_line_number(mapper)));
    TRY(event_writer_add_str(w, "adjustment_group_code", x12_mapper_element_or_empty(seg, 0)));
    TRY(event_writer_add_str_array(w, "reason_codes", reason_codes, adjustment_count));
    TRY(event_writer_add_str_array(w, "amounts", amounts, adjustment_count));
    TRY(event_writer_add_str_array(w, "quantities", quantities, adjustment_count));
    return event_writer_end_event(w);
}

static int write_date_observed(
    x12_mapper_835_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    TRY(event_writer_begin_event(w, "RemittanceDateRecorded", seg));
    TRY(event_writer_add_str(w, "remittance_id", mapper->remittance_id));
    TRY(x12_mapper_add_tokenized_or_phi(w, "claim_id", TOK_CLAIM_ID, mapper->current_claim_id));
    TRY(x12_mapper_add_phi_token(w, "claim_id_token", TOK_CLAIM_ID, mapper->current_claim_id));
    TRY(event_writer_add_cstr(w, "date_scope", current_date_scope(mapper)));
    TRY(event_writer_add_str(w, "service_line_number", current_service_line_number(mapper)));
    TRY(event_writer_add_str(w, "date_qualifier", x12_mapper_element_or_empty(seg, 0)));
    TRY(event_writer_add_str(w, "date_value", x12_mapper_element_or_empty(seg, 1)));
    return event_writer_end_event(w);
}

void x12_mapper_835_init(x12_mapper_835_t *mapper, event_writer_t *writer)
{
    if (mapper == NULL)
    {
        return;
    }

    memset(mapper, 0, sizeof(*mapper));

    mapper->writer = writer;
    mapper->remittance_id = x12_mapper_empty_str();
    mapper->current_claim_id = x12_mapper_empty_str();
    mapper->component_sep = ':';
}

int x12_mapper_835_on_segment(const x12_segment_t *seg, void *user)
{
    x12_mapper_835_t *mapper = (x12_mapper_835_t *)user;

    if (seg == NULL || mapper == NULL || mapper->writer == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    event_writer_observe_control(mapper->writer, seg);

    if (x12_str_eq_cstr(seg->tag, "ISA") &&
        seg->element_count > 15u &&
        seg->elements[15].len > 0u)
    {
        mapper->component_sep = seg->elements[15].ptr[0];
    }

    if (x12_str_eq_cstr(seg->tag, "BPR"))
    {
        mapper->bpr.present = 1;
        mapper->bpr.segment = *seg;
        clear_claim_position(mapper);
        return X12_OK;
    }
    if (x12_str_eq_cstr(seg->tag, "TRN"))
    {
        return write_remittance_advice_observed(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "N1"))
    {
        return write_remittance_party_referenced(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "CLP"))
    {
        return write_claim_payment_observed(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "NM1") && seg->element_count > 0u)
    {
        if (x12_str_eq_cstr(seg->elements[0], "QC"))
        {
            return write_nm1_reference(mapper, seg, "RemittanceClaimReferencedPatient", TOK_PATIENT_ID);
        }
        if (x12_str_eq_cstr(seg->elements[0], "IL"))
        {
            return write_nm1_reference(mapper, seg, "RemittanceClaimReferencedSubscriber", TOK_MEMBER_ID);
        }
    }
    if (x12_str_eq_cstr(seg->tag, "SVC"))
    {
        return write_service_line_payment_observed(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "CAS"))
    {
        return write_adjustment_observed(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "DTM"))
    {
        return write_date_observed(mapper, seg);
    }

    return X12_OK;
}

int x12_map_835_document(x12_document_t *doc, event_writer_t *writer)
{
    x12_mapper_835_t mapper;

    if (doc == NULL || writer == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    x12_mapper_835_init(&mapper, writer);
    return x12_document_each_segment(doc, x12_mapper_835_on_segment, &mapper);
}
