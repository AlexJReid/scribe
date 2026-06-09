#include "event_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static event_writer_t *active_binary_writer = NULL;

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

    if (fputs("\\u00", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (write_char(fp, digits[(value >> 4u) & 0x0fu]) != X12_OK) {
        return X12_ERR_IO;
    }
    return write_char(fp, digits[value & 0x0fu]);
}

int event_writer_open(
    event_writer_t *writer,
    const char *out_path,
    const char *source_file,
    const char *source_transaction
)
{
    if (writer == NULL || source_file == NULL || source_transaction == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(writer, 0, sizeof(*writer));
    writer->source_file = source_file;
    writer->source_transaction = source_transaction;
    writer->run_id = NULL;
    writer->isa13 = empty_str();
    writer->gs06 = empty_str();
    writer->st02 = empty_str();
    writer->include_phi = 0;
    writer->phi_vault = NULL;
    writer->phi_source_ref = source_file;
    writer->binary_journal = 0;
    writer->payload_sink = NULL;
    journal_record_builder_init(&writer->journal_record);

    if (out_path == NULL || strcmp(out_path, "-") == 0) {
        writer->fp = stdout;
        writer->owns_file = 0;
    } else {
        writer->fp = fopen(out_path, "wb");
        writer->owns_file = 1;
    }

    if (writer->fp == NULL) {
        return X12_ERR_IO;
    }

    return X12_OK;
}

int event_writer_open_stream(
    event_writer_t *writer,
    FILE *fp,
    const char *source_file,
    const char *source_transaction
)
{
    if (writer == NULL || fp == NULL || source_file == NULL || source_transaction == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(writer, 0, sizeof(*writer));
    writer->fp = fp;
    writer->owns_file = 0;
    writer->source_file = source_file;
    writer->source_transaction = source_transaction;
    writer->run_id = NULL;
    writer->isa13 = empty_str();
    writer->gs06 = empty_str();
    writer->st02 = empty_str();
    writer->include_phi = 0;
    writer->phi_vault = NULL;
    writer->phi_source_ref = source_file;
    writer->binary_journal = 0;
    writer->payload_sink = NULL;
    journal_record_builder_init(&writer->journal_record);

    return X12_OK;
}

void event_writer_set_include_phi(event_writer_t *writer, int include_phi)
{
    if (writer == NULL) {
        return;
    }

    writer->include_phi = include_phi ? 1 : 0;
}

int event_writer_include_phi(const event_writer_t *writer)
{
    if (writer == NULL) {
        return 0;
    }

    return writer->include_phi;
}

void event_writer_set_run_id(event_writer_t *writer, const char *run_id)
{
    if (writer == NULL) {
        return;
    }

    writer->run_id = run_id;
}

int event_writer_set_binary_journal(event_writer_t *writer, int binary_journal)
{
    if (writer == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    writer->binary_journal = binary_journal ? 1 : 0;
    if (!writer->binary_journal) {
        return X12_OK;
    }

    if (writer->payload_sink == NULL) {
        writer->payload_sink = tmpfile();
        if (writer->payload_sink == NULL) {
            return X12_ERR_IO;
        }
    }

    return X12_OK;
}

void event_writer_set_phi_vault(
    event_writer_t *writer,
    phi_vault_t *vault,
    const char *source_ref
)
{
    if (writer == NULL) {
        return;
    }

    writer->phi_vault = vault;
    writer->phi_source_ref = source_ref != NULL ? source_ref : writer->source_file;
}

int event_writer_record_phi_mapping(
    event_writer_t *writer,
    token_type_t type,
    x12_str_t raw
)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    int rc;

    if (writer == NULL || writer->phi_vault == NULL || raw.ptr == NULL || raw.len == 0u) {
        return X12_OK;
    }

    rc = tokenise_value(type, raw, token, sizeof(token));
    if (rc != X12_OK) {
        return rc;
    }

    return phi_vault_put_mapping(
        writer->phi_vault,
        tokenise_namespace(type),
        token,
        raw,
        writer->phi_source_ref
    );
}

int event_writer_record_phi_name(
    event_writer_t *writer,
    token_type_t name_type,
    x12_str_t last_name_or_org,
    x12_str_t first_name,
    token_type_t id_type,
    x12_str_t id_raw
)
{
    char name_raw_buf[512];
    char name_token[TOKENISE_MAX_TOKEN_LEN];
    char id_token[TOKENISE_MAX_TOKEN_LEN];
    char id_name_namespace[128];
    x12_str_t name_raw;
    int written;
    int rc;

    if (writer == NULL || writer->phi_vault == NULL ||
        last_name_or_org.ptr == NULL || last_name_or_org.len == 0u) {
        return X12_OK;
    }

    if (first_name.ptr != NULL && first_name.len > 0u) {
        written = snprintf(
            name_raw_buf,
            sizeof(name_raw_buf),
            "%.*s|%.*s",
            (int)last_name_or_org.len,
            last_name_or_org.ptr,
            (int)first_name.len,
            first_name.ptr
        );
    } else {
        written = snprintf(
            name_raw_buf,
            sizeof(name_raw_buf),
            "%.*s",
            (int)last_name_or_org.len,
            last_name_or_org.ptr
        );
    }
    if (written < 0 || (size_t)written >= sizeof(name_raw_buf)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    name_raw.ptr = name_raw_buf;
    name_raw.len = (size_t)written;

    rc = tokenise_value(name_type, name_raw, name_token, sizeof(name_token));
    if (rc != X12_OK) {
        return rc;
    }
    rc = phi_vault_put_mapping(
        writer->phi_vault,
        tokenise_namespace(name_type),
        name_token,
        name_raw,
        writer->phi_source_ref
    );
    if (rc != X12_OK) {
        return rc;
    }

    if (id_raw.ptr == NULL || id_raw.len == 0u || id_type == TOK_UNKNOWN) {
        return X12_OK;
    }

    rc = tokenise_value(id_type, id_raw, id_token, sizeof(id_token));
    if (rc != X12_OK) {
        return rc;
    }
    written = snprintf(
        id_name_namespace,
        sizeof(id_name_namespace),
        "%s_name",
        tokenise_namespace(id_type)
    );
    if (written < 0 || (size_t)written >= sizeof(id_name_namespace)) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    return phi_vault_put_mapping(
        writer->phi_vault,
        id_name_namespace,
        id_token,
        name_raw,
        writer->phi_source_ref
    );
}

int event_writer_close(event_writer_t *writer)
{
    int rc = X12_OK;

    if (writer == NULL) {
        return X12_OK;
    }

    if (active_binary_writer == writer) {
        active_binary_writer = NULL;
    }

    if (writer->fp != NULL && fflush(writer->fp) != 0) {
        rc = X12_ERR_IO;
    }

    if (writer->owns_file && writer->fp != NULL && fclose(writer->fp) != 0) {
        rc = X12_ERR_IO;
    }
    if (writer->payload_sink != NULL && fclose(writer->payload_sink) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    journal_record_builder_free(&writer->journal_record);

    writer->fp = NULL;
    writer->owns_file = 0;
    writer->payload_sink = NULL;
    writer->binary_journal = 0;

    return rc;
}

void event_writer_observe_control(
    event_writer_t *writer,
    const x12_segment_t *seg
)
{
    if (writer == NULL || seg == NULL) {
        return;
    }

    if (x12_str_eq_cstr(seg->tag, "ISA") && seg->element_count > 12u) {
        writer->isa13 = seg->elements[12];
    } else if (x12_str_eq_cstr(seg->tag, "GS") && seg->element_count > 5u) {
        writer->gs06 = seg->elements[5];
    } else if (x12_str_eq_cstr(seg->tag, "ST") && seg->element_count > 1u) {
        writer->st02 = seg->elements[1];
    }
}

FILE *event_writer_stream(event_writer_t *writer)
{
    if (writer == NULL) {
        return NULL;
    }
    if (writer->binary_journal) {
        return writer->payload_sink;
    }

    return writer->fp;
}

int event_writer_write_json_string(FILE *fp, const char *data, size_t len)
{
    size_t i;

    if (fp == NULL || data == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (write_char(fp, '"') != X12_OK) {
        return X12_ERR_IO;
    }

    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        switch (ch) {
        case '"':
            if (fputs("\\\"", fp) == EOF) {
                return X12_ERR_IO;
            }
            break;
        case '\\':
            if (fputs("\\\\", fp) == EOF) {
                return X12_ERR_IO;
            }
            break;
        case '\n':
            if (fputs("\\n", fp) == EOF) {
                return X12_ERR_IO;
            }
            break;
        case '\r':
            if (fputs("\\r", fp) == EOF) {
                return X12_ERR_IO;
            }
            break;
        case '\t':
            if (fputs("\\t", fp) == EOF) {
                return X12_ERR_IO;
            }
            break;
        default:
            if (ch < 0x20u) {
                if (write_hex_escape(fp, ch) != X12_OK) {
                    return X12_ERR_IO;
                }
            } else if (write_char(fp, (char)ch) != X12_OK) {
                return X12_ERR_IO;
            }
            break;
        }
    }

    if (write_char(fp, '"') != X12_OK) {
        return X12_ERR_IO;
    }

    return check_file(fp);
}

int event_writer_write_cstring(FILE *fp, const char *value)
{
    if (value == NULL) {
        value = "";
    }

    return event_writer_write_json_string(fp, value, strlen(value));
}

int event_writer_begin_event(
    event_writer_t *writer,
    const char *event_type,
    const x12_segment_t *seg
)
{
    FILE *fp;
    int rc;

    if (writer == NULL || writer->fp == NULL || event_type == NULL || seg == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (writer->binary_journal) {
        if (writer->payload_sink == NULL) {
            return X12_ERR_INVALID_ARGUMENT;
        }
        rewind(writer->payload_sink);
        rc = journal_record_builder_reset(&writer->journal_record);
        if (rc == X12_OK) {
            rc = journal_record_add_cstring(&writer->journal_record, "event_type", event_type);
        }
        if (rc == X12_OK) {
            rc = journal_record_add_cstring(&writer->journal_record, "source_file", writer->source_file);
        }
        if (rc == X12_OK) {
            rc = journal_record_add_cstring(
                &writer->journal_record,
                "source_transaction",
                writer->source_transaction
            );
        }
        if (rc == X12_OK && writer->run_id != NULL && writer->run_id[0] != '\0') {
            rc = journal_record_add_cstring(&writer->journal_record, "run_id", writer->run_id);
        }
        if (rc == X12_OK) {
            rc = journal_record_add_u64(
                &writer->journal_record,
                "source_segment_index",
                (unsigned long long)seg->segment_index
            );
        }
        if (rc == X12_OK) {
            rc = journal_record_add_u64(
                &writer->journal_record,
                "source_byte_offset",
                (unsigned long long)seg->byte_offset
            );
        }
        if (rc == X12_OK) {
            rc = journal_record_add_string(
                &writer->journal_record,
                "isa13",
                writer->isa13.ptr,
                writer->isa13.len
            );
        }
        if (rc == X12_OK) {
            rc = journal_record_add_string(
                &writer->journal_record,
                "gs06",
                writer->gs06.ptr,
                writer->gs06.len
            );
        }
        if (rc == X12_OK) {
            rc = journal_record_add_string(
                &writer->journal_record,
                "st02",
                writer->st02.ptr,
                writer->st02.len
            );
        }
        if (rc != X12_OK) {
            return rc;
        }
        active_binary_writer = writer;
        return X12_OK;
    }

    fp = writer->fp;

    if (fputs("{\"event_type\":", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring(fp, event_type) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs(",\"source_file\":", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring(fp, writer->source_file) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs(",\"source_transaction\":", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring(fp, writer->source_transaction) != X12_OK) {
        return X12_ERR_IO;
    }
    if (writer->run_id != NULL && writer->run_id[0] != '\0') {
        if (fputs(",\"run_id\":", fp) == EOF) {
            return X12_ERR_IO;
        }
        if (event_writer_write_cstring(fp, writer->run_id) != X12_OK) {
            return X12_ERR_IO;
        }
    }
    if (fprintf(
            fp,
            ",\"source_segment_index\":%zu,\"source_byte_offset\":%zu,\"control\":{\"isa13\":",
            seg->segment_index,
            seg->byte_offset
        ) < 0) {
        return X12_ERR_IO;
    }
    if (event_writer_write_json_string(fp, writer->isa13.ptr, writer->isa13.len) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs(",\"gs06\":", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (event_writer_write_json_string(fp, writer->gs06.ptr, writer->gs06.len) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs(",\"st02\":", fp) == EOF) {
        return X12_ERR_IO;
    }
    if (event_writer_write_json_string(fp, writer->st02.ptr, writer->st02.len) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs("},\"payload\":", fp) == EOF) {
        return X12_ERR_IO;
    }

    return check_file(fp);
}

int event_writer_end_event(event_writer_t *writer)
{
    if (writer == NULL || writer->fp == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (writer->binary_journal) {
        int rc = journal_write_record(writer->fp, &writer->journal_record, NULL, NULL);
        if (active_binary_writer == writer) {
            active_binary_writer = NULL;
        }
        return rc;
    }

    if (fputc('}', writer->fp) == EOF) {
        return X12_ERR_IO;
    }
    if (fputc('\n', writer->fp) == EOF) {
        return X12_ERR_IO;
    }

    return check_file(writer->fp);
}

int event_writer_write_string_field(
    FILE *fp,
    const char *name,
    x12_str_t value,
    int prefix_comma
)
{
    if (fp == NULL || name == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (active_binary_writer != NULL && fp == active_binary_writer->payload_sink) {
        return journal_record_add_string(
            &active_binary_writer->journal_record,
            name,
            value.ptr,
            value.len
        );
    }

    if (prefix_comma && write_char(fp, ',') != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_json_string(fp, name, strlen(name)) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_char(fp, ':') != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_write_json_string(fp, value.ptr, value.len);
}

int event_writer_write_cstring_field(
    FILE *fp,
    const char *name,
    const char *value,
    int prefix_comma
)
{
    if (fp == NULL || name == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (active_binary_writer != NULL && fp == active_binary_writer->payload_sink) {
        return journal_record_add_cstring(&active_binary_writer->journal_record, name, value);
    }

    if (prefix_comma && write_char(fp, ',') != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_json_string(fp, name, strlen(name)) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_char(fp, ':') != X12_OK) {
        return X12_ERR_IO;
    }
    return event_writer_write_cstring(fp, value);
}

int event_writer_write_bool_field(
    FILE *fp,
    const char *name,
    int value,
    int prefix_comma
)
{
    if (fp == NULL || name == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (active_binary_writer != NULL && fp == active_binary_writer->payload_sink) {
        return journal_record_add_bool(&active_binary_writer->journal_record, name, value);
    }

    if (prefix_comma && write_char(fp, ',') != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_json_string(fp, name, strlen(name)) != X12_OK) {
        return X12_ERR_IO;
    }
    if (write_char(fp, ':') != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs(value ? "true" : "false", fp) == EOF) {
        return X12_ERR_IO;
    }

    return check_file(fp);
}

int event_writer_write_str_array_field(
    FILE *fp,
    const char *name,
    const x12_str_t *values,
    size_t count,
    int prefix_comma
)
{
    size_t i;

    if (fp == NULL || name == NULL || (values == NULL && count > 0u)) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (active_binary_writer != NULL && fp == active_binary_writer->payload_sink) {
        return journal_record_add_string_array(
            &active_binary_writer->journal_record,
            name,
            values,
            count
        );
    }

    if (prefix_comma && write_char(fp, ',') != X12_OK) {
        return X12_ERR_IO;
    }
    if (event_writer_write_json_string(fp, name, strlen(name)) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputs(":[", fp) == EOF) {
        return X12_ERR_IO;
    }

    for (i = 0; i < count; i++) {
        if (i > 0u && write_char(fp, ',') != X12_OK) {
            return X12_ERR_IO;
        }
        if (event_writer_write_json_string(fp, values[i].ptr, values[i].len) != X12_OK) {
            return X12_ERR_IO;
        }
    }

    if (write_char(fp, ']') != X12_OK) {
        return X12_ERR_IO;
    }

    return check_file(fp);
}
