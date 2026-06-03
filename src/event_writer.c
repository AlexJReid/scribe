#include "event_writer.h"

#include <stdio.h>
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
    writer->isa13 = empty_str();
    writer->gs06 = empty_str();
    writer->st02 = empty_str();
    writer->include_phi = 0;

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
    writer->isa13 = empty_str();
    writer->gs06 = empty_str();
    writer->st02 = empty_str();
    writer->include_phi = 0;

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

int event_writer_close(event_writer_t *writer)
{
    int rc = X12_OK;

    if (writer == NULL || writer->fp == NULL) {
        return X12_OK;
    }

    if (fflush(writer->fp) != 0) {
        rc = X12_ERR_IO;
    }

    if (writer->owns_file && fclose(writer->fp) != 0) {
        rc = X12_ERR_IO;
    }

    writer->fp = NULL;
    writer->owns_file = 0;

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

    if (writer == NULL || writer->fp == NULL || event_type == NULL || seg == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
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

    if (fputs("}\n", writer->fp) == EOF) {
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
