#include "event_writer.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static x12_str_t empty_str(void)
{
    x12_str_t value;

    value.ptr = "";
    value.len = 0;
    return value;
}

static int check_file(FILE *fp)
{
    return ferror(fp) ? X12_ERR_IO : X12_OK;
}

static int write_char(FILE *fp, char value)
{
    return fputc((unsigned char)value, fp) == EOF ? X12_ERR_IO : X12_OK;
}

static int write_hex_escape(FILE *fp, unsigned char value)
{
    static const char digits[] = "0123456789abcdef";

    if (fputs("\\u00", fp) == EOF)
    {
        return X12_ERR_IO;
    }
    if (write_char(fp, digits[(value >> 4u) & 0x0fu]) != X12_OK)
    {
        return X12_ERR_IO;
    }
    return write_char(fp, digits[value & 0x0fu]);
}

int event_writer_write_json_string(FILE *fp, const char *data, size_t len)
{
    size_t i;

    if (fp == NULL || data == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (write_char(fp, '"') != X12_OK)
    {
        return X12_ERR_IO;
    }

    for (i = 0; i < len; i++)
    {
        unsigned char ch = (unsigned char)data[i];

        switch (ch)
        {
        case '"':
            if (fputs("\\\"", fp) == EOF)
            {
                return X12_ERR_IO;
            }
            break;
        case '\\':
            if (fputs("\\\\", fp) == EOF)
            {
                return X12_ERR_IO;
            }
            break;
        case '\n':
            if (fputs("\\n", fp) == EOF)
            {
                return X12_ERR_IO;
            }
            break;
        case '\r':
            if (fputs("\\r", fp) == EOF)
            {
                return X12_ERR_IO;
            }
            break;
        case '\t':
            if (fputs("\\t", fp) == EOF)
            {
                return X12_ERR_IO;
            }
            break;
        default:
            if (ch < 0x20u)
            {
                if (write_hex_escape(fp, ch) != X12_OK)
                {
                    return X12_ERR_IO;
                }
            }
            else if (write_char(fp, (char)ch) != X12_OK)
            {
                return X12_ERR_IO;
            }
            break;
        }
    }

    if (write_char(fp, '"') != X12_OK)
    {
        return X12_ERR_IO;
    }

    return check_file(fp);
}

static int build_source_drop_id(
    const event_writer_t *writer,
    char *out,
    size_t out_len)
{
    int written;

    if (writer == NULL || out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    out[0] = '\0';
    if (writer->isa13.len == 0u ||
        writer->gs06.len == 0u ||
        writer->st02.len == 0u)
    {
        return X12_OK;
    }
    if (writer->isa13.len > INT_MAX ||
        writer->gs06.len > INT_MAX ||
        writer->st02.len > INT_MAX)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    written = snprintf(
        out,
        out_len,
        "%s:%.*s:%.*s:%.*s",
        writer->source_transaction,
        (int)writer->isa13.len,
        writer->isa13.ptr,
        (int)writer->gs06.len,
        writer->gs06.ptr,
        (int)writer->st02.len,
        writer->st02.ptr);
    if (written < 0 || (size_t)written >= out_len)
    {
        out[0] = '\0';
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return X12_OK;
}

static void writer_reset_fields(event_writer_t *writer)
{
    writer->source_file = NULL;
    writer->source_transaction = NULL;
    writer->run_id = NULL;
    writer->isa13 = empty_str();
    writer->gs06 = empty_str();
    writer->st02 = empty_str();
    writer->include_phi = 0;
    writer->phi_vault = NULL;
    writer->current_source_drop_id[0] = '\0';
    writer->mode = EVENT_WRITER_MODE_JSON;
    writer->journal_context_written = 0;
    writer->journal_context_source_drop_id[0] = '\0';
    writer->builder_open = 0;
}

int event_writer_open(
    event_writer_t *writer,
    const char *out_path,
    const char *source_file,
    const char *source_transaction)
{
    if (writer == NULL || source_file == NULL || source_transaction == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(writer, 0, sizeof(*writer));
    writer_reset_fields(writer);
    writer->source_file = source_file;
    writer->source_transaction = source_transaction;
    journal_record_builder_init(&writer->builder);

    if (out_path == NULL || strcmp(out_path, "-") == 0)
    {
        writer->fp = stdout;
        writer->owns_file = 0;
    }
    else
    {
        writer->fp = fopen(out_path, "wb");
        writer->owns_file = 1;
    }

    if (writer->fp == NULL)
    {
        return X12_ERR_IO;
    }

    return X12_OK;
}

int event_writer_open_stream(
    event_writer_t *writer,
    FILE *fp,
    const char *source_file,
    const char *source_transaction)
{
    if (writer == NULL || fp == NULL || source_file == NULL || source_transaction == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(writer, 0, sizeof(*writer));
    writer_reset_fields(writer);
    writer->fp = fp;
    writer->owns_file = 0;
    writer->source_file = source_file;
    writer->source_transaction = source_transaction;
    journal_record_builder_init(&writer->builder);

    return X12_OK;
}

void event_writer_set_include_phi(event_writer_t *writer, int include_phi)
{
    if (writer != NULL)
    {
        writer->include_phi = include_phi ? 1 : 0;
    }
}

int event_writer_include_phi(const event_writer_t *writer)
{
    return writer == NULL ? 0 : writer->include_phi;
}

void event_writer_set_run_id(event_writer_t *writer, const char *run_id)
{
    if (writer != NULL)
    {
        writer->run_id = run_id;
    }
}

int event_writer_set_mode(event_writer_t *writer, event_writer_mode_t mode)
{
    if (writer == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    writer->mode = mode;
    return X12_OK;
}

void event_writer_set_phi_vault(event_writer_t *writer, phi_vault_t *vault)
{
    if (writer != NULL)
    {
        writer->phi_vault = vault;
    }
}

int event_writer_record_phi_mapping(
    event_writer_t *writer,
    token_type_t type,
    x12_str_t raw)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    int rc;

    if (writer == NULL || writer->phi_vault == NULL || raw.ptr == NULL || raw.len == 0u)
    {
        return X12_OK;
    }
    if (writer->current_source_drop_id[0] == '\0')
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = tokenise_value(type, raw, token, sizeof(token));
    if (rc != X12_OK)
    {
        return rc;
    }

    return phi_vault_put_mapping(
        writer->phi_vault,
        tokenise_namespace(type),
        token,
        raw,
        writer->current_source_drop_id);
}

int event_writer_record_phi_name(
    event_writer_t *writer,
    token_type_t name_type,
    x12_str_t last_name_or_org,
    x12_str_t first_name,
    token_type_t id_type,
    x12_str_t id_raw)
{
    char name_raw_buf[512];
    char name_token[TOKENISE_MAX_TOKEN_LEN];
    char id_token[TOKENISE_MAX_TOKEN_LEN];
    char id_name_namespace[128];
    x12_str_t name_raw;
    int written;
    int rc;

    if (writer == NULL || writer->phi_vault == NULL ||
        last_name_or_org.ptr == NULL || last_name_or_org.len == 0u)
    {
        return X12_OK;
    }
    if (writer->current_source_drop_id[0] == '\0')
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (first_name.ptr != NULL && first_name.len > 0u)
    {
        written = snprintf(
            name_raw_buf, sizeof(name_raw_buf),
            "%.*s|%.*s",
            (int)last_name_or_org.len, last_name_or_org.ptr,
            (int)first_name.len, first_name.ptr);
    }
    else
    {
        written = snprintf(
            name_raw_buf, sizeof(name_raw_buf),
            "%.*s",
            (int)last_name_or_org.len, last_name_or_org.ptr);
    }
    if (written < 0 || (size_t)written >= sizeof(name_raw_buf))
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    name_raw.ptr = name_raw_buf;
    name_raw.len = (size_t)written;

    rc = tokenise_value(name_type, name_raw, name_token, sizeof(name_token));
    if (rc != X12_OK)
    {
        return rc;
    }
    rc = phi_vault_put_mapping(
        writer->phi_vault,
        tokenise_namespace(name_type),
        name_token,
        name_raw,
        writer->current_source_drop_id);
    if (rc != X12_OK)
    {
        return rc;
    }

    if (id_raw.ptr == NULL || id_raw.len == 0u || id_type == TOK_UNKNOWN)
    {
        return X12_OK;
    }

    rc = tokenise_value(id_type, id_raw, id_token, sizeof(id_token));
    if (rc != X12_OK)
    {
        return rc;
    }
    written = snprintf(
        id_name_namespace, sizeof(id_name_namespace),
        "%s_name",
        tokenise_namespace(id_type));
    if (written < 0 || (size_t)written >= sizeof(id_name_namespace))
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return phi_vault_put_mapping(
        writer->phi_vault,
        id_name_namespace,
        id_token,
        name_raw,
        writer->current_source_drop_id);
}

void event_writer_observe_control(
    event_writer_t *writer,
    const x12_segment_t *seg)
{
    if (writer == NULL || seg == NULL)
    {
        return;
    }

    if (x12_str_eq_cstr(seg->tag, "ISA") && seg->element_count > 12u)
    {
        writer->isa13 = seg->elements[12];
    }
    else if (x12_str_eq_cstr(seg->tag, "GS") && seg->element_count > 5u)
    {
        writer->gs06 = seg->elements[5];
    }
    else if (x12_str_eq_cstr(seg->tag, "ST") && seg->element_count > 1u)
    {
        writer->st02 = seg->elements[1];
    }
}

FILE *event_writer_underlying_file(event_writer_t *writer)
{
    return writer == NULL ? NULL : writer->fp;
}

int event_writer_close(event_writer_t *writer)
{
    int rc = X12_OK;

    if (writer == NULL)
    {
        return X12_OK;
    }

    if (writer->fp != NULL && fflush(writer->fp) != 0)
    {
        rc = X12_ERR_IO;
    }
    if (writer->owns_file && writer->fp != NULL && fclose(writer->fp) != 0)
    {
        rc = X12_ERR_IO;
    }
    journal_record_builder_free(&writer->builder);

    writer->fp = NULL;
    writer->owns_file = 0;
    writer->builder_open = 0;
    writer->journal_context_written = 0;
    writer->journal_context_source_drop_id[0] = '\0';

    return rc;
}

int event_writer_begin_event(
    event_writer_t *writer,
    const char *event_type,
    const x12_segment_t *seg)
{
    char source_drop_id[EVENT_WRITER_SOURCE_DROP_ID_MAX];
    int write_context;
    int rc;

    if (writer == NULL || writer->fp == NULL || event_type == NULL || seg == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = build_source_drop_id(writer, source_drop_id, sizeof(source_drop_id));
    if (rc != X12_OK)
    {
        return rc;
    }
    (void)snprintf(
        writer->current_source_drop_id,
        sizeof(writer->current_source_drop_id),
        "%s",
        source_drop_id);

    rc = journal_record_builder_reset(&writer->builder);
    if (rc != X12_OK)
    {
        return rc;
    }
    writer->builder_open = 1;

    rc = journal_record_add_cstring(&writer->builder, "event_type", event_type);
    if (rc != X12_OK)
    {
        return rc;
    }

    /*
     * In JSON mode every event carries the full context block. In journal
     * mode the per-drop fields are written only when the drop changes; later
     * events in the same drop omit them and a reader replays the most recent
     * value from its in-memory context.
     */
    write_context = writer->mode == EVENT_WRITER_MODE_JSON ||
                    !writer->journal_context_written ||
                    strcmp(writer->journal_context_source_drop_id, source_drop_id) != 0;

    if (write_context)
    {
        rc = journal_record_add_cstring(&writer->builder, "source_file", writer->source_file);
        if (rc == X12_OK)
        {
            rc = journal_record_add_cstring(
                &writer->builder, "source_transaction", writer->source_transaction);
        }
        if (rc == X12_OK && source_drop_id[0] != '\0')
        {
            rc = journal_record_add_cstring(&writer->builder, "source_drop_id", source_drop_id);
        }
        if (rc == X12_OK && writer->run_id != NULL && writer->run_id[0] != '\0')
        {
            rc = journal_record_add_cstring(&writer->builder, "run_id", writer->run_id);
        }
    }

    if (rc == X12_OK)
    {
        rc = journal_record_add_u64(
            &writer->builder, "source_segment_index", (unsigned long long)seg->segment_index);
    }
    if (rc == X12_OK)
    {
        rc = journal_record_add_u64(
            &writer->builder, "source_byte_offset", (unsigned long long)seg->byte_offset);
    }

    if (rc == X12_OK && write_context)
    {
        rc = journal_record_add_string(
            &writer->builder, "isa13", writer->isa13.ptr, writer->isa13.len);
        if (rc == X12_OK)
        {
            rc = journal_record_add_string(
                &writer->builder, "gs06", writer->gs06.ptr, writer->gs06.len);
        }
        if (rc == X12_OK)
        {
            rc = journal_record_add_string(
                &writer->builder, "st02", writer->st02.ptr, writer->st02.len);
        }
    }

    if (rc == X12_OK && write_context && writer->mode == EVENT_WRITER_MODE_JOURNAL)
    {
        (void)snprintf(
            writer->journal_context_source_drop_id,
            sizeof(writer->journal_context_source_drop_id),
            "%s",
            source_drop_id);
        writer->journal_context_written = 1;
    }

    return rc;
}

int event_writer_add_str(event_writer_t *writer, const char *name, x12_str_t value)
{
    if (writer == NULL || !writer->builder_open)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    return journal_record_add_string(&writer->builder, name, value.ptr, value.len);
}

int event_writer_add_cstr(event_writer_t *writer, const char *name, const char *value)
{
    if (writer == NULL || !writer->builder_open)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    return journal_record_add_cstring(&writer->builder, name, value);
}

int event_writer_add_bool(event_writer_t *writer, const char *name, int value)
{
    if (writer == NULL || !writer->builder_open)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    return journal_record_add_bool(&writer->builder, name, value);
}

int event_writer_add_str_array(
    event_writer_t *writer,
    const char *name,
    const x12_str_t *values,
    size_t count)
{
    if (writer == NULL || !writer->builder_open)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    return journal_record_add_string_array(&writer->builder, name, values, count);
}

/*
 * The journal_record_builder_t buffer layout is documented in journal.c:
 * [field_count u16] then per field [key_id u16][type u8][value_len u32][value].
 * For JSON emit we walk that buffer and translate it. The builder retains the
 * key names in builder->key_names indexed by (key_id - 1).
 */
static int emit_json_value(FILE *fp, unsigned char type, const unsigned char *data, uint32_t len)
{
    if (type == JOURNAL_VALUE_STRING)
    {
        return event_writer_write_json_string(fp, (const char *)data, (size_t)len);
    }
    if (type == JOURNAL_VALUE_BOOL)
    {
        if (len != 1u)
        {
            return X12_ERR_IO;
        }
        return fputs(data[0] != 0u ? "true" : "false", fp) == EOF ? X12_ERR_IO : X12_OK;
    }
    if (type == JOURNAL_VALUE_U64)
    {
        unsigned long long value = 0ull;
        size_t i;

        if (len != 8u)
        {
            return X12_ERR_IO;
        }
        for (i = 0u; i < 8u; i++)
        {
            value |= ((unsigned long long)data[i]) << (i * 8u);
        }
        return fprintf(fp, "%llu", value) < 0 ? X12_ERR_IO : X12_OK;
    }
    if (type == JOURNAL_VALUE_STRING_ARRAY)
    {
        const unsigned char *cursor = data;
        const unsigned char *end = data + len;
        unsigned short count;
        size_t i;

        if (len < 2u)
        {
            return X12_ERR_IO;
        }
        count = (unsigned short)(cursor[0] | (cursor[1] << 8u));
        cursor += 2u;

        if (fputc('[', fp) == EOF)
        {
            return X12_ERR_IO;
        }
        for (i = 0u; i < count; i++)
        {
            uint32_t item_len;
            int rc;

            if ((size_t)(end - cursor) < 4u)
            {
                return X12_ERR_IO;
            }
            item_len = (uint32_t)cursor[0] |
                       ((uint32_t)cursor[1] << 8u) |
                       ((uint32_t)cursor[2] << 16u) |
                       ((uint32_t)cursor[3] << 24u);
            cursor += 4u;
            if ((size_t)(end - cursor) < item_len)
            {
                return X12_ERR_IO;
            }
            if (i > 0u && fputc(',', fp) == EOF)
            {
                return X12_ERR_IO;
            }
            rc = event_writer_write_json_string(fp, (const char *)cursor, (size_t)item_len);
            if (rc != X12_OK)
            {
                return rc;
            }
            cursor += item_len;
        }
        return fputc(']', fp) == EOF ? X12_ERR_IO : X12_OK;
    }

    return X12_ERR_IO;
}

/*
 * The JSON wire layout the mappers used to write by hand:
 *   {event_type, source_file, source_transaction, [source_drop_id],
 *    [run_id], source_segment_index, source_byte_offset,
 *    "control": {isa13, gs06, st02},
 *    "payload": {<event fields>}}
 *
 * The builder buffer contains all those fields flat (event_type ... st02 ...
 * then payload fields). We split it into the framing fields, the "control"
 * subobject, and the "payload" subobject by name.
 */
static int field_is_control(const char *name)
{
    return strcmp(name, "isa13") == 0 ||
           strcmp(name, "gs06") == 0 ||
           strcmp(name, "st02") == 0;
}

static int field_is_framing(const char *name)
{
    return strcmp(name, "event_type") == 0 ||
           strcmp(name, "source_file") == 0 ||
           strcmp(name, "source_transaction") == 0 ||
           strcmp(name, "source_drop_id") == 0 ||
           strcmp(name, "run_id") == 0 ||
           strcmp(name, "source_segment_index") == 0 ||
           strcmp(name, "source_byte_offset") == 0;
}

typedef struct
{
    const char *name;
    unsigned char type;
    const unsigned char *data;
    uint32_t len;
} parsed_field_t;

static int parse_builder_fields(
    const journal_record_builder_t *builder,
    parsed_field_t *out,
    size_t out_cap,
    size_t *out_count)
{
    const unsigned char *cursor;
    const unsigned char *end;
    unsigned short field_count;
    size_t i;

    if (builder == NULL || builder->data == NULL || builder->len < 2u ||
        out == NULL || out_count == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    cursor = builder->data;
    end = builder->data + builder->len;
    field_count = (unsigned short)(cursor[0] | (cursor[1] << 8u));
    cursor += 2u;

    /*
     * field_count in the header is written by end_event/journal_write_record;
     * here builder->field_count is the source of truth during emit.
     */
    (void)field_count;
    if (builder->field_count > out_cap)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    for (i = 0u; i < builder->field_count; i++)
    {
        unsigned short key_id;
        uint32_t value_len;

        if ((size_t)(end - cursor) < 7u)
        {
            return X12_ERR_IO;
        }
        key_id = (unsigned short)(cursor[0] | (cursor[1] << 8u));
        cursor += 2u;
        if (key_id == 0u || (size_t)key_id > builder->key_count)
        {
            return X12_ERR_IO;
        }
        out[i].name = builder->key_names[key_id - 1u];
        out[i].type = *cursor++;
        value_len = (uint32_t)cursor[0] |
                    ((uint32_t)cursor[1] << 8u) |
                    ((uint32_t)cursor[2] << 16u) |
                    ((uint32_t)cursor[3] << 24u);
        cursor += 4u;
        if ((size_t)(end - cursor) < value_len)
        {
            return X12_ERR_IO;
        }
        out[i].data = cursor;
        out[i].len = value_len;
        cursor += value_len;
    }

    *out_count = builder->field_count;
    return X12_OK;
}

static int emit_json(event_writer_t *writer)
{
    parsed_field_t fields[JOURNAL_EVENT_MAX_FIELDS];
    size_t field_count;
    FILE *fp = writer->fp;
    int wrote_any;
    int control_open;
    int payload_open;
    size_t i;
    int rc;

    rc = parse_builder_fields(&writer->builder, fields, JOURNAL_EVENT_MAX_FIELDS, &field_count);
    if (rc != X12_OK)
    {
        return rc;
    }

    if (fputc('{', fp) == EOF)
    {
        return X12_ERR_IO;
    }

    wrote_any = 0;
    control_open = 0;
    payload_open = 0;

    /* Framing fields. */
    for (i = 0u; i < field_count; i++)
    {
        if (!field_is_framing(fields[i].name))
        {
            continue;
        }
        if (wrote_any && fputc(',', fp) == EOF)
        {
            return X12_ERR_IO;
        }
        rc = event_writer_write_json_string(fp, fields[i].name, strlen(fields[i].name));
        if (rc != X12_OK)
        {
            return rc;
        }
        if (fputc(':', fp) == EOF)
        {
            return X12_ERR_IO;
        }
        rc = emit_json_value(fp, fields[i].type, fields[i].data, fields[i].len);
        if (rc != X12_OK)
        {
            return rc;
        }
        wrote_any = 1;
    }

    /* Control subobject. */
    for (i = 0u; i < field_count; i++)
    {
        if (!field_is_control(fields[i].name))
        {
            continue;
        }
        if (!control_open)
        {
            if (wrote_any && fputc(',', fp) == EOF)
            {
                return X12_ERR_IO;
            }
            if (fputs("\"control\":{", fp) == EOF)
            {
                return X12_ERR_IO;
            }
            control_open = 1;
            wrote_any = 1;
        }
        else if (fputc(',', fp) == EOF)
        {
            return X12_ERR_IO;
        }
        rc = event_writer_write_json_string(fp, fields[i].name, strlen(fields[i].name));
        if (rc != X12_OK)
        {
            return rc;
        }
        if (fputc(':', fp) == EOF)
        {
            return X12_ERR_IO;
        }
        rc = emit_json_value(fp, fields[i].type, fields[i].data, fields[i].len);
        if (rc != X12_OK)
        {
            return rc;
        }
    }
    if (control_open && fputc('}', fp) == EOF)
    {
        return X12_ERR_IO;
    }

    /* Payload subobject. */
    for (i = 0u; i < field_count; i++)
    {
        if (field_is_framing(fields[i].name) || field_is_control(fields[i].name))
        {
            continue;
        }
        if (!payload_open)
        {
            if (wrote_any && fputc(',', fp) == EOF)
            {
                return X12_ERR_IO;
            }
            if (fputs("\"payload\":{", fp) == EOF)
            {
                return X12_ERR_IO;
            }
            payload_open = 1;
        }
        else if (fputc(',', fp) == EOF)
        {
            return X12_ERR_IO;
        }
        rc = event_writer_write_json_string(fp, fields[i].name, strlen(fields[i].name));
        if (rc != X12_OK)
        {
            return rc;
        }
        if (fputc(':', fp) == EOF)
        {
            return X12_ERR_IO;
        }
        rc = emit_json_value(fp, fields[i].type, fields[i].data, fields[i].len);
        if (rc != X12_OK)
        {
            return rc;
        }
    }
    if (payload_open && fputc('}', fp) == EOF)
    {
        return X12_ERR_IO;
    }

    if (fputs("}\n", fp) == EOF)
    {
        return X12_ERR_IO;
    }

    return check_file(fp);
}

int event_writer_end_event(event_writer_t *writer)
{
    int rc;

    if (writer == NULL || writer->fp == NULL || !writer->builder_open)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    writer->builder_open = 0;

    if (writer->mode == EVENT_WRITER_MODE_JOURNAL)
    {
        return journal_write_record(writer->fp, &writer->builder, NULL, NULL);
    }

    rc = emit_json(writer);
    return rc;
}
