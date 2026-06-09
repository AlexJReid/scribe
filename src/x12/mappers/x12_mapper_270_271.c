#include "x12_mapper_270_271.h"

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

static int write_payload_start(FILE *fp)
{
    return fputc('{', fp) == EOF ? X12_ERR_IO : X12_OK;
}

static int write_payload_end(FILE *fp)
{
    return fputc('}', fp) == EOF ? X12_ERR_IO : X12_OK;
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

static token_type_t id_token_type_for_entity(x12_str_t entity_identifier_code)
{
    if (x12_str_eq_cstr(entity_identifier_code, "PR")) {
        return TOK_PAYER_ID;
    }
    if (x12_str_eq_cstr(entity_identifier_code, "1P") ||
        x12_str_eq_cstr(entity_identifier_code, "VN")) {
        return TOK_PROVIDER_ID;
    }
    if (x12_str_eq_cstr(entity_identifier_code, "IL") ||
        x12_str_eq_cstr(entity_identifier_code, "QC")) {
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
    return mapper->is_response ?
        "EligibilityResponseDemographicsObserved" :
        "EligibilityInquiryDemographicsObserved";
}

static const char *current_date_scope(const x12_mapper_270_271_t *mapper)
{
    if (mapper->current_service_type_code.len > 0u) {
        return "benefit";
    }
    if (mapper->current_member_id.len > 0u) {
        return "member";
    }

    return "transaction";
}

static int write_context_ids(x12_mapper_270_271_t *mapper, FILE *fp)
{
    if (mapper->current_payer_id.len > 0u) {
        if (write_tokenized_or_phi_field(
                mapper->writer,
                fp,
                "payer_id",
                TOK_PAYER_ID,
                mapper->current_payer_id,
                1,
                event_writer_include_phi(mapper->writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        if (write_phi_token_field(
                fp,
                "payer_id_token",
                TOK_PAYER_ID,
                mapper->current_payer_id,
                1,
                event_writer_include_phi(mapper->writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
    }

    if (mapper->current_member_id.len > 0u) {
        if (write_tokenized_or_phi_field(
                mapper->writer,
                fp,
                "member_id",
                TOK_MEMBER_ID,
                mapper->current_member_id,
                1,
                event_writer_include_phi(mapper->writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        if (write_phi_token_field(
                fp,
                "member_id_token",
                TOK_MEMBER_ID,
                mapper->current_member_id,
                1,
                event_writer_include_phi(mapper->writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
    }

    return X12_OK;
}

static int write_eligibility_observed(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    mapper->eligibility_id = element_or_empty(seg, 2);
    mapper->current_member_id = empty_str();
    mapper->current_payer_id = empty_str();
    mapper->current_service_type_code = empty_str();

    rc = event_writer_begin_event(mapper->writer, observed_event_type(mapper), seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "eligibility_id", mapper->eligibility_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "transaction_set_purpose_code", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "transaction_date", element_or_empty(seg, 3), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "transaction_time", element_or_empty(seg, 4), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "transaction_type_code", element_or_empty(seg, 5), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(fp, "raw_elements", seg->elements, seg->element_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_party_id_alias(
    x12_mapper_270_271_t *mapper,
    FILE *fp,
    token_type_t id_token_type,
    x12_str_t id_value
)
{
    const char *field_name = NULL;
    const char *token_field_name = NULL;

    switch (id_token_type) {
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

    if (id_value.len == 0u) {
        return X12_OK;
    }

    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            field_name,
            id_token_type,
            id_value,
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            token_field_name,
            id_token_type,
            id_value,
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }

    return X12_OK;
}

static int write_party_referenced(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    x12_str_t entity_identifier_code = element_or_empty(seg, 0);
    x12_str_t id_value = element_or_empty(seg, 8);
    token_type_t id_token_type = id_token_type_for_entity(entity_identifier_code);
    int rc;

    if (id_token_type == TOK_PAYER_ID) {
        mapper->current_payer_id = id_value;
    } else if (id_token_type == TOK_MEMBER_ID) {
        mapper->current_member_id = id_value;
        mapper->current_service_type_code = empty_str();
    }

    rc = event_writer_begin_event(mapper->writer, party_event_type(mapper), seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "eligibility_id", mapper->eligibility_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "entity_identifier_code", entity_identifier_code, 1) != X12_OK) {
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
    if (id_token_type != TOK_UNKNOWN &&
        event_writer_record_phi_name(
            mapper->writer,
            name_token_type_for_id(id_token_type),
            element_or_empty(seg, 2),
            element_or_empty(seg, 3),
            id_token_type,
            id_value
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "id_qualifier", element_or_empty(seg, 7), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (id_token_type != TOK_UNKNOWN) {
        if (write_tokenized_or_phi_field(
                mapper->writer,
                fp,
                "id_value",
                id_token_type,
                id_value,
                1,
                event_writer_include_phi(mapper->writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        if (write_phi_token_field(
                fp,
                "id_value_token",
                id_token_type,
                id_value,
                1,
                event_writer_include_phi(mapper->writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
    } else if (event_writer_include_phi(mapper->writer) &&
               event_writer_write_string_field(fp, "id_value", id_value, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_party_id_alias(mapper, fp, id_token_type, id_value) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_trace_recorded(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, trace_event_type(mapper), seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "eligibility_id", mapper->eligibility_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_context_ids(mapper, fp) != X12_OK) {
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
    if (event_writer_write_str_array_field(fp, "raw_elements", seg->elements, seg->element_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_date_recorded(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, date_event_type(mapper), seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "eligibility_id", mapper->eligibility_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_context_ids(mapper, fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "date_scope", current_date_scope(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (mapper->current_service_type_code.len > 0u &&
        event_writer_write_string_field(fp, "service_type_code", mapper->current_service_type_code, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "date_qualifier", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "date_format", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "date_value", element_or_empty(seg, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_demographics_observed(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, demographics_event_type(mapper), seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "eligibility_id", mapper->eligibility_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_context_ids(mapper, fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "date_format", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            "date_of_birth",
            TOK_MEMBER_DOB,
            element_or_empty(seg, 1),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            "date_of_birth_token",
            TOK_MEMBER_DOB,
            element_or_empty(seg, 1),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_string_field(
            fp,
            "gender_code",
            element_or_empty(seg, 2),
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

static int write_service_type_requested(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    mapper->current_service_type_code = element_or_empty(seg, 0);

    rc = event_writer_begin_event(mapper->writer, "EligibilityInquiryServiceTypeRequested", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "eligibility_id", mapper->eligibility_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_context_ids(mapper, fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_type_code", mapper->current_service_type_code, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(fp, "raw_elements", seg->elements, seg->element_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_benefit_observed(
    x12_mapper_270_271_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    mapper->current_service_type_code = element_or_empty(seg, 2);

    rc = event_writer_begin_event(mapper->writer, "EligibilityBenefitObserved", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "eligibility_id", mapper->eligibility_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_context_ids(mapper, fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "eligibility_or_benefit_information_code", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "coverage_level_code", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_type_code", mapper->current_service_type_code, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "insurance_type_code", element_or_empty(seg, 3), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "plan_coverage_description", element_or_empty(seg, 4), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "time_period_qualifier", element_or_empty(seg, 5), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "monetary_amount", element_or_empty(seg, 6), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "percent", element_or_empty(seg, 7), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "quantity_qualifier", element_or_empty(seg, 8), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "quantity", element_or_empty(seg, 9), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "authorization_or_certification_indicator", element_or_empty(seg, 10), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "in_plan_network_indicator", element_or_empty(seg, 11), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(fp, "raw_elements", seg->elements, seg->element_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static void x12_mapper_270_271_init(
    x12_mapper_270_271_t *mapper,
    event_writer_t *writer,
    const char *transaction_type,
    int is_response
)
{
    if (mapper == NULL) {
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

    if (seg == NULL || mapper == NULL || mapper->writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    event_writer_observe_control(mapper->writer, seg);

    if (x12_str_eq_cstr(seg->tag, "ISA") &&
        seg->element_count > 15u &&
        seg->elements[15].len > 0u) {
        mapper->component_sep = seg->elements[15].ptr[0];
    }

    if (x12_str_eq_cstr(seg->tag, "BHT")) {
        return write_eligibility_observed(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "HL")) {
        mapper->current_service_type_code = empty_str();
        return X12_OK;
    }

    if (x12_str_eq_cstr(seg->tag, "NM1")) {
        return write_party_referenced(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "TRN")) {
        return write_trace_recorded(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "DTP")) {
        return write_date_recorded(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "DMG")) {
        return write_demographics_observed(mapper, seg);
    }

    if (!mapper->is_response && x12_str_eq_cstr(seg->tag, "EQ")) {
        return write_service_type_requested(mapper, seg);
    }

    if (mapper->is_response && x12_str_eq_cstr(seg->tag, "EB")) {
        return write_benefit_observed(mapper, seg);
    }

    (void)mapper->transaction_type;
    return X12_OK;
}

int x12_map_270_document(x12_document_t *doc, event_writer_t *writer)
{
    x12_mapper_270_271_t mapper;

    if (doc == NULL || writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    x12_mapper_270_init(&mapper, writer);
    return x12_document_each_segment(doc, x12_mapper_270_271_on_segment, &mapper);
}

int x12_map_271_document(x12_document_t *doc, event_writer_t *writer)
{
    x12_mapper_270_271_t mapper;

    if (doc == NULL || writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    x12_mapper_271_init(&mapper, writer);
    return x12_document_each_segment(doc, x12_mapper_270_271_on_segment, &mapper);
}
