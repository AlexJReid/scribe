#include "x12_mapper_837.h"

#include "tokenise.h"

#include <stdio.h>
#include <string.h>

#define X12_837_PARTY_NONE 0
#define X12_837_PARTY_SUBSCRIBER 1
#define X12_837_PARTY_PATIENT 2

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

static int str_contains_char(x12_str_t value, char needle)
{
    size_t i;

    for (i = 0; i < value.len; i++) {
        if (value.ptr[i] == needle) {
            return 1;
        }
    }

    return 0;
}

static size_t split_components(
    x12_str_t value,
    char component_sep,
    x12_str_t *components,
    size_t component_cap
)
{
    size_t start = 0u;
    size_t count = 0u;
    size_t i;

    if (components == NULL || component_cap == 0u || value.len == 0u) {
        return 0u;
    }

    for (i = 0u; i <= value.len; i++) {
        if (i == value.len || value.ptr[i] == component_sep) {
            if (count < component_cap) {
                components[count].ptr = value.ptr + start;
                components[count].len = i - start;
            }
            count++;
            start = i + 1u;
        }
    }

    return count < component_cap ? count : component_cap;
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

static void split_procedure_components(
    x12_str_t value,
    char component_sep,
    x12_str_t *qualifier,
    x12_str_t *code,
    x12_str_t *modifiers,
    size_t *modifier_count
)
{
    size_t start = 0u;
    size_t index = 0u;
    size_t i;

    *qualifier = empty_str();
    *code = empty_str();
    *modifier_count = 0u;

    for (i = 0u; i <= value.len; i++) {
        if (i == value.len || value.ptr[i] == component_sep) {
            x12_str_t component;

            component.ptr = value.ptr + start;
            component.len = i - start;

            if (index == 0u) {
                *qualifier = component;
            } else if (index == 1u) {
                *code = component;
            } else if (index <= 5u && component.len > 0u && *modifier_count < 4u) {
                modifiers[*modifier_count] = component;
                (*modifier_count)++;
            }

            start = i + 1u;
            index++;
        }
    }

    if (index == 1u) {
        *code = *qualifier;
        *qualifier = empty_str();
    }
}

static int normalize_diagnosis_code(
    x12_str_t raw_code,
    char *buffer,
    size_t buffer_len,
    x12_str_t *out
)
{
    if (buffer == NULL || out == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (raw_code.len > 3u && !str_contains_char(raw_code, '.')) {
        if (raw_code.len + 1u > buffer_len) {
            return X12_ERR_BUFFER_TOO_SMALL;
        }

        memcpy(buffer, raw_code.ptr, 3u);
        buffer[3] = '.';
        memcpy(buffer + 4u, raw_code.ptr + 3u, raw_code.len - 3u);
        out->ptr = buffer;
        out->len = raw_code.len + 1u;
        return X12_OK;
    }

    *out = raw_code;
    return X12_OK;
}

static int diagnosis_is_principal(x12_str_t qualifier)
{
    return x12_str_eq_cstr(qualifier, "ABK") ||
           x12_str_eq_cstr(qualifier, "BK");
}

static int diagnosis_is_other(x12_str_t qualifier)
{
    return x12_str_eq_cstr(qualifier, "ABF") ||
           x12_str_eq_cstr(qualifier, "BF");
}

static int healthcare_code_is_diagnosis(x12_str_t qualifier)
{
    return diagnosis_is_principal(qualifier) || diagnosis_is_other(qualifier);
}

static const char *procedure_code_set(x12_str_t qualifier)
{
    if (x12_str_eq_cstr(qualifier, "HC")) {
        return "CPT/HCPCS";
    }

    return "";
}

static const char *healthcare_code_kind(x12_str_t qualifier)
{
    if (diagnosis_is_principal(qualifier)) {
        return "principal_diagnosis";
    }
    if (diagnosis_is_other(qualifier)) {
        return "other_diagnosis";
    }
    if (x12_str_eq_cstr(qualifier, "BG")) {
        return "condition_code";
    }
    if (x12_str_eq_cstr(qualifier, "BH")) {
        return "occurrence_code";
    }
    if (x12_str_eq_cstr(qualifier, "BE")) {
        return "value_code";
    }
    if (x12_str_eq_cstr(qualifier, "BBR")) {
        return "institutional_procedure";
    }
    if (x12_str_eq_cstr(qualifier, "DR")) {
        return "diagnosis_related_group";
    }

    return "other";
}

static const char *provider_context_from_nm1(x12_str_t entity_identifier_code)
{
    if (x12_str_eq_cstr(entity_identifier_code, "85")) {
        return "billing_provider";
    }
    if (x12_str_eq_cstr(entity_identifier_code, "82")) {
        return "rendering_provider";
    }
    if (x12_str_eq_cstr(entity_identifier_code, "DN")) {
        return "referring_provider";
    }
    if (x12_str_eq_cstr(entity_identifier_code, "DQ")) {
        return "supervising_provider";
    }
    if (x12_str_eq_cstr(entity_identifier_code, "77")) {
        return "facility";
    }
    if (x12_str_eq_cstr(entity_identifier_code, "71")) {
        return "attending_provider";
    }
    if (x12_str_eq_cstr(entity_identifier_code, "72")) {
        return "operating_provider";
    }
    if (x12_str_eq_cstr(entity_identifier_code, "ZZ")) {
        return "other_provider";
    }

    return "";
}

static const char *provider_context_from_prv(x12_str_t provider_code)
{
    if (x12_str_eq_cstr(provider_code, "BI")) {
        return "billing_provider";
    }
    if (x12_str_eq_cstr(provider_code, "PE")) {
        return "rendering_provider";
    }
    if (x12_str_eq_cstr(provider_code, "RF")) {
        return "referring_provider";
    }
    if (x12_str_eq_cstr(provider_code, "SU")) {
        return "supervising_provider";
    }
    if (x12_str_eq_cstr(provider_code, "AT")) {
        return "attending_provider";
    }
    if (x12_str_eq_cstr(provider_code, "OP")) {
        return "operating_provider";
    }
    if (x12_str_eq_cstr(provider_code, "FA")) {
        return "facility";
    }
    if (x12_str_eq_cstr(provider_code, "OT")) {
        return "other_provider";
    }

    return "";
}

static const char *provider_context_for_prv(
    const x12_mapper_837_t *mapper,
    x12_str_t provider_code
)
{
    if (mapper != NULL &&
        mapper->current_provider_context != NULL &&
        mapper->current_provider_context[0] != '\0') {
        return mapper->current_provider_context;
    }

    return provider_context_from_prv(provider_code);
}

static const char *current_loop_scope(const x12_mapper_837_t *mapper)
{
    if (mapper->current_claim_id.len == 0u) {
        return "transaction";
    }
    if (mapper->in_service_line) {
        return "service_line";
    }

    return "claim";
}

static const char *party_scope_name(int party_scope)
{
    if (party_scope == X12_837_PARTY_PATIENT) {
        return "patient";
    }
    if (party_scope == X12_837_PARTY_SUBSCRIBER) {
        return "subscriber";
    }

    return "unknown";
}

static void clear_claim_position(x12_mapper_837_t *mapper)
{
    mapper->current_claim_id = empty_str();
    mapper->current_service_line_number = empty_str();
    mapper->in_service_line = 0;
    mapper->rendering_provider.present = 0;
    mapper->rendering_provider_taxonomy.present = 0;
    mapper->current_provider_context = "";
}

static void buffer_segment(
    x12_mapper_837_buffered_segment_t *buffer,
    const x12_segment_t *seg
)
{
    buffer->present = 1;
    buffer->segment = *seg;
}

static int flush_claim_context_references(x12_mapper_837_t *mapper);
static int flush_claim_party_context(x12_mapper_837_t *mapper);
static int flush_claim_context_taxonomies(x12_mapper_837_t *mapper);

static const char *current_date_event_type(const x12_mapper_837_t *mapper)
{
    if (mapper->in_service_line) {
        return "ClaimLineDateRecorded";
    }

    return "ClaimDateRecorded";
}

static int write_payload_start(FILE *fp)
{
    return fputc('{', fp) == EOF ? X12_ERR_IO : X12_OK;
}

static int write_payload_end(FILE *fp)
{
    return fputc('}', fp) == EOF ? X12_ERR_IO : X12_OK;
}

static int write_current_claim_fields(
    x12_mapper_837_t *mapper,
    FILE *fp,
    int prefix_comma
)
{
    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            "claim_id",
            TOK_CLAIM_ID,
            mapper->current_claim_id,
            prefix_comma,
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

    return X12_OK;
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

static int write_claim_observed(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    x12_str_t claim_type_components[3];
    size_t claim_type_component_count;
    int rc;

    mapper->current_claim_id = element_or_empty(seg, 0);
    mapper->current_service_line_number = empty_str();
    mapper->in_service_line = 0;
    claim_type_component_count = split_components(
        element_or_empty(seg, 4),
        mapper->component_sep,
        claim_type_components,
        sizeof(claim_type_components) / sizeof(claim_type_components[0])
    );

    rc = event_writer_begin_event(mapper->writer, "ClaimObserved", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
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
    if (event_writer_write_string_field(fp, "total_charge_amount", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(
            fp,
            "facility_type_code",
            claim_type_component_count > 0u ? claim_type_components[0] : empty_str(),
            1
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(
            fp,
            "facility_code_qualifier",
            claim_type_component_count > 1u ? claim_type_components[1] : empty_str(),
            1
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(
            fp,
            "claim_frequency_type_code",
            claim_type_component_count > 2u ? claim_type_components[2] : empty_str(),
            1
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "provider_signature_indicator", element_or_empty(seg, 5), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "assignment_or_plan_participation_code", element_or_empty(seg, 6), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "benefits_assignment_certification_indicator", element_or_empty(seg, 7), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "release_of_information_code", element_or_empty(seg, 8), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "patient_signature_source_code", element_or_empty(seg, 9), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    rc = event_writer_end_event(mapper->writer);
    if (rc != X12_OK) {
        return rc;
    }
    rc = flush_claim_party_context(mapper);
    if (rc != X12_OK) {
        return rc;
    }
    rc = flush_claim_context_references(mapper);
    if (rc != X12_OK) {
        return rc;
    }
    rc = flush_claim_context_taxonomies(mapper);
    mapper->current_provider_context = "";
    return rc;
}

static int write_nm1_reference(
    x12_mapper_837_t *mapper,
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
    if (event_writer_write_cstring_field(fp, "reference_scope", current_loop_scope(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_line_number", mapper->current_service_line_number, 1) != X12_OK) {
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

static int flush_buffered_nm1_reference(
    x12_mapper_837_t *mapper,
    x12_mapper_837_buffered_segment_t *buffer,
    const char *event_type,
    token_type_t id_token_type,
    int clear_after_flush
)
{
    int rc;

    if (!buffer->present) {
        return X12_OK;
    }

    rc = write_nm1_reference(mapper, &buffer->segment, event_type, id_token_type);
    if (rc != X12_OK) {
        return rc;
    }

    if (clear_after_flush) {
        buffer->present = 0;
    }

    return X12_OK;
}

static int flush_claim_context_references(x12_mapper_837_t *mapper)
{
    int rc;

    rc = flush_buffered_nm1_reference(
        mapper,
        &mapper->billing_provider,
        "ClaimReferencedBillingProvider",
        TOK_PROVIDER_ID,
        0
    );
    if (rc != X12_OK) {
        return rc;
    }

    rc = flush_buffered_nm1_reference(
        mapper,
        &mapper->subscriber,
        "ClaimReferencedSubscriber",
        TOK_MEMBER_ID,
        0
    );
    if (rc != X12_OK) {
        return rc;
    }

    rc = flush_buffered_nm1_reference(
        mapper,
        &mapper->patient,
        "ClaimReferencedPatient",
        TOK_PATIENT_ID,
        0
    );
    if (rc != X12_OK) {
        return rc;
    }

    return flush_buffered_nm1_reference(
        mapper,
        &mapper->rendering_provider,
        "ClaimReferencedRenderingProvider",
        TOK_PROVIDER_ID,
        1
    );
}

static int write_provider_taxonomy_recorded(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg,
    const char *provider_context
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    if (mapper->current_claim_id.len == 0u) {
        return X12_OK;
    }
    if (provider_context == NULL || provider_context[0] == '\0') {
        provider_context = provider_context_from_prv(element_or_empty(seg, 0));
    }

    rc = event_writer_begin_event(mapper->writer, "ClaimProviderTaxonomyRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_current_claim_fields(mapper, fp, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "reference_scope", current_loop_scope(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_line_number", mapper->current_service_line_number, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "provider_context", provider_context, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "provider_role_code", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "reference_identification_qualifier", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "provider_taxonomy_code", element_or_empty(seg, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int flush_buffered_provider_taxonomy(
    x12_mapper_837_t *mapper,
    x12_mapper_837_buffered_segment_t *buffer,
    const char *provider_context,
    int clear_after_flush
)
{
    int rc;

    if (!buffer->present) {
        return X12_OK;
    }

    rc = write_provider_taxonomy_recorded(mapper, &buffer->segment, provider_context);
    if (rc != X12_OK) {
        return rc;
    }

    if (clear_after_flush) {
        buffer->present = 0;
    }

    return X12_OK;
}

static int flush_claim_context_taxonomies(x12_mapper_837_t *mapper)
{
    int rc;

    rc = flush_buffered_provider_taxonomy(
        mapper,
        &mapper->billing_provider_taxonomy,
        "billing_provider",
        0
    );
    if (rc != X12_OK) {
        return rc;
    }

    return flush_buffered_provider_taxonomy(
        mapper,
        &mapper->rendering_provider_taxonomy,
        "rendering_provider",
        1
    );
}

static int write_claim_scoped_nm1_reference(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg,
    const char *event_type,
    token_type_t id_token_type
)
{
    if (mapper->current_claim_id.len == 0u) {
        return X12_OK;
    }

    return write_nm1_reference(mapper, seg, event_type, id_token_type);
}

static int write_subscriber_information(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    if (mapper->current_claim_id.len == 0u) {
        return X12_OK;
    }

    rc = event_writer_begin_event(mapper->writer, "ClaimSubscriberInformationRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_current_claim_fields(mapper, fp, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "party_scope", "subscriber", 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "payer_responsibility_sequence_number_code", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "individual_relationship_code", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            "insured_group_or_policy_number",
            TOK_REFERENCE_ID,
            element_or_empty(seg, 2),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            "insured_group_or_policy_number_token",
            TOK_REFERENCE_ID,
            element_or_empty(seg, 2),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "claim_filing_indicator_code", element_or_empty(seg, 8), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_patient_information(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    if (mapper->current_claim_id.len == 0u) {
        return X12_OK;
    }

    rc = event_writer_begin_event(mapper->writer, "ClaimPatientInformationRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_current_claim_fields(mapper, fp, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "party_scope", "patient", 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "individual_relationship_code", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_demographics_recorded(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg,
    int party_scope
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    if (mapper->current_claim_id.len == 0u) {
        return X12_OK;
    }

    rc = event_writer_begin_event(mapper->writer, "ClaimDemographicsRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_current_claim_fields(mapper, fp, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "party_scope", party_scope_name(party_scope), 1) != X12_OK) {
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
    if (event_writer_write_string_field(fp, "gender_code", element_or_empty(seg, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int flush_buffered_claim_context(
    x12_mapper_837_t *mapper,
    x12_mapper_837_buffered_segment_t *buffer,
    int (*write_fn)(x12_mapper_837_t *, const x12_segment_t *)
)
{
    if (!buffer->present) {
        return X12_OK;
    }

    return write_fn(mapper, &buffer->segment);
}

static int flush_buffered_demographics(
    x12_mapper_837_t *mapper,
    x12_mapper_837_buffered_segment_t *buffer,
    int party_scope
)
{
    if (!buffer->present) {
        return X12_OK;
    }

    return write_demographics_recorded(mapper, &buffer->segment, party_scope);
}

static int flush_claim_party_context(x12_mapper_837_t *mapper)
{
    int rc;

    rc = flush_buffered_claim_context(mapper, &mapper->subscriber_info, write_subscriber_information);
    if (rc != X12_OK) {
        return rc;
    }
    rc = flush_buffered_demographics(
        mapper,
        &mapper->subscriber_demographics,
        X12_837_PARTY_SUBSCRIBER
    );
    if (rc != X12_OK) {
        return rc;
    }
    rc = flush_buffered_claim_context(mapper, &mapper->patient_info, write_patient_information);
    if (rc != X12_OK) {
        return rc;
    }
    return flush_buffered_demographics(
        mapper,
        &mapper->patient_demographics,
        X12_837_PARTY_PATIENT
    );
}

static int write_claim_reference_recorded(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    if (mapper->current_claim_id.len == 0u) {
        return X12_OK;
    }

    rc = event_writer_begin_event(mapper->writer, "ClaimReferenceRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_current_claim_fields(mapper, fp, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "reference_scope", current_loop_scope(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_line_number", mapper->current_service_line_number, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "reference_qualifier", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_tokenized_or_phi_field(
            mapper->writer,
            fp,
            "reference_identification",
            TOK_REFERENCE_ID,
            element_or_empty(seg, 1),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            "reference_identification_token",
            TOK_REFERENCE_ID,
            element_or_empty(seg, 1),
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

static int write_institutional_information_recorded(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    if (mapper->current_claim_id.len == 0u) {
        return X12_OK;
    }

    rc = event_writer_begin_event(mapper->writer, "ClaimInstitutionalInformationRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_current_claim_fields(mapper, fp, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "admission_type_code", element_or_empty(seg, 0), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "admission_source_code", element_or_empty(seg, 1), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "patient_status_code", element_or_empty(seg, 2), 1) != X12_OK) {
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

static int write_date_observed(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, current_date_event_type(mapper), seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
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
    if (event_writer_write_cstring_field(fp, "date_scope", current_loop_scope(mapper), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_line_number", mapper->current_service_line_number, 1) != X12_OK) {
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

static int write_diagnosis_observed(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    x12_str_t principal_diagnosis = empty_str();
    x12_str_t other_diagnoses[128];
    char principal_buffer[64];
    char other_buffers[128][64];
    size_t other_diagnosis_count = 0;
    size_t i;
    int rc;

    for (i = 0; i < seg->element_count; i++) {
        x12_str_t qualifier;
        x12_str_t raw_code;
        x12_str_t normalized_code;

        split_first_component(seg->elements[i], mapper->component_sep, &qualifier, &raw_code);
        if (raw_code.len == 0u) {
            continue;
        }

        if (diagnosis_is_principal(qualifier)) {
            rc = normalize_diagnosis_code(
                raw_code,
                principal_buffer,
                sizeof(principal_buffer),
                &principal_diagnosis
            );
            if (rc != X12_OK) {
                return rc;
            }
        } else if (diagnosis_is_other(qualifier) &&
                   other_diagnosis_count < (sizeof(other_diagnoses) / sizeof(other_diagnoses[0]))) {
            rc = normalize_diagnosis_code(
                raw_code,
                other_buffers[other_diagnosis_count],
                sizeof(other_buffers[other_diagnosis_count]),
                &normalized_code
            );
            if (rc != X12_OK) {
                return rc;
            }
            other_diagnoses[other_diagnosis_count] = normalized_code;
            other_diagnosis_count++;
        }
    }

    rc = event_writer_begin_event(mapper->writer, "ClaimDiagnosesRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
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
    if (event_writer_write_string_field(fp, "principal_diagnosis_code", principal_diagnosis, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(fp, "other_diagnosis_codes", other_diagnoses, other_diagnosis_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(fp, "raw_diagnosis_elements", seg->elements, seg->element_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_healthcare_code_recorded(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg,
    x12_str_t raw_component
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    x12_str_t components[8];
    x12_str_t code = empty_str();
    char normalized_buffer[64];
    size_t component_count;
    int rc;

    component_count = split_components(
        raw_component,
        mapper->component_sep,
        components,
        sizeof(components) / sizeof(components[0])
    );
    if (component_count == 0u || components[0].len == 0u) {
        return X12_OK;
    }

    if (component_count > 1u) {
        code = components[1];
        if (healthcare_code_is_diagnosis(components[0])) {
            rc = normalize_diagnosis_code(code, normalized_buffer, sizeof(normalized_buffer), &code);
            if (rc != X12_OK) {
                return rc;
            }
        }
    }

    rc = event_writer_begin_event(mapper->writer, "ClaimHealthcareCodeRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_current_claim_fields(mapper, fp, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "healthcare_code_kind", healthcare_code_kind(components[0]), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "healthcare_code_qualifier", components[0], 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "healthcare_code", code, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(
            fp,
            "healthcare_code_date_format",
            component_count > 2u ? components[2] : empty_str(),
            1
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(
            fp,
            "healthcare_code_date_value",
            component_count > 3u ? components[3] : empty_str(),
            1
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(
            fp,
            "healthcare_code_amount",
            component_count > 4u ? components[4] : empty_str(),
            1
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(
            fp,
            "healthcare_code_components",
            components,
            component_count,
            1
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_end_event(mapper->writer);
}

static int write_healthcare_codes_recorded(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    size_t i;
    int rc;

    for (i = 0u; i < seg->element_count; i++) {
        rc = write_healthcare_code_recorded(mapper, seg, seg->elements[i]);
        if (rc != X12_OK) {
            return rc;
        }
    }

    return X12_OK;
}

static int write_service_line_observed(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    x12_str_t revenue_code = empty_str();
    x12_str_t procedure_element = element_or_empty(seg, 0);
    x12_str_t procedure_qualifier;
    x12_str_t procedure_code;
    x12_str_t procedure_modifiers[4];
    size_t procedure_modifier_count;
    x12_str_t charge_amount = element_or_empty(seg, 1);
    x12_str_t unit_measure_code = element_or_empty(seg, 2);
    x12_str_t unit_count = element_or_empty(seg, 3);
    x12_str_t diagnosis_pointers = element_or_empty(seg, 6);
    x12_str_t diagnosis_pointer_values[4];
    size_t diagnosis_pointer_count = 0u;
    int rc;

    if (x12_str_eq_cstr(seg->tag, "SV2")) {
        revenue_code = element_or_empty(seg, 0);
        procedure_element = element_or_empty(seg, 1);
        charge_amount = element_or_empty(seg, 2);
        unit_measure_code = element_or_empty(seg, 3);
        unit_count = element_or_empty(seg, 4);
        diagnosis_pointers = empty_str();
    }

    if (diagnosis_pointers.len > 0u) {
        diagnosis_pointer_count = split_components(
            diagnosis_pointers,
            mapper->component_sep,
            diagnosis_pointer_values,
            sizeof(diagnosis_pointer_values) / sizeof(diagnosis_pointer_values[0])
        );
    }

    split_procedure_components(
        procedure_element,
        mapper->component_sep,
        &procedure_qualifier,
        &procedure_code,
        procedure_modifiers,
        &procedure_modifier_count
    );

    rc = event_writer_begin_event(mapper->writer, "ClaimServiceLineRecorded", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
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
    if (event_writer_write_string_field(fp, "line_type", seg->tag, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "service_line_number", mapper->current_service_line_number, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "revenue_code", revenue_code, 1) != X12_OK) {
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
    if (event_writer_write_str_array_field(
            fp,
            "procedure_modifiers",
            procedure_modifiers,
            procedure_modifier_count,
            1
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "charge_amount", charge_amount, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "unit_measure_code", unit_measure_code, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "unit_count", unit_count, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_str_array_field(
            fp,
            "diagnosis_pointers",
            diagnosis_pointer_values,
            diagnosis_pointer_count,
            1
        ) != X12_OK) {
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

void x12_mapper_837_init(x12_mapper_837_t *mapper, event_writer_t *writer)
{
    if (mapper == NULL) {
        return;
    }

    memset(mapper, 0, sizeof(*mapper));

    mapper->writer = writer;
    mapper->current_claim_id = empty_str();
    mapper->current_service_line_number = empty_str();
    mapper->in_service_line = 0;
    mapper->current_party = X12_837_PARTY_NONE;
    mapper->current_provider_context = "";
    mapper->component_sep = ':';
}

int x12_mapper_837_on_segment(const x12_segment_t *seg, void *user)
{
    x12_mapper_837_t *mapper = (x12_mapper_837_t *)user;

    if (seg == NULL || mapper == NULL || mapper->writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    event_writer_observe_control(mapper->writer, seg);

    if (x12_str_eq_cstr(seg->tag, "ISA") &&
        seg->element_count > 15u &&
        seg->elements[15].len > 0u) {
        mapper->component_sep = seg->elements[15].ptr[0];
    }

    if (x12_str_eq_cstr(seg->tag, "CLM")) {
        return write_claim_observed(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "LX")) {
        mapper->current_service_line_number = element_or_empty(seg, 0);
        mapper->in_service_line = 1;
        return X12_OK;
    }

    if (x12_str_eq_cstr(seg->tag, "NM1") && seg->element_count > 0u) {
        if (x12_str_eq_cstr(seg->elements[0], "IL")) {
            clear_claim_position(mapper);
            buffer_segment(&mapper->subscriber, seg);
            mapper->patient.present = 0;
            mapper->patient_info.present = 0;
            mapper->patient_demographics.present = 0;
            mapper->current_party = X12_837_PARTY_SUBSCRIBER;
            mapper->current_provider_context = "";
            return X12_OK;
        }
        if (x12_str_eq_cstr(seg->elements[0], "QC")) {
            clear_claim_position(mapper);
            buffer_segment(&mapper->patient, seg);
            mapper->current_party = X12_837_PARTY_PATIENT;
            mapper->current_provider_context = "";
            return X12_OK;
        }
        if (x12_str_eq_cstr(seg->elements[0], "85")) {
            clear_claim_position(mapper);
            buffer_segment(&mapper->billing_provider, seg);
            mapper->subscriber.present = 0;
            mapper->patient.present = 0;
            mapper->subscriber_info.present = 0;
            mapper->subscriber_demographics.present = 0;
            mapper->patient_info.present = 0;
            mapper->patient_demographics.present = 0;
            mapper->current_party = X12_837_PARTY_NONE;
            mapper->current_provider_context = provider_context_from_nm1(seg->elements[0]);
            return X12_OK;
        }
        if (x12_str_eq_cstr(seg->elements[0], "82")) {
            mapper->current_provider_context = provider_context_from_nm1(seg->elements[0]);
            if (mapper->current_claim_id.len > 0u) {
                return write_nm1_reference(mapper, seg, "ClaimReferencedRenderingProvider", TOK_PROVIDER_ID);
            }

            buffer_segment(&mapper->rendering_provider, seg);
            return X12_OK;
        }
        if (x12_str_eq_cstr(seg->elements[0], "DN")) {
            mapper->current_provider_context = provider_context_from_nm1(seg->elements[0]);
            return write_claim_scoped_nm1_reference(
                mapper,
                seg,
                "ClaimReferencedReferringProvider",
                TOK_PROVIDER_ID
            );
        }
        if (x12_str_eq_cstr(seg->elements[0], "DQ")) {
            mapper->current_provider_context = provider_context_from_nm1(seg->elements[0]);
            return write_claim_scoped_nm1_reference(
                mapper,
                seg,
                "ClaimReferencedSupervisingProvider",
                TOK_PROVIDER_ID
            );
        }
        if (x12_str_eq_cstr(seg->elements[0], "77")) {
            mapper->current_provider_context = provider_context_from_nm1(seg->elements[0]);
            return write_claim_scoped_nm1_reference(mapper, seg, "ClaimReferencedFacility", TOK_PROVIDER_ID);
        }
        if (x12_str_eq_cstr(seg->elements[0], "71")) {
            mapper->current_provider_context = provider_context_from_nm1(seg->elements[0]);
            return write_claim_scoped_nm1_reference(
                mapper,
                seg,
                "ClaimReferencedAttendingProvider",
                TOK_PROVIDER_ID
            );
        }
        if (x12_str_eq_cstr(seg->elements[0], "72")) {
            mapper->current_provider_context = provider_context_from_nm1(seg->elements[0]);
            return write_claim_scoped_nm1_reference(
                mapper,
                seg,
                "ClaimReferencedOperatingProvider",
                TOK_PROVIDER_ID
            );
        }
        if (x12_str_eq_cstr(seg->elements[0], "ZZ")) {
            mapper->current_provider_context = provider_context_from_nm1(seg->elements[0]);
            return write_claim_scoped_nm1_reference(mapper, seg, "ClaimReferencedOtherProvider", TOK_PROVIDER_ID);
        }
    }

    if (x12_str_eq_cstr(seg->tag, "SBR")) {
        clear_claim_position(mapper);
        buffer_segment(&mapper->subscriber_info, seg);
        mapper->subscriber_demographics.present = 0;
        mapper->patient_info.present = 0;
        mapper->patient_demographics.present = 0;
        mapper->current_party = X12_837_PARTY_SUBSCRIBER;
        mapper->current_provider_context = "";
        return X12_OK;
    }

    if (x12_str_eq_cstr(seg->tag, "PAT")) {
        clear_claim_position(mapper);
        buffer_segment(&mapper->patient_info, seg);
        mapper->patient_demographics.present = 0;
        mapper->current_party = X12_837_PARTY_PATIENT;
        mapper->current_provider_context = "";
        return X12_OK;
    }

    if (x12_str_eq_cstr(seg->tag, "PRV")) {
        const char *provider_context = provider_context_for_prv(mapper, element_or_empty(seg, 0));

        if (mapper->current_claim_id.len == 0u) {
            if (strcmp(provider_context, "billing_provider") == 0) {
                buffer_segment(&mapper->billing_provider_taxonomy, seg);
            } else if (strcmp(provider_context, "rendering_provider") == 0) {
                buffer_segment(&mapper->rendering_provider_taxonomy, seg);
            }
            mapper->current_provider_context = provider_context;
            return X12_OK;
        }

        return write_provider_taxonomy_recorded(mapper, seg, provider_context);
    }

    if (x12_str_eq_cstr(seg->tag, "DMG")) {
        if (mapper->current_claim_id.len > 0u) {
            return write_demographics_recorded(mapper, seg, mapper->current_party);
        }
        if (mapper->current_party == X12_837_PARTY_PATIENT) {
            buffer_segment(&mapper->patient_demographics, seg);
        } else {
            buffer_segment(&mapper->subscriber_demographics, seg);
            mapper->current_party = X12_837_PARTY_SUBSCRIBER;
        }
        return X12_OK;
    }

    if (x12_str_eq_cstr(seg->tag, "REF")) {
        return write_claim_reference_recorded(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "CL1")) {
        return write_institutional_information_recorded(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "HI")) {
        int rc = write_diagnosis_observed(mapper, seg);
        if (rc != X12_OK) {
            return rc;
        }
        return write_healthcare_codes_recorded(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "DTP")) {
        return write_date_observed(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "SV1") || x12_str_eq_cstr(seg->tag, "SV2")) {
        mapper->in_service_line = 1;
        return write_service_line_observed(mapper, seg);
    }

    return X12_OK;
}

int x12_map_837_document(x12_document_t *doc, event_writer_t *writer)
{
    x12_mapper_837_t mapper;

    if (doc == NULL || writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    /*
     * Future boundary:
     * raw EDI -> parser -> mapper -> tokenisation -> canonical event
     *         -> append-only book/journal -> projections
     */
    x12_mapper_837_init(&mapper, writer);
    return x12_document_each_segment(doc, x12_mapper_837_on_segment, &mapper);
}
