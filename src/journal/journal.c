#include "journal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JOURNAL_MAGIC "SCRIBEJ2"
#define JOURNAL_MAGIC_LEN 8u
#define JOURNAL_LEN_SIZE 4u
#define JOURNAL_MAX_RECORD_LEN (64u * 1024u * 1024u)

typedef struct {
    const char *name;
    unsigned short id;
} journal_key_def_t;

static const journal_key_def_t journal_keys[] = {
    {"event_type", 1u},
    {"source_file", 2u},
    {"source_transaction", 3u},
    {"source_segment_index", 4u},
    {"source_byte_offset", 5u},
    {"isa13", 6u},
    {"gs06", 7u},
    {"st02", 8u},
    {"encounter_id", 9u},
    {"patient_id", 10u},
    {"patient_id_token", 11u},
    {"synthetic", 12u},
    {"claim_id", 13u},
    {"claim_id_token", 14u},
    {"claim_type", 15u},
    {"service_line_number", 16u},
    {"procedure_code", 17u},
    {"description", 18u},
    {"service_date", 19u},
    {"amount", 20u},
    {"total_charge_amount", 21u},
    {"reference_scope", 22u},
    {"entity_type", 23u},
    {"last_name_or_org", 24u},
    {"first_name", 25u},
    {"id_qualifier", 26u},
    {"id_value", 27u},
    {"id_value_token", 28u},
    {"date_scope", 29u},
    {"date_qualifier", 30u},
    {"date_format", 31u},
    {"date_value", 32u},
    {"principal_diagnosis_code", 33u},
    {"other_diagnosis_codes", 34u},
    {"raw_diagnosis_elements", 35u},
    {"line_type", 36u},
    {"procedure_code_qualifier", 37u},
    {"procedure_code_set", 38u},
    {"raw_elements", 39u},
    {"remittance_id", 40u},
    {"trace_type_code", 41u},
    {"trace_number", 42u},
    {"originating_company_id", 43u},
    {"transaction_handling_code", 44u},
    {"payment_amount", 45u},
    {"credit_debit_flag", 46u},
    {"payment_method_code", 47u},
    {"payment_date", 48u},
    {"entity_identifier_code", 49u},
    {"name", 50u},
    {"claim_status_code", 51u},
    {"paid_amount", 52u},
    {"patient_responsibility_amount", 53u},
    {"claim_filing_indicator_code", 54u},
    {"payer_claim_control_number", 55u},
    {"payer_claim_control_number_token", 56u},
    {"facility_type_code", 57u},
    {"claim_frequency_type_code", 58u},
    {"line_charge_amount", 59u},
    {"line_paid_amount", 60u},
    {"paid_service_unit_count", 61u},
    {"adjustment_scope", 62u},
    {"adjustment_group_code", 63u},
    {"reason_codes", 64u},
    {"amounts", 65u},
    {"quantities", 66u},
    {"relationship_code", 67u},
    {"maintenance_type_code", 68u},
    {"benefit_status_code", 69u},
    {"insurance_line_code", 70u},
    {"plan_coverage_description", 71u},
    {"coverage_level_code", 72u},
    {"aggregate_type", 73u},
    {"aggregate_id", 74u},
    {"updated_by_event_type", 75u},
    {"update_scope", 76u},
    {"source_drop_id", 77u},
    {"claim_id_raw", 78u},
    {"payer_claim_control_number_raw", 79u},
    {"run_id", 80u},
    {"eligibility_id", 81u},
    {"transaction_set_purpose_code", 82u},
    {"transaction_date", 83u},
    {"transaction_time", 84u},
    {"transaction_type_code", 85u},
    {"member_id", 86u},
    {"member_id_token", 87u},
    {"payer_id", 88u},
    {"payer_id_token", 89u},
    {"provider_id", 90u},
    {"provider_id_token", 91u},
    {"service_type_code", 92u},
    {"date_of_birth", 93u},
    {"date_of_birth_token", 94u},
    {"gender_code", 95u},
    {"eligibility_or_benefit_information_code", 96u},
    {"insurance_type_code", 97u},
    {"time_period_qualifier", 98u},
    {"monetary_amount", 99u},
    {"percent", 100u},
    {"quantity_qualifier", 101u},
    {"quantity", 102u},
    {"authorization_or_certification_indicator", 103u},
    {"in_plan_network_indicator", 104u}
};

static unsigned short key_id_for_name(const char *name)
{
    size_t i;

    if (name == NULL) {
        return 0u;
    }

    for (i = 0u; i < sizeof(journal_keys) / sizeof(journal_keys[0]); i++) {
        if (strcmp(name, journal_keys[i].name) == 0) {
            return journal_keys[i].id;
        }
    }

    return 0u;
}

static void encode_u16_le(unsigned char out[2], unsigned short value)
{
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8u) & 0xffu);
}

static void encode_u32_le(unsigned char out[4], uint32_t value)
{
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8u) & 0xffu);
    out[2] = (unsigned char)((value >> 16u) & 0xffu);
    out[3] = (unsigned char)((value >> 24u) & 0xffu);
}

static void encode_u64_le(unsigned char out[8], unsigned long long value)
{
    size_t i;

    for (i = 0u; i < 8u; i++) {
        out[i] = (unsigned char)((value >> (i * 8u)) & 0xffu);
    }
}

static unsigned short decode_u16_le(const unsigned char value[2])
{
    return (unsigned short)(((unsigned short)value[0]) |
                            ((unsigned short)value[1] << 8u));
}

static uint32_t decode_u32_le(const unsigned char value[4])
{
    return ((uint32_t)value[0]) |
           ((uint32_t)value[1] << 8u) |
           ((uint32_t)value[2] << 16u) |
           ((uint32_t)value[3] << 24u);
}

static unsigned long long decode_u64_le(const unsigned char value[8])
{
    unsigned long long out = 0u;
    size_t i;

    for (i = 0u; i < 8u; i++) {
        out |= ((unsigned long long)value[i]) << (i * 8u);
    }

    return out;
}

static int ensure_builder_cap(journal_record_builder_t *builder, size_t extra)
{
    unsigned char *next;
    size_t next_cap;

    if (builder == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (extra > SIZE_MAX - builder->len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    if (builder->len + extra <= builder->cap) {
        return X12_OK;
    }

    next_cap = builder->cap == 0u ? 512u : builder->cap;
    while (next_cap < builder->len + extra) {
        if (next_cap > SIZE_MAX / 2u) {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
        next_cap *= 2u;
    }

    next = (unsigned char *)realloc(builder->data, next_cap);
    if (next == NULL) {
        return X12_ERR_NO_MEMORY;
    }

    builder->data = next;
    builder->cap = next_cap;
    return X12_OK;
}

static int append_bytes(journal_record_builder_t *builder, const void *data, size_t len)
{
    int rc;

    if (data == NULL && len > 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = ensure_builder_cap(builder, len);
    if (rc != X12_OK) {
        return rc;
    }
    if (len > 0u) {
        memcpy(builder->data + builder->len, data, len);
    }
    builder->len += len;
    return X12_OK;
}

static int append_u16(journal_record_builder_t *builder, unsigned short value)
{
    unsigned char encoded[2];

    encode_u16_le(encoded, value);
    return append_bytes(builder, encoded, sizeof(encoded));
}

static int append_u32(journal_record_builder_t *builder, uint32_t value)
{
    unsigned char encoded[4];

    encode_u32_le(encoded, value);
    return append_bytes(builder, encoded, sizeof(encoded));
}

static int append_field_header(
    journal_record_builder_t *builder,
    const char *key,
    journal_value_type_t type,
    size_t value_len
)
{
    unsigned short key_id = key_id_for_name(key);
    int rc;

    if (key_id == 0u) {
        return X12_ERR_UNSUPPORTED;
    }
    if (value_len > UINT32_MAX) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    if (builder->field_count == UINT16_MAX) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    rc = append_u16(builder, key_id);
    if (rc == X12_OK) {
        unsigned char type_byte = (unsigned char)type;
        rc = append_bytes(builder, &type_byte, 1u);
    }
    if (rc == X12_OK) {
        rc = append_u32(builder, (uint32_t)value_len);
    }
    if (rc == X12_OK) {
        builder->field_count++;
    }

    return rc;
}

static int copy_field_string(
    const journal_event_field_t *field,
    char *out,
    size_t out_len
)
{
    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';
    if (field == NULL || field->len >= out_len) {
        return 0;
    }

    memcpy(out, field->data, field->len);
    out[field->len] = '\0';
    return 1;
}

static const journal_event_field_t *find_field(
    const journal_event_t *event,
    const char *key
)
{
    unsigned short key_id = key_id_for_name(key);
    size_t i;

    if (event == NULL || key_id == 0u) {
        return NULL;
    }

    for (i = 0u; i < event->field_count; i++) {
        if (event->fields[i].key_id == key_id) {
            return &event->fields[i];
        }
    }

    return NULL;
}

int journal_write_header(FILE *fp)
{
    if (fp == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return fwrite(JOURNAL_MAGIC, 1u, JOURNAL_MAGIC_LEN, fp) == JOURNAL_MAGIC_LEN ?
        X12_OK :
        X12_ERR_IO;
}

void journal_record_builder_init(journal_record_builder_t *builder)
{
    if (builder != NULL) {
        builder->data = NULL;
        builder->len = 0u;
        builder->cap = 0u;
        builder->field_count = 0u;
    }
}

void journal_record_builder_free(journal_record_builder_t *builder)
{
    if (builder != NULL) {
        free(builder->data);
        journal_record_builder_init(builder);
    }
}

int journal_record_builder_reset(journal_record_builder_t *builder)
{
    int rc;

    if (builder == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    builder->len = 0u;
    builder->field_count = 0u;
    rc = append_u16(builder, 0u);
    if (rc != X12_OK) {
        return rc;
    }

    return X12_OK;
}

int journal_record_add_string(
    journal_record_builder_t *builder,
    const char *key,
    const char *value,
    size_t value_len
)
{
    int rc;

    if (value == NULL) {
        value = "";
        value_len = 0u;
    }

    rc = append_field_header(builder, key, JOURNAL_VALUE_STRING, value_len);
    if (rc != X12_OK) {
        return rc;
    }
    return append_bytes(builder, value, value_len);
}

int journal_record_add_cstring(
    journal_record_builder_t *builder,
    const char *key,
    const char *value
)
{
    if (value == NULL) {
        value = "";
    }

    return journal_record_add_string(builder, key, value, strlen(value));
}

int journal_record_add_bool(
    journal_record_builder_t *builder,
    const char *key,
    int value
)
{
    unsigned char bool_value = value ? 1u : 0u;
    int rc;

    rc = append_field_header(builder, key, JOURNAL_VALUE_BOOL, 1u);
    if (rc != X12_OK) {
        return rc;
    }
    return append_bytes(builder, &bool_value, sizeof(bool_value));
}

int journal_record_add_u64(
    journal_record_builder_t *builder,
    const char *key,
    unsigned long long value
)
{
    unsigned char encoded[8];
    int rc;

    rc = append_field_header(builder, key, JOURNAL_VALUE_U64, sizeof(encoded));
    if (rc != X12_OK) {
        return rc;
    }
    encode_u64_le(encoded, value);
    return append_bytes(builder, encoded, sizeof(encoded));
}

int journal_record_add_string_array(
    journal_record_builder_t *builder,
    const char *key,
    const x12_str_t *values,
    size_t count
)
{
    size_t payload_len = 2u;
    size_t i;
    int rc;

    if (values == NULL && count > 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (count > UINT16_MAX) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    for (i = 0u; i < count; i++) {
        if (values[i].len > UINT32_MAX ||
            payload_len > SIZE_MAX - 4u ||
            payload_len + 4u > SIZE_MAX - values[i].len) {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
        payload_len += 4u + values[i].len;
    }

    rc = append_field_header(builder, key, JOURNAL_VALUE_STRING_ARRAY, payload_len);
    if (rc == X12_OK) {
        rc = append_u16(builder, (unsigned short)count);
    }
    for (i = 0u; i < count && rc == X12_OK; i++) {
        rc = append_u32(builder, (uint32_t)values[i].len);
        if (rc == X12_OK) {
            rc = append_bytes(builder, values[i].ptr, values[i].len);
        }
    }

    return rc;
}

int journal_write_record(
    FILE *fp,
    journal_record_builder_t *builder,
    long long *out_offset,
    long long *out_stored_len
)
{
    unsigned char encoded_len[4];
    long offset;

    if (fp == NULL || builder == NULL || builder->data == NULL || builder->len < 2u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (builder->len > UINT32_MAX) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    encode_u16_le(builder->data, builder->field_count);
    encode_u32_le(encoded_len, (uint32_t)builder->len);

    offset = ftell(fp);
    if (offset < 0) {
        return X12_ERR_IO;
    }
    if (fwrite(encoded_len, 1u, sizeof(encoded_len), fp) != sizeof(encoded_len)) {
        return X12_ERR_IO;
    }
    if (fwrite(builder->data, 1u, builder->len, fp) != builder->len) {
        return X12_ERR_IO;
    }

    if (out_offset != NULL) {
        *out_offset = (long long)offset;
    }
    if (out_stored_len != NULL) {
        *out_stored_len = (long long)(sizeof(encoded_len) + builder->len);
    }

    return X12_OK;
}

void journal_reader_init(journal_reader_t *reader)
{
    if (reader != NULL) {
        reader->fp = NULL;
        reader->buffer = NULL;
        reader->buffer_cap = 0u;
    }
}

int journal_reader_open(journal_reader_t *reader, const char *path)
{
    unsigned char magic[JOURNAL_MAGIC_LEN];

    if (reader == NULL || path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    journal_reader_init(reader);
    reader->fp = fopen(path, "rb");
    if (reader->fp == NULL) {
        return X12_ERR_IO;
    }

    if (fread(magic, 1u, sizeof(magic), reader->fp) != sizeof(magic)) {
        (void)journal_reader_close(reader);
        return X12_ERR_IO;
    }
    if (memcmp(magic, JOURNAL_MAGIC, sizeof(magic)) != 0) {
        (void)journal_reader_close(reader);
        return X12_ERR_UNSUPPORTED;
    }

    return X12_OK;
}

int journal_reader_next(journal_reader_t *reader, journal_event_t *out)
{
    unsigned char encoded_len[4];
    unsigned char *cursor;
    unsigned char *end;
    unsigned short field_count;
    uint32_t len;
    size_t bytes_read;
    size_t i;
    long record_offset;

    if (reader == NULL || reader->fp == NULL || out == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));

    record_offset = ftell(reader->fp);
    if (record_offset < 0) {
        return X12_ERR_IO;
    }

    bytes_read = fread(encoded_len, 1u, sizeof(encoded_len), reader->fp);
    if (bytes_read == 0u) {
        return feof(reader->fp) ? X12_OK : X12_ERR_IO;
    }
    if (bytes_read != sizeof(encoded_len)) {
        return X12_ERR_IO;
    }

    len = decode_u32_le(encoded_len);
    if (len < 2u || len > JOURNAL_MAX_RECORD_LEN) {
        return X12_ERR_IO;
    }

    if ((size_t)len > reader->buffer_cap) {
        unsigned char *next = (unsigned char *)realloc(reader->buffer, (size_t)len);
        if (next == NULL) {
            return X12_ERR_NO_MEMORY;
        }
        reader->buffer = next;
        reader->buffer_cap = (size_t)len;
    }

    if (fread(reader->buffer, 1u, (size_t)len, reader->fp) != (size_t)len) {
        return X12_ERR_IO;
    }

    cursor = reader->buffer;
    end = reader->buffer + len;
    field_count = decode_u16_le(cursor);
    cursor += 2u;
    if (field_count > JOURNAL_EVENT_MAX_FIELDS) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    out->record = reader->buffer;
    out->record_len = (size_t)len;
    out->offset = (long long)record_offset;
    out->stored_len = (long long)(JOURNAL_LEN_SIZE + (size_t)len);
    out->field_count = field_count;

    for (i = 0u; i < field_count; i++) {
        uint32_t value_len;

        if ((size_t)(end - cursor) < 7u) {
            return X12_ERR_IO;
        }
        out->fields[i].key_id = decode_u16_le(cursor);
        cursor += 2u;
        out->fields[i].type = *cursor++;
        value_len = decode_u32_le(cursor);
        cursor += 4u;
        if ((size_t)(end - cursor) < value_len) {
            return X12_ERR_IO;
        }
        out->fields[i].data = cursor;
        out->fields[i].len = value_len;
        cursor += value_len;
    }
    if (cursor != end) {
        return X12_ERR_IO;
    }

    return X12_OK;
}

int journal_reader_close(journal_reader_t *reader)
{
    int rc = X12_OK;

    if (reader == NULL) {
        return X12_OK;
    }

    if (reader->fp != NULL && fclose(reader->fp) != 0) {
        rc = X12_ERR_IO;
    }
    free(reader->buffer);
    journal_reader_init(reader);
    return rc;
}

int journal_event_get_string(
    const journal_event_t *event,
    const char *key,
    char *out,
    size_t out_len
)
{
    const journal_event_field_t *field = find_field(event, key);

    if (field == NULL || field->type != JOURNAL_VALUE_STRING) {
        if (out != NULL && out_len > 0u) {
            out[0] = '\0';
        }
        return 0;
    }

    return copy_field_string(field, out, out_len);
}

int journal_event_get_bool(const journal_event_t *event, const char *key, int *out)
{
    const journal_event_field_t *field = find_field(event, key);

    if (out == NULL || field == NULL ||
        field->type != JOURNAL_VALUE_BOOL ||
        field->len != 1u) {
        return 0;
    }

    *out = field->data[0] != 0u;
    return 1;
}

int journal_event_get_number_text(
    const journal_event_t *event,
    const char *key,
    char *out,
    size_t out_len
)
{
    const journal_event_field_t *field = find_field(event, key);
    unsigned long long value;
    int written;

    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';
    if (field == NULL) {
        return 0;
    }
    if (field->type == JOURNAL_VALUE_STRING) {
        return copy_field_string(field, out, out_len);
    }
    if (field->type != JOURNAL_VALUE_U64 || field->len != 8u) {
        return 0;
    }

    value = decode_u64_le(field->data);
    written = snprintf(out, out_len, "%llu", value);
    if (written < 0 || (size_t)written >= out_len) {
        out[0] = '\0';
        return 0;
    }

    return 1;
}

int journal_event_get_array_string_at(
    const journal_event_t *event,
    const char *key,
    size_t index,
    char *out,
    size_t out_len
)
{
    const journal_event_field_t *field = find_field(event, key);
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned short count;
    size_t i;

    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';

    if (field == NULL || field->type != JOURNAL_VALUE_STRING_ARRAY || field->len < 2u) {
        return 0;
    }

    cursor = field->data;
    end = field->data + field->len;
    count = decode_u16_le(cursor);
    cursor += 2u;

    if (index >= count) {
        return 0;
    }

    for (i = 0u; i < count; i++) {
        uint32_t len;

        if ((size_t)(end - cursor) < 4u) {
            return 0;
        }
        len = decode_u32_le(cursor);
        cursor += 4u;
        if ((size_t)(end - cursor) < len) {
            return 0;
        }
        if (i == index) {
            if ((size_t)len >= out_len) {
                return 0;
            }
            memcpy(out, cursor, len);
            out[len] = '\0';
            return 1;
        }
        cursor += len;
    }

    return 0;
}
