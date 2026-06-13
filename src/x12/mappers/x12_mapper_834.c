#include "x12_mapper_834.h"

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

static int add_phi_str(
    event_writer_t *writer,
    const char *name,
    x12_str_t value)
{
    if (!event_writer_include_phi(writer))
    {
        return X12_OK;
    }
    return event_writer_add_str(writer, name, value);
}

static int add_tokenized_or_phi(
    event_writer_t *writer,
    const char *name,
    token_type_t type,
    x12_str_t raw)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t token_value;
    int include_phi = event_writer_include_phi(writer);

    if (include_phi)
    {
        TRY(event_writer_record_phi_mapping(writer, type, raw));
        return event_writer_add_str(writer, name, raw);
    }

    if (raw.len == 0u)
    {
        return event_writer_add_str(writer, name, empty_str());
    }

    TRY(tokenise_value(type, raw, token, sizeof(token)));
    TRY(event_writer_record_phi_mapping(writer, type, raw));

    token_value.ptr = token;
    token_value.len = strlen(token);
    return event_writer_add_str(writer, name, token_value);
}

static int add_phi_token(
    event_writer_t *writer,
    const char *name,
    token_type_t type,
    x12_str_t raw)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t token_value;

    if (!event_writer_include_phi(writer))
    {
        return X12_OK;
    }
    if (raw.len == 0u)
    {
        return event_writer_add_str(writer, name, empty_str());
    }

    TRY(tokenise_value(type, raw, token, sizeof(token)));
    token_value.ptr = token;
    token_value.len = strlen(token);
    return event_writer_add_str(writer, name, token_value);
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

static int write_member_referenced(
    x12_mapper_834_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    TRY(event_writer_begin_event(w, "MemberReferenced", seg));
    TRY(event_writer_add_str(w, "entity_type", element_or_empty(seg, 1)));
    TRY(add_phi_str(w, "last_name_or_org", element_or_empty(seg, 2)));
    TRY(add_phi_str(w, "first_name", element_or_empty(seg, 3)));
    TRY(event_writer_record_phi_name(
        w,
        name_token_type_for_id(TOK_MEMBER_ID),
        element_or_empty(seg, 2),
        element_or_empty(seg, 3),
        TOK_MEMBER_ID,
        element_or_empty(seg, 8)));
    TRY(event_writer_add_str(w, "id_qualifier", element_or_empty(seg, 7)));
    TRY(add_tokenized_or_phi(w, "id_value", TOK_MEMBER_ID, element_or_empty(seg, 8)));
    TRY(add_phi_token(w, "id_value_token", TOK_MEMBER_ID, element_or_empty(seg, 8)));
    return event_writer_end_event(w);
}

static int write_member_enrollment_changed(
    x12_mapper_834_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    TRY(event_writer_begin_event(w, "MemberEnrollmentChanged", seg));
    TRY(event_writer_add_str(w, "relationship_code", element_or_empty(seg, 1)));
    TRY(event_writer_add_str(w, "maintenance_type_code", element_or_empty(seg, 2)));
    TRY(event_writer_add_str(w, "benefit_status_code", element_or_empty(seg, 4)));
    TRY(event_writer_add_str_array(w, "raw_elements", seg->elements, seg->element_count));
    return event_writer_end_event(w);
}

static int write_coverage_date_observed(
    x12_mapper_834_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    TRY(event_writer_begin_event(w, "CoverageDateObserved", seg));
    TRY(event_writer_add_str(w, "date_qualifier", element_or_empty(seg, 0)));
    TRY(event_writer_add_str(w, "date_format", element_or_empty(seg, 1)));
    TRY(event_writer_add_str(w, "date_value", element_or_empty(seg, 2)));
    return event_writer_end_event(w);
}

static int write_health_coverage_observed(
    x12_mapper_834_t *mapper,
    const x12_segment_t *seg)
{
    event_writer_t *w = mapper->writer;

    TRY(event_writer_begin_event(w, "HealthCoverageObserved", seg));
    TRY(event_writer_add_str(w, "maintenance_type_code", element_or_empty(seg, 0)));
    TRY(event_writer_add_str(w, "insurance_line_code", element_or_empty(seg, 2)));
    TRY(event_writer_add_str(w, "plan_coverage_description", element_or_empty(seg, 3)));
    TRY(event_writer_add_str(w, "coverage_level_code", element_or_empty(seg, 4)));
    TRY(event_writer_add_str_array(w, "raw_elements", seg->elements, seg->element_count));
    return event_writer_end_event(w);
}

void x12_mapper_834_init(x12_mapper_834_t *mapper, event_writer_t *writer)
{
    if (mapper == NULL)
    {
        return;
    }

    mapper->writer = writer;
}

int x12_mapper_834_on_segment(const x12_segment_t *seg, void *user)
{
    x12_mapper_834_t *mapper = (x12_mapper_834_t *)user;

    if (seg == NULL || mapper == NULL || mapper->writer == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    event_writer_observe_control(mapper->writer, seg);

    if (x12_str_eq_cstr(seg->tag, "NM1") &&
        seg->element_count > 0u &&
        x12_str_eq_cstr(seg->elements[0], "IL"))
    {
        return write_member_referenced(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "INS"))
    {
        return write_member_enrollment_changed(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "DTP"))
    {
        return write_coverage_date_observed(mapper, seg);
    }
    if (x12_str_eq_cstr(seg->tag, "HD"))
    {
        return write_health_coverage_observed(mapper, seg);
    }

    return X12_OK;
}

int x12_map_834_document(x12_document_t *doc, event_writer_t *writer)
{
    x12_mapper_834_t mapper;

    if (doc == NULL || writer == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    x12_mapper_834_init(&mapper, writer);
    return x12_document_each_segment(doc, x12_mapper_834_on_segment, &mapper);
}
