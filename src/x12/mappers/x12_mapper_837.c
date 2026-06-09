#include "x12_mapper_837.h"

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

static const char *procedure_code_set(x12_str_t qualifier)
{
    if (x12_str_eq_cstr(qualifier, "HC")) {
        return "CPT/HCPCS";
    }

    return "";
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

static void clear_claim_position(x12_mapper_837_t *mapper)
{
    mapper->current_claim_id = empty_str();
    mapper->current_service_line_number = empty_str();
    mapper->in_service_line = 0;
    mapper->rendering_provider.present = 0;
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
    int rc;

    mapper->current_claim_id = element_or_empty(seg, 0);
    mapper->current_service_line_number = empty_str();
    mapper->in_service_line = 0;

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
    if (write_payload_end(fp) != X12_OK) {
        return X12_ERR_IO;
    }
    rc = event_writer_end_event(mapper->writer);
    if (rc != X12_OK) {
        return rc;
    }
    return flush_claim_context_references(mapper);
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

        if (x12_str_eq_cstr(qualifier, "ABK")) {
            rc = normalize_diagnosis_code(
                raw_code,
                principal_buffer,
                sizeof(principal_buffer),
                &principal_diagnosis
            );
            if (rc != X12_OK) {
                return rc;
            }
        } else if (x12_str_eq_cstr(qualifier, "ABF") &&
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

static int write_service_line_observed(
    x12_mapper_837_t *mapper,
    const x12_segment_t *seg
)
{
    FILE *fp = event_writer_stream(mapper->writer);
    x12_str_t procedure_qualifier;
    x12_str_t procedure_code;
    int rc;

    split_first_component(element_or_empty(seg, 0), mapper->component_sep, &procedure_qualifier, &procedure_code);

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
    if (event_writer_write_string_field(fp, "procedure_code_qualifier", procedure_qualifier, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "procedure_code_set", procedure_code_set(procedure_qualifier), 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_string_field(fp, "procedure_code", procedure_code, 1) != X12_OK) {
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
            return X12_OK;
        }
        if (x12_str_eq_cstr(seg->elements[0], "QC")) {
            clear_claim_position(mapper);
            buffer_segment(&mapper->patient, seg);
            return X12_OK;
        }
        if (x12_str_eq_cstr(seg->elements[0], "85")) {
            clear_claim_position(mapper);
            buffer_segment(&mapper->billing_provider, seg);
            mapper->subscriber.present = 0;
            mapper->patient.present = 0;
            return X12_OK;
        }
        if (x12_str_eq_cstr(seg->elements[0], "82")) {
            if (mapper->current_claim_id.len > 0u) {
                return write_nm1_reference(mapper, seg, "ClaimReferencedRenderingProvider", TOK_PROVIDER_ID);
            }

            buffer_segment(&mapper->rendering_provider, seg);
            return X12_OK;
        }
    }

    if (x12_str_eq_cstr(seg->tag, "HI")) {
        return write_diagnosis_observed(mapper, seg);
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
