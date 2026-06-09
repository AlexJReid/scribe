#include "x12_mapper_834.h"

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

static int write_member_referenced(
    x12_mapper_834_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, "MemberReferenced", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "entity_type", element_or_empty(seg, 1), 0) != X12_OK) {
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
            name_token_type_for_id(TOK_MEMBER_ID),
            element_or_empty(seg, 2),
            element_or_empty(seg, 3),
            TOK_MEMBER_ID,
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
            TOK_MEMBER_ID,
            element_or_empty(seg, 8),
            1,
            event_writer_include_phi(mapper->writer)
        ) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_phi_token_field(
            fp,
            "id_value_token",
            TOK_MEMBER_ID,
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

static int write_member_enrollment_changed(
    x12_mapper_834_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, "MemberEnrollmentChanged", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "relationship_code", element_or_empty(seg, 1), 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "maintenance_type_code", element_or_empty(seg, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "benefit_status_code", element_or_empty(seg, 4), 1) != X12_OK) {
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

static int write_coverage_date_observed(
    x12_mapper_834_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, "CoverageDateObserved", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "date_qualifier", element_or_empty(seg, 0), 0) != X12_OK) {
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

static int write_health_coverage_observed(
    x12_mapper_834_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    int rc;

    rc = event_writer_begin_event(mapper->writer, "HealthCoverageObserved", seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (write_payload_start(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "maintenance_type_code", element_or_empty(seg, 0), 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "insurance_line_code", element_or_empty(seg, 2), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "plan_coverage_description", element_or_empty(seg, 3), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "coverage_level_code", element_or_empty(seg, 4), 1) != X12_OK) {
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

void x12_mapper_834_init(x12_mapper_834_t *mapper, event_writer_t *writer)
{
    if (mapper == NULL) {
        return;
    }

    mapper->writer = writer;
}

int x12_mapper_834_on_segment(const x12_segment_t *seg, void *user)
{
    x12_mapper_834_t *mapper = (x12_mapper_834_t *)user;

    if (seg == NULL || mapper == NULL || mapper->writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    event_writer_observe_control(mapper->writer, seg);

    if (x12_str_eq_cstr(seg->tag, "NM1") &&
        seg->element_count > 0u &&
        x12_str_eq_cstr(seg->elements[0], "IL")) {
        return write_member_referenced(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "INS")) {
        return write_member_enrollment_changed(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "DTP")) {
        return write_coverage_date_observed(mapper, seg);
    }

    if (x12_str_eq_cstr(seg->tag, "HD")) {
        return write_health_coverage_observed(mapper, seg);
    }

    return X12_OK;
}

int x12_map_834_document(x12_document_t *doc, event_writer_t *writer)
{
    x12_mapper_834_t mapper;

    if (doc == NULL || writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    /*
     * Future boundary:
     * raw EDI -> parser -> mapper -> tokenisation -> canonical event
     *         -> append-only book/journal -> projections
     */
    x12_mapper_834_init(&mapper, writer);
    return x12_document_each_segment(doc, x12_mapper_834_on_segment, &mapper);
}
