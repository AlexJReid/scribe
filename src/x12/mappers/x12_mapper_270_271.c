#include "x12_mapper_270_271.h"

#include "tokenise.h"

#include <stdio.h>
#include <string.h>

#define TRY(expr)           \
    do                      \
    {                       \
        int rc__ = (expr);  \
        if (rc__ != X12_OK) \
        {                   \
            return rc__;    \
        }                   \
    } while (0)

static x12_str_t empty_str(void)
{
    x12_str_t value;

    value.ptr = "";
    value.len = 0;
    return value;
}

static x12_str_t element_or_empty(const x12_segment_t *seg, size_t index)
{
    return seg->element_count <= index ? empty_str() : seg->elements[index];
}

static int add_phi_str(event_writer_t *w, const char *name, x12_str_t value)
{
    if (!event_writer_include_phi(w))
    {
        return X12_OK;
    }
    return event_writer_add_str(w, name, value);
}

static int add_tokenized_or_phi(
    event_writer_t *w,
    const char *name,
    token_type_t type,
    x12_str_t raw)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t token_value;

    if (event_writer_include_phi(w))
    {
        TRY(event_writer_record_phi_mapping(w, type, raw));
        return event_writer_add_str(w, name, raw);
    }

    if (raw.len == 0u)
    {
        return event_writer_add_str(w, name, empty_str());
    }

    TRY(tokenise_value(type, raw, token, sizeof(token)));
    TRY(event_writer_record_phi_mapping(w, type, raw));

    token_value.ptr = token;
    token_value.len = strlen(token);
    return event_writer_add_str(w, name, token_value);
}

static int add_phi_token(
    event_writer_t *w,
    const char *name,
    token_type_t type,
    x12_str_t raw)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t token_value;

    if (!event_writer_include_phi(w))
    {
        return X12_OK;
    }
    if (raw.len == 0u)
    {
        return event_writer_add_str(w, name, empty_str());
    }

    TRY(tokenise_value(type, raw, token, sizeof(token)));
    token_value.ptr = token;
    token_value.len = strlen(token);
    return event_writer_add_str(w, name, token_value);
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

static token_type_t id_token_type_for_entity(x12_str_t entity_identifier_code)
{
    if (x12_str_eq_cstr(entity_identifier_code, "PR"))
    {
        return TOK_PAYER_ID;
    }
    if (x12_str_eq_cstr(entity_identifier_code, "1P") ||
        x12_str_eq_cstr(entity_identifier_code, "VN"))
    {
        return TOK_PROVIDER_ID;
    }
    if (x12_str_eq_cstr(entity_identifier_code, "IL") ||
        x12_str_eq_cstr(entity_identifier_code, "QC"))
    {
        return TOK_MEMBER_ID;
    }

    return TOK_UNKNOWN;
}

static const char *observed_event_type(const x12_mapper_270_271_t *mapper)
{
    return mapper->is_response ? "EligibilityResponseObserved" : "EligibilityInquiryObserved";
}

static const char *party_event_type(const x12_mapper_270_271_t *mapper)
{
    return mapper->is_response ? "EligibilityResponsePartyReferenced" : "EligibilityInquiryPartyReferenced";
}

static const char *trace_event_type(const x12_mapper_270_271_t *mapper)
{
    return mapper->is_response ? "EligibilityResponseTraceRecorded" : "EligibilityInquiryTraceRecorded";
}

static const char *date_event_type(const x12_mapper_270_271_t *mapper)
{
    return mapper->is_response ? "EligibilityResponseDateRecorded" : "EligibilityInquiryDateRecorded";
}

static const char *demographics_event_type(const x12_mapper_270_271_t *mapper)
{
    return mapper->is_response ? "EligibilityResponseDemographicsObserved" : "EligibilityInquiryDemographicsObserved";
}

static const char *current_date_scope(const x12_mapper_270_271_t *mapper)
{
    if (mapper->current_service_type_code.len > 0u)
    {
        return "benefit";
    }
    if (mapper->current_member_id.len > 0u)
    {
        return "member";
    }

    return "transaction";
}

static int add_context_ids(x12_mapper_270_271_t *mapper)
{
    event_writer_t *w = mapper->writer;

    if (mapper->current_payer_id.len > 0u)
    {
        TRY(add_tokenized_or_phi(w, "payer_id", TOK_PAYER_ID, mapper->current_payer_id));
        TRY(add_phi_token(w, "payer_id_token", TOK_PAYER_ID, mapper->current_payer_id));
    }

    if (mapper->current_member_id.len > 0u)
    {
        TRY(add_tokenized_or_phi(w, "member_id", TOK_MEMBER_ID, mapper->current_member_id));
        TRY(add_phi_token(w, "member_id_token", TOK_MEMBER_ID, mapper->current_member_id));
    }

    return X12_OK;
}

static int write_eligibility_observed(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    mapper->eligibility_id = element_or_empty(seg, 2);
    mapper->current_member_id = empty_str();
    mapper->current_payer_id = empty_str();
    mapper->current_service_type_code = empty_str();

    TRY(event_writer_begin_event(w, observed_event_type(mapper), seg));
    TRY(event_writer_add_str(w, "eligibility_id", mapper->eligibility_id));
    TRY(event_writer_add_str(w, "transaction_set_purpose_code", element_or_empty(seg, 1)));
    TRY(event_writer_add_str(w, "transaction_date", element_or_empty(seg, 3)));
    TRY(event_writer_add_str(w, "transaction_time", element_or_empty(seg, 4)));
    TRY(event_writer_add_str(w, "transaction_type_code", element_or_empty(seg, 5)));
    TRY(event_writer_add_str_array(w, "raw_elements", seg->elements, seg->element_count));
    return event_writer_end_event(w);
}

static int add_party_key_fields(
    x12_mapper_270_271_t *mapper,
    token_type_t id_token_type,
    x12_str_t id_value)
{
    event_writer_t *w = mapper->writer;
    const char *field_name = NULL;
    const char *token_field_name = NULL;

    switch (id_token_type)
    {
    case TOK_MEMBER_ID:
        field_name = "member_id";
        token_field_name = "member_id_token";
        break;
    case TOK_PROVIDER_ID:
        field_name = "provider_id";
        token_field_name = "provider_id_token";
        break;
    case TOK_PAYER_ID:
        field_name = "payer_id";
        token_field_name = "payer_id_token";
        break;
    default:
        return X12_OK;
    }

    if (id_value.len == 0u)
    {
        return X12_OK;
    }

    TRY(add_tokenized_or_phi(w, field_name, id_token_type, id_value));
    TRY(add_phi_token(w, token_field_name, id_token_type, id_value));

    return X12_OK;
}

static int write_party_referenced(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;
    x12_str_t entity_identifier_code = element_or_empty(seg, 0);
    x12_str_t id_value = element_or_empty(seg, 8);
    token_type_t id_token_type = id_token_type_for_entity(entity_identifier_code);

    if (id_token_type == TOK_PAYER_ID)
    {
        mapper->current_payer_id = id_value;
    }
    else if (id_token_type == TOK_MEMBER_ID)
    {
        mapper->current_member_id = id_value;
        mapper->current_service_type_code = empty_str();
    }

    TRY(event_writer_begin_event(w, party_event_type(mapper), seg));
    TRY(event_writer_add_str(w, "eligibility_id", mapper->eligibility_id));
    TRY(event_writer_add_str(w, "entity_identifier_code", entity_identifier_code));
    TRY(event_writer_add_str(w, "entity_type", element_or_empty(seg, 1)));
    TRY(add_phi_str(w, "last_name_or_org", element_or_empty(seg, 2)));
    TRY(add_phi_str(w, "first_name", element_or_empty(seg, 3)));
    if (id_token_type != TOK_UNKNOWN)
    {
        TRY(event_writer_record_phi_name(
            w,
            name_token_type_for_id(id_token_type),
            element_or_empty(seg, 2),
            element_or_empty(seg, 3),
            id_token_type,
            id_value));
    }
    TRY(event_writer_add_str(w, "id_qualifier", element_or_empty(seg, 7)));
    if (id_token_type != TOK_UNKNOWN)
    {
        TRY(add_tokenized_or_phi(w, "id_value", id_token_type, id_value));
        TRY(add_phi_token(w, "id_value_token", id_token_type, id_value));
    }
    else if (event_writer_include_phi(w))
    {
        TRY(event_writer_add_str(w, "id_value", id_value));
    }
    TRY(add_party_key_fields(mapper, id_token_type, id_value));
    return event_writer_end_event(w);
}

static int write_trace_recorded(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    TRY(event_writer_begin_event(w, trace_event_type(mapper), seg));
    TRY(event_writer_add_str(w, "eligibility_id", mapper->eligibility_id));
    TRY(add_context_ids(mapper));
    TRY(event_writer_add_str(w, "trace_type_code", element_or_empty(seg, 0)));
    TRY(event_writer_add_str(w, "trace_number", element_or_empty(seg, 1)));
    TRY(event_writer_add_str(w, "originating_company_id", element_or_empty(seg, 2)));
    TRY(event_writer_add_str_array(w, "raw_elements", seg->elements, seg->element_count));
    return event_writer_end_event(w);
}

static int write_date_recorded(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    TRY(event_writer_begin_event(w, date_event_type(mapper), seg));
    TRY(event_writer_add_str(w, "eligibility_id", mapper->eligibility_id));
    TRY(add_context_ids(mapper));
    TRY(event_writer_add_cstr(w, "date_scope", current_date_scope(mapper)));
    if (mapper->current_service_type_code.len > 0u)
    {
        TRY(event_writer_add_str(w, "service_type_code", mapper->current_service_type_code));
    }
    TRY(event_writer_add_str(w, "date_qualifier", element_or_empty(seg, 0)));
    TRY(event_writer_add_str(w, "date_format", element_or_empty(seg, 1)));
    TRY(event_writer_add_str(w, "date_value", element_or_empty(seg, 2)));
    return event_writer_end_event(w);
}

static int write_demographics_observed(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    TRY(event_writer_begin_event(w, demographics_event_type(mapper), seg));
    TRY(event_writer_add_str(w, "eligibility_id", mapper->eligibility_id));
    TRY(add_context_ids(mapper));
    TRY(event_writer_add_str(w, "date_format", element_or_empty(seg, 0)));
    TRY(add_tokenized_or_phi(w, "date_of_birth", TOK_MEMBER_DOB, element_or_empty(seg, 1)));
    TRY(add_phi_token(w, "date_of_birth_token", TOK_MEMBER_DOB, element_or_empty(seg, 1)));
    TRY(add_phi_str(w, "gender_code", element_or_empty(seg, 2)));
    return event_writer_end_event(w);
}

static int write_service_type_requested(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    mapper->current_service_type_code = element_or_empty(seg, 0);

    TRY(event_writer_begin_event(w, "EligibilityInquiryServiceTypeRequested", seg));
    TRY(event_writer_add_str(w, "eligibility_id", mapper->eligibility_id));
    TRY(add_context_ids(mapper));
    TRY(event_writer_add_str(w, "service_type_code", mapper->current_service_type_code));
    TRY(event_writer_add_str_array(w, "raw_elements", seg->elements, seg->element_count));
    return event_writer_end_event(w);
}

static int write_benefit_observed(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    mapper->current_service_type_code = element_or_empty(seg, 2);

    TRY(event_writer_begin_event(w, "EligibilityBenefitObserved", seg));
    TRY(event_writer_add_str(w, "eligibility_id", mapper->eligibility_id));
    TRY(add_context_ids(mapper));
    TRY(event_writer_add_str(w, "eligibility_or_benefit_information_code", element_or_empty(seg, 0)));
    TRY(event_writer_add_str(w, "coverage_level_code", element_or_empty(seg, 1)));
    TRY(event_writer_add_str(w, "service_type_code", mapper->current_service_type_code));
    TRY(event_writer_add_str(w, "insurance_type_code", element_or_empty(seg, 3)));
    TRY(event_writer_add_str(w, "plan_coverage_description", element_or_empty(seg, 4)));
    TRY(event_writer_add_str(w, "time_period_qualifier", element_or_empty(seg, 5)));
    TRY(event_writer_add_str(w, "monetary_amount", element_or_empty(seg, 6)));
    TRY(event_writer_add_str(w, "percent", element_or_empty(seg, 7)));
    TRY(event_writer_add_str(w, "quantity_qualifier", element_or_empty(seg, 8)));
    TRY(event_writer_add_str(w, "quantity", element_or_empty(seg, 9)));
    TRY(event_writer_add_str(w, "authorization_or_certification_indicator", element_or_empty(seg, 10)));
    TRY(event_writer_add_str(w, "in_plan_network_indicator", element_or_empty(seg, 11)));
    TRY(event_writer_add_str_array(w, "raw_elements", seg->elements, seg->element_count));
    return event_writer_end_event(w);
}

static void x12_mapper_270_271_init(
    x12_mapper_270_271_t *mapper,
    event_writer_t *writer,
    const char *transaction_type,
    int is_response)
{
    if (mapper == NULL)
    {
        return;
    }

    memset(mapper, 0, sizeof(*mapper));
    mapper->writer = writer;
    mapper->transaction_type = transaction_type;
    mapper->is_response = is_response;
    mapper->eligibility_id = empty_str();
    mapper->current_member_id = empty_str();
    mapper->current_payer_id = empty_str();
    mapper->current_service_type_code = empty_str();
    mapper->component_sep = ':';
}

void x12_mapper_270_init(x12_mapper_270_271_t *mapper, event_writer_t *writer)
{
    x12_mapper_270_271_init(mapper, writer, "270", 0);
}

void x12_mapper_271_init(x12_mapper_270_271_t *mapper, event_writer_t *writer)
{
    x12_mapper_270_271_init(mapper, writer, "271", 1);
}

int x12_mapper_270_271_on_segment(const x12_segment_t *seg, void *user)
{
    x12_mapper_270_271_t *mapper = (x12_mapper_270_271_t *)user;

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

    if (x12_str_eq_cstr(seg->tag, "BHT"))
    {
        return write_eligibility_observed(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "HL"))
    {
        mapper->current_service_type_code = empty_str();
        return X12_OK;
    }
    if (x12_str_eq_cstr(seg->tag, "NM1"))
    {
        return write_party_referenced(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "TRN"))
    {
        return write_trace_recorded(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "DTP"))
    {
        return write_date_recorded(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "DMG"))
    {
        return write_demographics_observed(mapper, seg);
    }
    if (!mapper->is_response && x12_str_eq_cstr(seg->tag, "EQ"))
    {
        return write_service_type_requested(mapper, seg);
    }
    if (mapper->is_response && x12_str_eq_cstr(seg->tag, "EB"))
    {
        return write_benefit_observed(mapper, seg);
    }

    (void)mapper->transaction_type;
    return X12_OK;
}

int x12_map_270_document(x12_document_t *doc, event_writer_t *writer)
{
    x12_mapper_270_271_t mapper;

    if (doc == NULL || writer == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    x12_mapper_270_init(&mapper, writer);
    return x12_document_each_segment(doc, x12_mapper_270_271_on_segment, &mapper);
}

int x12_map_271_document(x12_document_t *doc, event_writer_t *writer)
{
    x12_mapper_270_271_t mapper;

    if (doc == NULL || writer == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    x12_mapper_271_init(&mapper, writer);
    return x12_document_each_segment(doc, x12_mapper_270_271_on_segment, &mapper);
}
