#include "journal.h"
#include "try.h"

#ifndef _WIN32
#include <dirent.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zstd.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define JOURNAL_MAGIC "SCRIBEJ5"
#define JOURNAL_MAGIC_LEN 8u
#define JOURNAL_LEN_SIZE 4u
#define JOURNAL_MAX_RECORD_LEN (64u * 1024u * 1024u)
#define JOURNAL_CONTROL_DICTIONARY 1u

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

    for (i = 0u; i < 8u; i++)
    {
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

    for (i = 0u; i < 8u; i++)
    {
        out |= ((unsigned long long)value[i]) << (i * 8u);
    }

    return out;
}

static void clear_builder_dictionary(journal_record_builder_t *builder)
{
    size_t i;

    if (builder == NULL)
    {
        return;
    }

    for (i = 0u; i < builder->key_count; i++)
    {
        free(builder->key_names[i]);
    }
    free(builder->key_names);
    free(builder->key_written);
    builder->key_names = NULL;
    builder->key_written = NULL;
    builder->key_count = 0u;
    builder->key_cap = 0u;
}

static int ensure_builder_key_cap(journal_record_builder_t *builder)
{
    char **next_names;
    unsigned char *next_written;
    size_t old_cap;
    size_t next_cap;
    size_t i;

    if (builder == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (builder->key_count < builder->key_cap)
    {
        return X12_OK;
    }

    old_cap = builder->key_cap;
    next_cap = old_cap == 0u ? 16u : old_cap * 2u;
    if (next_cap <= old_cap || next_cap > UINT16_MAX)
    {
        next_cap = UINT16_MAX;
    }
    if (next_cap <= old_cap)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    next_names = (char **)realloc(builder->key_names, next_cap * sizeof(*next_names));
    if (next_names == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    builder->key_names = next_names;

    next_written = (unsigned char *)realloc(
        builder->key_written,
        next_cap * sizeof(*next_written));
    if (next_written == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    builder->key_written = next_written;

    for (i = old_cap; i < next_cap; i++)
    {
        builder->key_names[i] = NULL;
        builder->key_written[i] = 0u;
    }
    builder->key_cap = next_cap;
    return X12_OK;
}

static int builder_key_id_for_name(
    journal_record_builder_t *builder,
    const char *name,
    unsigned short *out)
{
    char *copy;
    size_t len;
    size_t i;

    if (builder == NULL || name == NULL || out == NULL || name[0] == '\0')
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    for (i = 0u; i < builder->key_count; i++)
    {
        if (strcmp(builder->key_names[i], name) == 0)
        {
            *out = (unsigned short)(i + 1u);
            return X12_OK;
        }
    }

    if (builder->key_count >= UINT16_MAX)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    len = strlen(name);
    if (len == 0u || len > UINT16_MAX)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    TRY(ensure_builder_key_cap(builder));

    copy = (char *)malloc(len + 1u);
    if (copy == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    memcpy(copy, name, len + 1u);

    builder->key_names[builder->key_count] = copy;
    builder->key_written[builder->key_count] = 0u;
    builder->key_count++;
    *out = (unsigned short)builder->key_count;
    return X12_OK;
}

static int ensure_builder_cap(journal_record_builder_t *builder, size_t extra)
{
    unsigned char *next;
    size_t next_cap;

    if (builder == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (extra > SIZE_MAX - builder->len)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    if (builder->len + extra <= builder->cap)
    {
        return X12_OK;
    }

    next_cap = builder->cap == 0u ? 512u : builder->cap;
    while (next_cap < builder->len + extra)
    {
        if (next_cap > SIZE_MAX / 2u)
        {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
        next_cap *= 2u;
    }

    next = (unsigned char *)realloc(builder->data, next_cap);
    if (next == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }

    builder->data = next;
    builder->cap = next_cap;
    return X12_OK;
}

static int append_bytes(journal_record_builder_t *builder, const void *data, size_t len)
{
    if (data == NULL && len > 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    TRY(ensure_builder_cap(builder, len));
    if (len > 0u)
    {
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
    size_t value_len)
{
    unsigned short key_id;
    int rc;

    TRY(builder_key_id_for_name(builder, key, &key_id));
    if (value_len > UINT32_MAX)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    if (builder->field_count == UINT16_MAX)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    rc = append_u16(builder, key_id);
    if (rc == X12_OK)
    {
        unsigned char type_byte = (unsigned char)type;
        rc = append_bytes(builder, &type_byte, 1u);
    }
    if (rc == X12_OK)
    {
        rc = append_u32(builder, (uint32_t)value_len);
    }
    if (rc == X12_OK)
    {
        builder->field_count++;
    }

    return rc;
}

static int copy_field_string(
    const journal_event_field_t *field,
    char *out,
    size_t out_len)
{
    if (out == NULL || out_len == 0u)
    {
        return 0;
    }
    out[0] = '\0';
    if (field == NULL || field->len >= out_len)
    {
        return 0;
    }

    memcpy(out, field->data, field->len);
    out[field->len] = '\0';
    return 1;
}

static const journal_event_field_t *find_field(
    const journal_event_t *event,
    const char *key)
{
    size_t i;

    if (event == NULL || key == NULL)
    {
        return NULL;
    }

    for (i = 0u; i < event->field_count; i++)
    {
        if (event->fields[i].key_name != NULL &&
            strcmp(event->fields[i].key_name, key) == 0)
        {
            return &event->fields[i];
        }
    }

    return NULL;
}

static void clear_context_slot(char **slot)
{
    if (slot != NULL)
    {
        free(*slot);
        *slot = NULL;
    }
}

static void clear_reader_context(journal_reader_t *reader)
{
    if (reader == NULL)
    {
        return;
    }

    clear_context_slot(&reader->context_source_file);
    clear_context_slot(&reader->context_source_transaction);
    clear_context_slot(&reader->context_source_drop_id);
    clear_context_slot(&reader->context_run_id);
    clear_context_slot(&reader->context_isa13);
    clear_context_slot(&reader->context_gs06);
    clear_context_slot(&reader->context_st02);
}

static void clear_reader_dictionary(journal_reader_t *reader)
{
    size_t i;

    if (reader == NULL)
    {
        return;
    }

    for (i = 0u; i < reader->dictionary_count; i++)
    {
        free(reader->dictionary_names[i]);
    }
    free(reader->dictionary_names);
    reader->dictionary_names = NULL;
    reader->dictionary_count = 0u;
    reader->dictionary_cap = 0u;
}

static int ensure_reader_dictionary_cap(journal_reader_t *reader, size_t min_cap)
{
    char **next;
    size_t old_cap;
    size_t next_cap;
    size_t i;

    if (reader == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (min_cap <= reader->dictionary_cap)
    {
        return X12_OK;
    }

    old_cap = reader->dictionary_cap;
    next_cap = old_cap == 0u ? 16u : old_cap;
    while (next_cap < min_cap)
    {
        if (next_cap > SIZE_MAX / 2u)
        {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
        next_cap *= 2u;
    }

    next = (char **)realloc(reader->dictionary_names, next_cap * sizeof(*next));
    if (next == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    reader->dictionary_names = next;
    for (i = old_cap; i < next_cap; i++)
    {
        reader->dictionary_names[i] = NULL;
    }
    reader->dictionary_cap = next_cap;
    return X12_OK;
}

static int reader_dictionary_set(
    journal_reader_t *reader,
    unsigned short key_id,
    const unsigned char *name,
    size_t name_len)
{
    char *copy;
    int rc;

    if (reader == NULL || key_id == 0u || name == NULL || name_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = ensure_reader_dictionary_cap(reader, (size_t)key_id);
    if (rc != X12_OK)
    {
        return rc;
    }

    copy = (char *)malloc(name_len + 1u);
    if (copy == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    memcpy(copy, name, name_len);
    copy[name_len] = '\0';

    free(reader->dictionary_names[key_id - 1u]);
    reader->dictionary_names[key_id - 1u] = copy;
    if ((size_t)key_id > reader->dictionary_count)
    {
        reader->dictionary_count = (size_t)key_id;
    }
    return X12_OK;
}

static const char *reader_dictionary_name(
    const journal_reader_t *reader,
    unsigned short key_id)
{
    if (reader == NULL || key_id == 0u || (size_t)key_id > reader->dictionary_count)
    {
        return NULL;
    }
    return reader->dictionary_names[key_id - 1u];
}

static int copy_field_to_context_slot(
    const journal_event_t *event,
    const char *key,
    char **slot,
    int clear_when_missing)
{
    const journal_event_field_t *field;
    char *copy;

    if (event == NULL || key == NULL || slot == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    field = find_field(event, key);
    if (field == NULL)
    {
        if (clear_when_missing)
        {
            clear_context_slot(slot);
        }
        return X12_OK;
    }
    if (field->type != JOURNAL_VALUE_STRING || field->len == SIZE_MAX)
    {
        return X12_ERR_IO;
    }

    copy = (char *)malloc(field->len + 1u);
    if (copy == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    if (field->len > 0u)
    {
        memcpy(copy, field->data, field->len);
    }
    copy[field->len] = '\0';

    free(*slot);
    *slot = copy;
    return X12_OK;
}

static void attach_reader_context(journal_reader_t *reader, journal_event_t *event)
{
    if (reader == NULL || event == NULL)
    {
        return;
    }

    event->context_source_file = reader->context_source_file;
    event->context_source_transaction = reader->context_source_transaction;
    event->context_source_drop_id = reader->context_source_drop_id;
    event->context_run_id = reader->context_run_id;
    event->context_isa13 = reader->context_isa13;
    event->context_gs06 = reader->context_gs06;
    event->context_st02 = reader->context_st02;
}

static int update_reader_context(journal_reader_t *reader, journal_event_t *event)
{
    int reset_context;
    int rc;

    if (reader == NULL || event == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    reset_context = find_field(event, "source_file") != NULL ||
                    find_field(event, "source_transaction") != NULL;

    rc = copy_field_to_context_slot(
        event,
        "source_file",
        &reader->context_source_file,
        reset_context);
    if (rc == X12_OK)
    {
        rc = copy_field_to_context_slot(
            event,
            "source_transaction",
            &reader->context_source_transaction,
            reset_context);
    }
    if (rc == X12_OK)
    {
        rc = copy_field_to_context_slot(
            event,
            "source_drop_id",
            &reader->context_source_drop_id,
            reset_context);
    }
    if (rc == X12_OK)
    {
        rc = copy_field_to_context_slot(
            event,
            "run_id",
            &reader->context_run_id,
            reset_context);
    }
    if (rc == X12_OK)
    {
        rc = copy_field_to_context_slot(
            event,
            "isa13",
            &reader->context_isa13,
            reset_context);
    }
    if (rc == X12_OK)
    {
        rc = copy_field_to_context_slot(
            event,
            "gs06",
            &reader->context_gs06,
            reset_context);
    }
    if (rc == X12_OK)
    {
        rc = copy_field_to_context_slot(
            event,
            "st02",
            &reader->context_st02,
            reset_context);
    }
    if (rc != X12_OK)
    {
        return rc;
    }

    attach_reader_context(reader, event);
    return X12_OK;
}

static int copy_context_string(const char *value, char *out, size_t out_len)
{
    size_t len;

    if (out == NULL || out_len == 0u)
    {
        return 0;
    }
    out[0] = '\0';
    if (value == NULL)
    {
        return 0;
    }

    len = strlen(value);
    if (len >= out_len)
    {
        return 0;
    }
    memcpy(out, value, len + 1u);
    return 1;
}

static const char *event_context_string(const journal_event_t *event, const char *key)
{
    if (event == NULL || key == NULL)
    {
        return NULL;
    }

    if (strcmp(key, "source_file") == 0)
    {
        return event->context_source_file;
    }
    if (strcmp(key, "source_transaction") == 0)
    {
        return event->context_source_transaction;
    }
    if (strcmp(key, "source_drop_id") == 0)
    {
        return event->context_source_drop_id;
    }
    if (strcmp(key, "run_id") == 0)
    {
        return event->context_run_id;
    }
    if (strcmp(key, "isa13") == 0)
    {
        return event->context_isa13;
    }
    if (strcmp(key, "gs06") == 0)
    {
        return event->context_gs06;
    }
    if (strcmp(key, "st02") == 0)
    {
        return event->context_st02;
    }

    return NULL;
}

int journal_write_header(FILE *fp)
{
    if (fp == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return fwrite(JOURNAL_MAGIC, 1u, JOURNAL_MAGIC_LEN, fp) == JOURNAL_MAGIC_LEN ? X12_OK : X12_ERR_IO;
}

int journal_read_header(FILE *fp)
{
    unsigned char magic[JOURNAL_MAGIC_LEN];

    if (fp == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (fread(magic, 1u, sizeof(magic), fp) != sizeof(magic))
    {
        return X12_ERR_IO;
    }
    return memcmp(magic, JOURNAL_MAGIC, sizeof(magic)) == 0 ? X12_OK : X12_ERR_UNSUPPORTED;
}

void journal_record_builder_init(journal_record_builder_t *builder)
{
    if (builder != NULL)
    {
        builder->data = NULL;
        builder->len = 0u;
        builder->cap = 0u;
        builder->field_count = 0u;
        builder->key_names = NULL;
        builder->key_written = NULL;
        builder->key_count = 0u;
        builder->key_cap = 0u;
    }
}

void journal_record_builder_free(journal_record_builder_t *builder)
{
    if (builder != NULL)
    {
        free(builder->data);
        clear_builder_dictionary(builder);
        journal_record_builder_init(builder);
    }
}

int journal_record_builder_reset(journal_record_builder_t *builder)
{
    int rc;

    if (builder == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    builder->len = 0u;
    builder->field_count = 0u;
    rc = append_u16(builder, 0u);
    if (rc != X12_OK)
    {
        return rc;
    }

    return X12_OK;
}

int journal_record_add_string(
    journal_record_builder_t *builder,
    const char *key,
    const char *value,
    size_t value_len)
{
    if (value == NULL)
    {
        value = "";
        value_len = 0u;
    }

    TRY(append_field_header(builder, key, JOURNAL_VALUE_STRING, value_len));
    return append_bytes(builder, value, value_len);
}

int journal_record_add_cstring(
    journal_record_builder_t *builder,
    const char *key,
    const char *value)
{
    if (value == NULL)
    {
        value = "";
    }

    return journal_record_add_string(builder, key, value, strlen(value));
}

int journal_record_add_bool(
    journal_record_builder_t *builder,
    const char *key,
    int value)
{
    unsigned char bool_value = value ? 1u : 0u;

    TRY(append_field_header(builder, key, JOURNAL_VALUE_BOOL, 1u));
    return append_bytes(builder, &bool_value, sizeof(bool_value));
}

int journal_record_add_u64(
    journal_record_builder_t *builder,
    const char *key,
    unsigned long long value)
{
    unsigned char encoded[8];

    TRY(append_field_header(builder, key, JOURNAL_VALUE_U64, sizeof(encoded)));
    encode_u64_le(encoded, value);
    return append_bytes(builder, encoded, sizeof(encoded));
}

int journal_record_add_string_array(
    journal_record_builder_t *builder,
    const char *key,
    const x12_str_t *values,
    size_t count)
{
    size_t payload_len = 2u;
    size_t i;
    int rc;

    if (values == NULL && count > 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (count > UINT16_MAX)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    for (i = 0u; i < count; i++)
    {
        if (values[i].len > UINT32_MAX ||
            payload_len > SIZE_MAX - 4u ||
            payload_len + 4u > SIZE_MAX - values[i].len)
        {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
        payload_len += 4u + values[i].len;
    }

    rc = append_field_header(builder, key, JOURNAL_VALUE_STRING_ARRAY, payload_len);
    if (rc == X12_OK)
    {
        rc = append_u16(builder, (unsigned short)count);
    }
    for (i = 0u; i < count && rc == X12_OK; i++)
    {
        rc = append_u32(builder, (uint32_t)values[i].len);
        if (rc == X12_OK)
        {
            rc = append_bytes(builder, values[i].ptr, values[i].len);
        }
    }

    return rc;
}

static int write_record_bytes(
    FILE *fp,
    const unsigned char *data,
    size_t len,
    long long *out_offset,
    long long *out_stored_len)
{
    unsigned char encoded_len[4];
    long offset;

    if (fp == NULL || data == NULL || len < 2u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (len > UINT32_MAX)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    encode_u32_le(encoded_len, (uint32_t)len);

    offset = ftell(fp);
    if (offset < 0)
    {
        return X12_ERR_IO;
    }
    if (fwrite(encoded_len, 1u, sizeof(encoded_len), fp) != sizeof(encoded_len))
    {
        return X12_ERR_IO;
    }
    if (fwrite(data, 1u, len, fp) != len)
    {
        return X12_ERR_IO;
    }

    if (out_offset != NULL)
    {
        *out_offset = (long long)offset;
    }
    if (out_stored_len != NULL)
    {
        *out_stored_len = (long long)(sizeof(encoded_len) + len);
    }

    return X12_OK;
}

static size_t pending_dictionary_entry_count(const journal_record_builder_t *builder)
{
    size_t count = 0u;
    size_t i;

    if (builder == NULL)
    {
        return 0u;
    }

    for (i = 0u; i < builder->key_count; i++)
    {
        if (!builder->key_written[i])
        {
            count++;
        }
    }

    return count;
}

static void mark_dictionary_unwritten(journal_record_builder_t *builder)
{
    size_t i;

    if (builder == NULL)
    {
        return;
    }

    for (i = 0u; i < builder->key_count; i++)
    {
        builder->key_written[i] = 0u;
    }
}

static int write_pending_dictionary(FILE *fp, journal_record_builder_t *builder)
{
    journal_record_builder_t dictionary;
    unsigned char record_type = JOURNAL_CONTROL_DICTIONARY;
    size_t pending_count;
    size_t i;
    int rc;

    if (fp == NULL || builder == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    pending_count = pending_dictionary_entry_count(builder);
    if (pending_count == 0u)
    {
        return X12_OK;
    }
    if (pending_count > UINT16_MAX)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    journal_record_builder_init(&dictionary);
    rc = append_u16(&dictionary, 0u);
    if (rc == X12_OK)
    {
        rc = append_bytes(&dictionary, &record_type, sizeof(record_type));
    }
    if (rc == X12_OK)
    {
        rc = append_u16(&dictionary, (unsigned short)pending_count);
    }

    for (i = 0u; i < builder->key_count && rc == X12_OK; i++)
    {
        size_t name_len;

        if (builder->key_written[i])
        {
            continue;
        }

        name_len = strlen(builder->key_names[i]);
        if (name_len == 0u || name_len > UINT16_MAX)
        {
            rc = X12_ERR_BUFFER_TOO_SMALL;
            break;
        }

        rc = append_u16(&dictionary, (unsigned short)(i + 1u));
        if (rc == X12_OK)
        {
            rc = append_u16(&dictionary, (unsigned short)name_len);
        }
        if (rc == X12_OK)
        {
            rc = append_bytes(&dictionary, builder->key_names[i], name_len);
        }
    }

    if (rc == X12_OK)
    {
        rc = write_record_bytes(fp, dictionary.data, dictionary.len, NULL, NULL);
    }
    journal_record_builder_free(&dictionary);
    if (rc != X12_OK)
    {
        return rc;
    }

    for (i = 0u; i < builder->key_count; i++)
    {
        builder->key_written[i] = 1u;
    }
    return X12_OK;
}

int journal_write_record(
    FILE *fp,
    journal_record_builder_t *builder,
    long long *out_offset,
    long long *out_stored_len)
{
    long offset;

    if (fp == NULL || builder == NULL || builder->data == NULL || builder->len < 2u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (builder->field_count == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    offset = ftell(fp);
    if (offset < 0)
    {
        return X12_ERR_IO;
    }
    if (offset == (long)JOURNAL_MAGIC_LEN)
    {
        mark_dictionary_unwritten(builder);
    }

    TRY(write_pending_dictionary(fp, builder));

    encode_u16_le(builder->data, builder->field_count);
    return write_record_bytes(fp, builder->data, builder->len, out_offset, out_stored_len);
}

static int has_suffix(const char *value, const char *suffix)
{
    size_t value_len;
    size_t suffix_len;

    if (value == NULL || suffix == NULL)
    {
        return 0;
    }

    value_len = strlen(value);
    suffix_len = strlen(suffix);
    if (value_len < suffix_len)
    {
        return 0;
    }

    return strcmp(value + value_len - suffix_len, suffix) == 0;
}

static char *dup_cstr(const char *value)
{
    char *copy;
    size_t len;

    if (value == NULL)
    {
        return NULL;
    }

    len = strlen(value);
    copy = (char *)malloc(len + 1u);
    if (copy == NULL)
    {
        return NULL;
    }
    memcpy(copy, value, len + 1u);
    return copy;
}

static int join_path(const char *base, const char *name, char **out)
{
    char *path;
    size_t base_len;
    size_t name_len;
    size_t path_len;
    int needs_separator;

    if (base == NULL || name == NULL || out == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    *out = NULL;
    base_len = strlen(base);
    name_len = strlen(name);
    needs_separator = base_len > 0u &&
                      base[base_len - 1u] != '/' &&
                      base[base_len - 1u] != '\\';
    if (base_len > SIZE_MAX - name_len - (needs_separator ? 2u : 1u))
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    path_len = base_len + name_len + (needs_separator ? 1u : 0u);
    path = (char *)malloc(path_len + 1u);
    if (path == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }

    memcpy(path, base, base_len);
    if (needs_separator)
    {
        path[base_len] = '/';
        memcpy(path + base_len + 1u, name, name_len);
    }
    else
    {
        memcpy(path + base_len, name, name_len);
    }
    path[path_len] = '\0';

    *out = path;
    return X12_OK;
}

static int copy_normalized_path(const char *path, char **out)
{
    char *copy;
    size_t i;
    size_t len;

    if (path == NULL || out == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    *out = NULL;
    len = strlen(path);
    copy = (char *)malloc(len + 1u);
    if (copy == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    for (i = 0u; i < len; i++)
    {
        copy[i] = path[i] == '\\' ? '/' : path[i];
    }
    copy[len] = '\0';

    *out = copy;
    return X12_OK;
}

static int segment_id_under_root(const char *root_path, const char *path, char **out)
{
    const char *rel;
    size_t root_len;

    if (root_path == NULL || path == NULL || out == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    root_len = strlen(root_path);
    if (strncmp(path, root_path, root_len) != 0)
    {
        return copy_normalized_path(path, out);
    }
    if (path[root_len] != '\0' &&
        path[root_len] != '/' &&
        path[root_len] != '\\')
    {
        return copy_normalized_path(path, out);
    }

    rel = path + root_len;
    while (*rel == '/' || *rel == '\\')
    {
        rel++;
    }
    if (*rel == '\0')
    {
        rel = path;
    }

    return copy_normalized_path(rel, out);
}

static int append_segment_path(journal_reader_t *reader, const char *path, const char *segment_id)
{
    char **next;
    char *path_copy;
    char *id_copy;

    if (reader == NULL || path == NULL || segment_id == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    path_copy = dup_cstr(path);
    if (path_copy == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    id_copy = dup_cstr(segment_id);
    if (id_copy == NULL)
    {
        free(path_copy);
        return X12_ERR_NO_MEMORY;
    }

    if (reader->segment_count == SIZE_MAX / sizeof(*reader->segment_paths))
    {
        free(path_copy);
        free(id_copy);
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    next = (char **)realloc(
        reader->segment_paths,
        (reader->segment_count + 1u) * sizeof(*reader->segment_paths));
    if (next == NULL)
    {
        free(path_copy);
        free(id_copy);
        return X12_ERR_NO_MEMORY;
    }
    reader->segment_paths = next;

    next = (char **)realloc(
        reader->segment_ids,
        (reader->segment_count + 1u) * sizeof(*reader->segment_ids));
    if (next == NULL)
    {
        free(path_copy);
        free(id_copy);
        return X12_ERR_NO_MEMORY;
    }
    reader->segment_ids = next;

    reader->segment_paths[reader->segment_count] = path_copy;
    reader->segment_ids[reader->segment_count] = id_copy;
    reader->segment_count++;
    return X12_OK;
}

static void sort_segment_paths(journal_reader_t *reader)
{
    size_t i;
    size_t j;

    if (reader == NULL || reader->segment_count < 2u)
    {
        return;
    }

    for (i = 0u; i + 1u < reader->segment_count; i++)
    {
        for (j = i + 1u; j < reader->segment_count; j++)
        {
            if (strcmp(reader->segment_ids[i], reader->segment_ids[j]) > 0)
            {
                char *tmp_path = reader->segment_paths[i];
                char *tmp_id = reader->segment_ids[i];

                reader->segment_paths[i] = reader->segment_paths[j];
                reader->segment_ids[i] = reader->segment_ids[j];
                reader->segment_paths[j] = tmp_path;
                reader->segment_ids[j] = tmp_id;
            }
        }
    }
}

static int path_is_directory(const char *path)
{
#ifdef _WIN32
    DWORD attrs;

    if (path == NULL)
    {
        return 0;
    }

    attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0u;
#else
    struct stat st;

    if (path == NULL)
    {
        return 0;
    }

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int path_is_regular_file(const char *path)
{
#ifdef _WIN32
    DWORD attrs;

    if (path == NULL)
    {
        return 0;
    }

    attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u;
#else
    struct stat st;

    if (path == NULL)
    {
        return 0;
    }

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static int segment_file_is_supported(const char *path)
{
    return has_suffix(path, ".journal") || has_suffix(path, ".journal.zst");
}

static int segment_file_is_compressed(const char *path)
{
    return has_suffix(path, ".zst");
}

static void journal_reader_clear_zstd_stream(journal_reader_t *reader)
{
    if (reader == NULL)
    {
        return;
    }

    if (reader->zstd_stream != NULL)
    {
        ZSTD_freeDStream((ZSTD_DStream *)reader->zstd_stream);
        reader->zstd_stream = NULL;
    }
    reader->zstd_in_pos = 0u;
    reader->zstd_in_len = 0u;
    reader->zstd_out_pos = 0u;
    reader->zstd_out_len = 0u;
    reader->zstd_input_eof = 0;
}

static void journal_reader_free_zstd(journal_reader_t *reader)
{
    if (reader == NULL)
    {
        return;
    }

    journal_reader_clear_zstd_stream(reader);
    free(reader->zstd_in);
    free(reader->zstd_out);
    reader->zstd_in = NULL;
    reader->zstd_out = NULL;
    reader->zstd_in_cap = 0u;
    reader->zstd_out_cap = 0u;
}

static int journal_reader_prepare_zstd(journal_reader_t *reader)
{
    size_t in_cap;
    size_t out_cap;
    size_t zstd_rc;

    if (reader == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    reader->zstd_stream = ZSTD_createDStream();
    if (reader->zstd_stream == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    zstd_rc = ZSTD_initDStream((ZSTD_DStream *)reader->zstd_stream);
    if (ZSTD_isError(zstd_rc))
    {
        journal_reader_clear_zstd_stream(reader);
        return X12_ERR_IO;
    }

    in_cap = ZSTD_DStreamInSize();
    out_cap = ZSTD_DStreamOutSize();
    if (reader->zstd_in_cap < in_cap)
    {
        unsigned char *next = (unsigned char *)realloc(reader->zstd_in, in_cap);
        if (next == NULL)
        {
            journal_reader_clear_zstd_stream(reader);
            return X12_ERR_NO_MEMORY;
        }
        reader->zstd_in = next;
        reader->zstd_in_cap = in_cap;
    }
    if (reader->zstd_out_cap < out_cap)
    {
        unsigned char *next = (unsigned char *)realloc(reader->zstd_out, out_cap);
        if (next == NULL)
        {
            journal_reader_clear_zstd_stream(reader);
            return X12_ERR_NO_MEMORY;
        }
        reader->zstd_out = next;
        reader->zstd_out_cap = out_cap;
    }
    reader->zstd_in_pos = 0u;
    reader->zstd_in_len = 0u;
    reader->zstd_out_pos = 0u;
    reader->zstd_out_len = 0u;
    reader->zstd_input_eof = 0;
    return X12_OK;
}

static int journal_reader_read_some(
    journal_reader_t *reader,
    unsigned char *out,
    size_t len,
    size_t *bytes_read)
{
    size_t copied = 0u;

    if (reader == NULL || out == NULL || bytes_read == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    *bytes_read = 0u;
    if (len == 0u)
    {
        return X12_OK;
    }

    if (!reader->current_segment_compressed)
    {
        copied = fread(out, 1u, len, reader->fp);
        if (copied > 0u)
        {
            reader->logical_offset += (long long)copied;
        }
        *bytes_read = copied;
        if (copied < len && ferror(reader->fp))
        {
            return X12_ERR_IO;
        }
        return X12_OK;
    }

    while (copied < len)
    {
        size_t available;
        size_t need;

        if (reader->zstd_out_pos < reader->zstd_out_len)
        {
            available = reader->zstd_out_len - reader->zstd_out_pos;
            need = len - copied;
            if (available > need)
            {
                available = need;
            }
            memcpy(out + copied, reader->zstd_out + reader->zstd_out_pos, available);
            reader->zstd_out_pos += available;
            copied += available;
            reader->logical_offset += (long long)available;
            continue;
        }

        reader->zstd_out_pos = 0u;
        reader->zstd_out_len = 0u;

        if (reader->zstd_in_pos >= reader->zstd_in_len && !reader->zstd_input_eof)
        {
            reader->zstd_in_len = fread(reader->zstd_in, 1u, reader->zstd_in_cap, reader->fp);
            reader->zstd_in_pos = 0u;
            if (reader->zstd_in_len == 0u)
            {
                if (ferror(reader->fp))
                {
                    return X12_ERR_IO;
                }
                reader->zstd_input_eof = 1;
            }
        }

        if (reader->zstd_input_eof && reader->zstd_in_pos >= reader->zstd_in_len)
        {
            break;
        }

        {
            ZSTD_inBuffer input = {
                reader->zstd_in,
                reader->zstd_in_len,
                reader->zstd_in_pos};
            ZSTD_outBuffer output = {
                reader->zstd_out,
                reader->zstd_out_cap,
                0u};
            size_t zstd_rc = ZSTD_decompressStream(
                (ZSTD_DStream *)reader->zstd_stream,
                &output,
                &input);
            if (ZSTD_isError(zstd_rc))
            {
                return X12_ERR_IO;
            }
            reader->zstd_in_pos = input.pos;
            reader->zstd_out_len = output.pos;
        }

        if (reader->zstd_out_len == 0u &&
            reader->zstd_in_pos >= reader->zstd_in_len &&
            reader->zstd_input_eof)
        {
            break;
        }
    }

    *bytes_read = copied;
    return X12_OK;
}

static int journal_reader_read_exact(journal_reader_t *reader, unsigned char *out, size_t len)
{
    size_t total = 0u;

    if (reader == NULL || out == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    while (total < len)
    {
        size_t bytes_read = 0u;
        int rc = journal_reader_read_some(reader, out + total, len - total, &bytes_read);

        if (rc != X12_OK)
        {
            return rc;
        }
        if (bytes_read == 0u)
        {
            return X12_ERR_IO;
        }
        total += bytes_read;
    }

    return X12_OK;
}

static int journal_reader_read_header(journal_reader_t *reader)
{
    unsigned char magic[JOURNAL_MAGIC_LEN];

    if (reader == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (journal_reader_read_exact(reader, magic, sizeof(magic)) != X12_OK)
    {
        return X12_ERR_IO;
    }
    return memcmp(magic, JOURNAL_MAGIC, JOURNAL_MAGIC_LEN) == 0 ? X12_OK : X12_ERR_UNSUPPORTED;
}

static int journal_reader_apply_control_record(
    journal_reader_t *reader,
    const unsigned char *cursor,
    const unsigned char *end)
{
    unsigned char record_type;
    unsigned short count;
    size_t i;
    int rc;

    if (reader == NULL || cursor == NULL || end == NULL || cursor >= end)
    {
        return X12_ERR_IO;
    }

    record_type = *cursor++;
    if (record_type != JOURNAL_CONTROL_DICTIONARY)
    {
        return X12_ERR_UNSUPPORTED;
    }
    if ((size_t)(end - cursor) < 2u)
    {
        return X12_ERR_IO;
    }

    count = decode_u16_le(cursor);
    cursor += 2u;

    for (i = 0u; i < count; i++)
    {
        unsigned short key_id;
        unsigned short name_len;

        if ((size_t)(end - cursor) < 4u)
        {
            return X12_ERR_IO;
        }
        key_id = decode_u16_le(cursor);
        cursor += 2u;
        name_len = decode_u16_le(cursor);
        cursor += 2u;
        if (name_len == 0u || (size_t)(end - cursor) < name_len)
        {
            return X12_ERR_IO;
        }

        rc = reader_dictionary_set(reader, key_id, cursor, name_len);
        if (rc != X12_OK)
        {
            return rc;
        }
        cursor += name_len;
    }

    return cursor == end ? X12_OK : X12_ERR_IO;
}

#ifdef _WIN32
static int scan_segment_dir(journal_reader_t *reader, const char *root_path, const char *dir_path)
{
    WIN32_FIND_DATAA data;
    HANDLE handle;
    char *pattern = NULL;
    int rc;

    TRY(join_path(dir_path, "*", &pattern));

    handle = FindFirstFileA(pattern, &data);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return X12_ERR_IO;
    }

    do
    {
        char *child_path;

        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0)
        {
            continue;
        }

        rc = join_path(dir_path, data.cFileName, &child_path);
        if (rc != X12_OK)
        {
            FindClose(handle);
            return rc;
        }

        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u)
        {
            rc = scan_segment_dir(reader, root_path, child_path);
        }
        else if (segment_file_is_supported(data.cFileName))
        {
            char *segment_id = NULL;

            rc = segment_id_under_root(root_path, child_path, &segment_id);
            if (rc == X12_OK)
            {
                rc = append_segment_path(reader, child_path, segment_id);
            }
            free(segment_id);
        }
        free(child_path);
        if (rc != X12_OK)
        {
            FindClose(handle);
            return rc;
        }
    } while (FindNextFileA(handle, &data) != 0);

    FindClose(handle);
    return X12_OK;
}
#else
static int scan_segment_dir(journal_reader_t *reader, const char *root_path, const char *dir_path)
{
    DIR *dir;
    struct dirent *entry;
    int rc = X12_OK;

    dir = opendir(dir_path);
    if (dir == NULL)
    {
        return X12_ERR_IO;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        char *child_path;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        rc = join_path(dir_path, entry->d_name, &child_path);
        if (rc != X12_OK)
        {
            break;
        }

        if (path_is_directory(child_path))
        {
            rc = scan_segment_dir(reader, root_path, child_path);
        }
        else if (path_is_regular_file(child_path) &&
                 segment_file_is_supported(entry->d_name))
        {
            char *segment_id = NULL;

            rc = segment_id_under_root(root_path, child_path, &segment_id);
            if (rc == X12_OK)
            {
                rc = append_segment_path(reader, child_path, segment_id);
            }
            free(segment_id);
        }
        free(child_path);
        if (rc != X12_OK)
        {
            break;
        }
    }

    if (closedir(dir) != 0 && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }
    return rc;
}
#endif

static void free_segment_paths(journal_reader_t *reader)
{
    size_t i;

    if (reader == NULL)
    {
        return;
    }

    for (i = 0u; i < reader->segment_count; i++)
    {
        free(reader->segment_paths[i]);
        free(reader->segment_ids[i]);
    }
    free(reader->segment_paths);
    free(reader->segment_ids);
    reader->segment_paths = NULL;
    reader->segment_ids = NULL;
    reader->segment_count = 0u;
    reader->segment_index = 0u;
    reader->current_segment_path = NULL;
    reader->current_segment_id = NULL;
    reader->current_segment_compressed = 0;
    reader->logical_offset = 0;
}

static int journal_reader_open_next_segment(journal_reader_t *reader)
{
    int rc;

    if (reader == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (reader->fp != NULL)
    {
        if (fclose(reader->fp) != 0)
        {
            reader->fp = NULL;
            journal_reader_clear_zstd_stream(reader);
            return X12_ERR_IO;
        }
        reader->fp = NULL;
    }
    journal_reader_clear_zstd_stream(reader);
    reader->current_segment_path = NULL;
    reader->current_segment_id = NULL;
    reader->current_segment_compressed = 0;
    reader->logical_offset = 0;
    clear_reader_dictionary(reader);
    clear_reader_context(reader);

    if (reader->segment_index >= reader->segment_count)
    {
        return X12_OK;
    }

    reader->current_segment_path = reader->segment_paths[reader->segment_index];
    reader->current_segment_id = reader->segment_ids[reader->segment_index];
    reader->current_segment_compressed =
        segment_file_is_compressed(reader->current_segment_path);
    reader->segment_index++;
    reader->fp = fopen(reader->current_segment_path, "rb");
    if (reader->fp == NULL)
    {
        reader->current_segment_path = NULL;
        reader->current_segment_id = NULL;
        reader->current_segment_compressed = 0;
        return X12_ERR_IO;
    }

    if (reader->current_segment_compressed)
    {
        rc = journal_reader_prepare_zstd(reader);
        if (rc != X12_OK)
        {
            (void)fclose(reader->fp);
            reader->fp = NULL;
            reader->current_segment_path = NULL;
            reader->current_segment_id = NULL;
            reader->current_segment_compressed = 0;
            return rc;
        }
    }

    rc = journal_reader_read_header(reader);
    if (rc != X12_OK)
    {
        (void)fclose(reader->fp);
        reader->fp = NULL;
        journal_reader_clear_zstd_stream(reader);
        reader->current_segment_path = NULL;
        reader->current_segment_id = NULL;
        reader->current_segment_compressed = 0;
        return rc;
    }

    return X12_OK;
}

void journal_reader_init(journal_reader_t *reader)
{
    if (reader != NULL)
    {
        reader->fp = NULL;
        reader->buffer = NULL;
        reader->buffer_cap = 0u;
        reader->segment_paths = NULL;
        reader->segment_ids = NULL;
        reader->segment_count = 0u;
        reader->segment_index = 0u;
        reader->current_segment_path = NULL;
        reader->current_segment_id = NULL;
        reader->current_segment_compressed = 0;
        reader->zstd_stream = NULL;
        reader->zstd_in = NULL;
        reader->zstd_in_cap = 0u;
        reader->zstd_in_pos = 0u;
        reader->zstd_in_len = 0u;
        reader->zstd_out = NULL;
        reader->zstd_out_cap = 0u;
        reader->zstd_out_pos = 0u;
        reader->zstd_out_len = 0u;
        reader->zstd_input_eof = 0;
        reader->logical_offset = 0;
        reader->context_source_file = NULL;
        reader->context_source_transaction = NULL;
        reader->context_source_drop_id = NULL;
        reader->context_run_id = NULL;
        reader->context_isa13 = NULL;
        reader->context_gs06 = NULL;
        reader->context_st02 = NULL;
        reader->dictionary_names = NULL;
        reader->dictionary_count = 0u;
        reader->dictionary_cap = 0u;
    }
}

int journal_reader_open(journal_reader_t *reader, const char *path)
{
    int rc;

    if (reader == NULL || path == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    journal_reader_init(reader);
    if (path_is_directory(path))
    {
        rc = scan_segment_dir(reader, path, path);
        if (rc != X12_OK)
        {
            journal_reader_close(reader);
            return rc;
        }
        sort_segment_paths(reader);
        if (reader->segment_count == 0u)
        {
            journal_reader_close(reader);
            return X12_ERR_IO;
        }
    }
    else
    {
        rc = append_segment_path(reader, path, path);
        if (rc != X12_OK)
        {
            journal_reader_close(reader);
            return rc;
        }
    }

    rc = journal_reader_open_next_segment(reader);
    if (rc != X12_OK)
    {
        (void)journal_reader_close(reader);
        return rc;
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
    long long record_offset;
    int rc;

    if (reader == NULL || reader->fp == NULL || out == NULL)
    {
        if (reader != NULL && reader->fp == NULL && out != NULL &&
            reader->segment_index >= reader->segment_count)
        {
            memset(out, 0, sizeof(*out));
            return X12_OK;
        }
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));

    while (reader->fp != NULL)
    {
        record_offset = reader->logical_offset;

        bytes_read = 0u;
        rc = journal_reader_read_some(
            reader,
            encoded_len,
            sizeof(encoded_len),
            &bytes_read);
        if (rc != X12_OK)
        {
            return rc;
        }
        if (bytes_read == 0u)
        {
            int next_rc;

            next_rc = journal_reader_open_next_segment(reader);
            if (next_rc != X12_OK)
            {
                return next_rc;
            }
            if (reader->fp == NULL)
            {
                return X12_OK;
            }
            continue;
        }
        if (bytes_read != sizeof(encoded_len))
        {
            return X12_ERR_IO;
        }

        len = decode_u32_le(encoded_len);
        if (len < 2u || len > JOURNAL_MAX_RECORD_LEN)
        {
            return X12_ERR_IO;
        }

        if ((size_t)len > reader->buffer_cap)
        {
            unsigned char *next = (unsigned char *)realloc(reader->buffer, (size_t)len);
            if (next == NULL)
            {
                return X12_ERR_NO_MEMORY;
            }
            reader->buffer = next;
            reader->buffer_cap = (size_t)len;
        }

        rc = journal_reader_read_exact(reader, reader->buffer, (size_t)len);
        if (rc != X12_OK)
        {
            return rc;
        }

        cursor = reader->buffer;
        end = reader->buffer + len;
        field_count = decode_u16_le(cursor);
        cursor += 2u;
        if (field_count == 0u)
        {
            TRY(journal_reader_apply_control_record(reader, cursor, end));
            continue;
        }
        if (field_count > JOURNAL_EVENT_MAX_FIELDS)
        {
            return X12_ERR_BUFFER_TOO_SMALL;
        }

        out->record = reader->buffer;
        out->record_len = (size_t)len;
        out->segment_path = reader->current_segment_id;
        out->offset = (long long)record_offset;
        out->stored_len = (long long)(JOURNAL_LEN_SIZE + (size_t)len);
        out->field_count = field_count;

        for (i = 0u; i < field_count; i++)
        {
            uint32_t value_len;

            if ((size_t)(end - cursor) < 7u)
            {
                return X12_ERR_IO;
            }
            out->fields[i].key_id = decode_u16_le(cursor);
            cursor += 2u;
            out->fields[i].key_name = reader_dictionary_name(reader, out->fields[i].key_id);
            if (out->fields[i].key_name == NULL)
            {
                return X12_ERR_IO;
            }
            out->fields[i].type = *cursor++;
            value_len = decode_u32_le(cursor);
            cursor += 4u;
            if ((size_t)(end - cursor) < value_len)
            {
                return X12_ERR_IO;
            }
            out->fields[i].data = cursor;
            out->fields[i].len = value_len;
            cursor += value_len;
        }
        if (cursor != end)
        {
            return X12_ERR_IO;
        }

        TRY(update_reader_context(reader, out));

        return X12_OK;
    }

    return X12_OK;
}

int journal_reader_close(journal_reader_t *reader)
{
    int rc = X12_OK;

    if (reader == NULL)
    {
        return X12_OK;
    }

    if (reader->fp != NULL && fclose(reader->fp) != 0)
    {
        rc = X12_ERR_IO;
    }
    free(reader->buffer);
    journal_reader_free_zstd(reader);
    clear_reader_dictionary(reader);
    free_segment_paths(reader);
    clear_reader_context(reader);
    journal_reader_init(reader);
    return rc;
}

int journal_event_get_string(
    const journal_event_t *event,
    const char *key,
    char *out,
    size_t out_len)
{
    const journal_event_field_t *field = find_field(event, key);

    if (field == NULL)
    {
        return copy_context_string(event_context_string(event, key), out, out_len);
    }
    if (field->type != JOURNAL_VALUE_STRING)
    {
        if (out != NULL && out_len > 0u)
        {
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
        field->len != 1u)
    {
        return 0;
    }

    *out = field->data[0] != 0u;
    return 1;
}

int journal_event_get_number_text(
    const journal_event_t *event,
    const char *key,
    char *out,
    size_t out_len)
{
    const journal_event_field_t *field = find_field(event, key);
    unsigned long long value;
    int written;

    if (out == NULL || out_len == 0u)
    {
        return 0;
    }
    out[0] = '\0';
    if (field == NULL)
    {
        return 0;
    }
    if (field->type == JOURNAL_VALUE_STRING)
    {
        return copy_field_string(field, out, out_len);
    }
    if (field->type != JOURNAL_VALUE_U64 || field->len != 8u)
    {
        return 0;
    }

    value = decode_u64_le(field->data);
    written = snprintf(out, out_len, "%llu", value);
    if (written < 0 || (size_t)written >= out_len)
    {
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
    size_t out_len)
{
    const journal_event_field_t *field = find_field(event, key);
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned short count;
    size_t i;

    if (out == NULL || out_len == 0u)
    {
        return 0;
    }
    out[0] = '\0';

    if (field == NULL || field->type != JOURNAL_VALUE_STRING_ARRAY || field->len < 2u)
    {
        return 0;
    }

    cursor = field->data;
    end = field->data + field->len;
    count = decode_u16_le(cursor);
    cursor += 2u;

    if (index >= count)
    {
        return 0;
    }

    for (i = 0u; i < count; i++)
    {
        uint32_t len;

        if ((size_t)(end - cursor) < 4u)
        {
            return 0;
        }
        len = decode_u32_le(cursor);
        cursor += 4u;
        if ((size_t)(end - cursor) < len)
        {
            return 0;
        }
        if (i == index)
        {
            if ((size_t)len >= out_len)
            {
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
