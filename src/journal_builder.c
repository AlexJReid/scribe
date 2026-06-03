#include "journal_builder.h"

#include "event_writer.h"
#include "phi_vault.h"
#include "tokenise.h"
#include "x12_mapper_835.h"
#include "x12_mapper_837.h"
#include "x12_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOURNAL_LINE_MAX 4096u
#define JOURNAL_VALUE_MAX 256u

static x12_str_t str_from_cstr(const char *value)
{
    x12_str_t out;

    if (value == NULL) {
        value = "";
    }

    out.ptr = (char *)value;
    out.len = strlen(value);
    return out;
}

void journal_builder_input_init(journal_builder_input_t *input)
{
    if (input != NULL) {
        memset(input, 0, sizeof(*input));
    }
}

int journal_builder_input_add_charges(journal_builder_input_t *input, const char *path)
{
    if (input == NULL || path == NULL ||
        input->charges_count >= JOURNAL_BUILDER_MAX_INPUT_FILES) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    input->charges_paths[input->charges_count++] = path;
    return X12_OK;
}

int journal_builder_input_add_837(journal_builder_input_t *input, const char *path)
{
    if (input == NULL || path == NULL ||
        input->x837_count >= JOURNAL_BUILDER_MAX_INPUT_FILES) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    input->x837_paths[input->x837_count++] = path;
    return X12_OK;
}

int journal_builder_input_add_835(journal_builder_input_t *input, const char *path)
{
    if (input == NULL || path == NULL ||
        input->x835_count >= JOURNAL_BUILDER_MAX_INPUT_FILES) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    input->x835_paths[input->x835_count++] = path;
    return X12_OK;
}

static int json_get_string(
    const char *line,
    const char *key,
    char *out,
    size_t out_len
)
{
    char pattern[96];
    const char *cursor;
    const char *start;
    size_t key_len;
    size_t len = 0u;
    int written;

    if (line == NULL || key == NULL || out == NULL || out_len == 0u) {
        return 0;
    }

    key_len = strlen(key);
    written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(pattern) || key_len == 0u) {
        return 0;
    }

    cursor = strstr(line, pattern);
    if (cursor == NULL) {
        return 0;
    }
    cursor += strlen(pattern);
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor != ':') {
        return 0;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor != '"') {
        return 0;
    }
    cursor++;
    start = cursor;

    while (*cursor != '\0' && *cursor != '"' && len + 1u < out_len) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
        }
        cursor++;
        len = (size_t)(cursor - start);
    }

    if (*cursor != '"') {
        return 0;
    }

    if (len >= out_len) {
        return 0;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

static int json_get_bool(const char *line, const char *key, int *out)
{
    char pattern[96];
    const char *cursor;
    int written;

    if (line == NULL || key == NULL || out == NULL) {
        return 0;
    }

    written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(pattern)) {
        return 0;
    }

    cursor = strstr(line, pattern);
    if (cursor == NULL) {
        return 0;
    }
    cursor += strlen(pattern);
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor != ':') {
        return 0;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    if (strncmp(cursor, "true", 4u) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(cursor, "false", 5u) == 0) {
        *out = 0;
        return 1;
    }

    return 0;
}

static int write_tokenized_or_phi_field(
    event_writer_t *writer,
    FILE *fp,
    const char *name,
    token_type_t type,
    const char *raw,
    int prefix_comma,
    int include_phi
)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t raw_value = str_from_cstr(raw);
    x12_str_t token_value;
    int rc;

    if (include_phi) {
        rc = event_writer_record_phi_mapping(writer, type, raw_value);
        if (rc != X12_OK) {
            return rc;
        }
        return event_writer_write_string_field(fp, name, raw_value, prefix_comma);
    }

    if (raw_value.len == 0u) {
        return event_writer_write_string_field(fp, name, raw_value, prefix_comma);
    }

    rc = tokenise_value(type, raw_value, token, sizeof(token));
    if (rc != X12_OK) {
        return rc;
    }
    rc = event_writer_record_phi_mapping(writer, type, raw_value);
    if (rc != X12_OK) {
        return rc;
    }

    token_value = str_from_cstr(token);
    return event_writer_write_string_field(fp, name, token_value, prefix_comma);
}

static int write_phi_token_field(
    FILE *fp,
    const char *name,
    token_type_t type,
    const char *raw,
    int prefix_comma,
    int include_phi
)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t raw_value = str_from_cstr(raw);
    x12_str_t token_value;
    int rc;

    if (!include_phi) {
        return X12_OK;
    }

    if (raw_value.len == 0u) {
        return event_writer_write_string_field(fp, name, raw_value, prefix_comma);
    }

    rc = tokenise_value(type, raw_value, token, sizeof(token));
    if (rc != X12_OK) {
        return rc;
    }

    token_value = str_from_cstr(token);
    return event_writer_write_string_field(fp, name, token_value, prefix_comma);
}

static int write_charge_event(
    event_writer_t *writer,
    const char *event_type,
    const char *line,
    size_t line_number
)
{
    char encounter_id[JOURNAL_VALUE_MAX] = "";
    char patient_id[JOURNAL_VALUE_MAX] = "";
    char claim_id[JOURNAL_VALUE_MAX] = "";
    char claim_type[JOURNAL_VALUE_MAX] = "";
    char service_line_number[JOURNAL_VALUE_MAX] = "";
    char procedure_code[JOURNAL_VALUE_MAX] = "";
    char description[JOURNAL_VALUE_MAX] = "";
    char service_date[JOURNAL_VALUE_MAX] = "";
    char amount[JOURNAL_VALUE_MAX] = "";
    x12_segment_t fake_seg;
    FILE *fp = event_writer_stream(writer);
    int synthetic = 0;
    int has_synthetic;
    int rc;

    memset(&fake_seg, 0, sizeof(fake_seg));
    fake_seg.segment_index = line_number;
    fake_seg.byte_offset = 0u;

    (void)json_get_string(line, "encounter_id", encounter_id, sizeof(encounter_id));
    (void)json_get_string(line, "patient_id", patient_id, sizeof(patient_id));
    (void)json_get_string(line, "claim_id", claim_id, sizeof(claim_id));
    (void)json_get_string(line, "claim_type", claim_type, sizeof(claim_type));
    (void)json_get_string(line, "service_line_number", service_line_number, sizeof(service_line_number));
    (void)json_get_string(line, "procedure_code", procedure_code, sizeof(procedure_code));
    (void)json_get_string(line, "description", description, sizeof(description));
    (void)json_get_string(line, "service_date", service_date, sizeof(service_date));
    (void)json_get_string(line, "charge_amount", amount, sizeof(amount));
    if (amount[0] == '\0') {
        (void)json_get_string(line, "amount", amount, sizeof(amount));
    }
    has_synthetic = json_get_bool(line, "synthetic", &synthetic);

    rc = event_writer_begin_event(writer, event_type, &fake_seg);
    if (rc != X12_OK) {
        return rc;
    }
    if (fputc('{', fp) == EOF) {
        return X12_ERR_IO;
    }
    if (event_writer_write_cstring_field(fp, "encounter_id", encounter_id, 0) != X12_OK) {
        return X12_ERR_IO;
    }
    if (patient_id[0] != '\0') {
        if (write_tokenized_or_phi_field(
                writer,
                fp,
                "patient_id",
                TOK_PATIENT_ID,
                patient_id,
                1,
                event_writer_include_phi(writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        if (write_phi_token_field(
                fp,
                "patient_id_token",
                TOK_PATIENT_ID,
                patient_id,
                1,
                event_writer_include_phi(writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
    }
    if (has_synthetic) {
        if (fprintf(fp, ",\"synthetic\":%s", synthetic ? "true" : "false") < 0) {
            return X12_ERR_IO;
        }
    }
    if (claim_id[0] != '\0') {
        if (write_tokenized_or_phi_field(
                writer,
                fp,
                "claim_id",
                TOK_CLAIM_ID,
                claim_id,
                1,
                event_writer_include_phi(writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
        if (write_phi_token_field(
                fp,
                "claim_id_token",
                TOK_CLAIM_ID,
                claim_id,
                1,
                event_writer_include_phi(writer)
            ) != X12_OK) {
            return X12_ERR_IO;
        }
    }
    if (claim_type[0] != '\0' &&
        event_writer_write_cstring_field(fp, "claim_type", claim_type, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (service_line_number[0] != '\0' &&
        event_writer_write_cstring_field(fp, "service_line_number", service_line_number, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (procedure_code[0] != '\0' &&
        event_writer_write_cstring_field(fp, "procedure_code", procedure_code, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (description[0] != '\0' &&
        event_writer_write_cstring_field(fp, "description", description, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (service_date[0] != '\0' &&
        event_writer_write_cstring_field(fp, "service_date", service_date, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (amount[0] != '\0' &&
        event_writer_write_cstring_field(fp, "amount", amount, 1) != X12_OK) {
        return X12_ERR_IO;
    }
    if (fputc('}', fp) == EOF) {
        return X12_ERR_IO;
    }

    return event_writer_end_event(writer);
}

static int append_charges_file(
    FILE *fp,
    const char *path,
    int include_phi,
    phi_vault_t *phi_vault
)
{
    char line[JOURNAL_LINE_MAX];
    char event_type[JOURNAL_VALUE_MAX];
    event_writer_t writer;
    FILE *in;
    size_t line_number = 0u;
    int rc;

    rc = event_writer_open_stream(&writer, fp, path, "charge");
    if (rc != X12_OK) {
        return rc;
    }
    event_writer_set_include_phi(&writer, include_phi);
    event_writer_set_phi_vault(&writer, phi_vault, path);

    in = fopen(path, "rb");
    if (in == NULL) {
        return X12_ERR_IO;
    }

    while (fgets(line, sizeof(line), in) != NULL) {
        line_number++;
        event_type[0] = '\0';
        if (!json_get_string(line, "event_type", event_type, sizeof(event_type))) {
            continue;
        }

        rc = write_charge_event(&writer, event_type, line, line_number);
        if (rc != X12_OK) {
            (void)fclose(in);
            return rc;
        }
    }

    if (ferror(in)) {
        (void)fclose(in);
        return X12_ERR_IO;
    }
    if (fclose(in) != 0) {
        return X12_ERR_IO;
    }

    return X12_OK;
}

static int append_x12_file(
    FILE *fp,
    const char *path,
    const char *type,
    int include_phi,
    phi_vault_t *phi_vault
)
{
    x12_document_t doc;
    event_writer_t writer;
    int rc;

    rc = x12_document_load(path, &doc);
    if (rc != X12_OK) {
        return rc;
    }

    rc = event_writer_open_stream(&writer, fp, path, type);
    if (rc != X12_OK) {
        x12_document_free(&doc);
        return rc;
    }
    event_writer_set_include_phi(&writer, include_phi);
    event_writer_set_phi_vault(&writer, phi_vault, path);

    if (strcmp(type, "837") == 0) {
        rc = x12_map_837_document(&doc, &writer);
    } else if (strcmp(type, "835") == 0) {
        rc = x12_map_835_document(&doc, &writer);
    } else {
        rc = X12_ERR_UNSUPPORTED;
    }

    x12_document_free(&doc);
    return rc;
}

int journal_builder_build(
    const journal_builder_input_t *input,
    const char *out_path
)
{
    FILE *fp;
    phi_vault_t phi_vault;
    phi_vault_t *phi_vault_ptr = NULL;
    size_t i;
    int rc = X12_OK;
    int owns_file = 0;

    if (input == NULL || out_path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (strcmp(out_path, "-") == 0) {
        fp = stdout;
    } else {
        fp = fopen(out_path, "wb");
        owns_file = 1;
    }
    if (fp == NULL) {
        return X12_ERR_IO;
    }

    phi_vault_init(&phi_vault);
    if (input->phi_vault_path != NULL) {
        rc = phi_vault_open(&phi_vault, input->phi_vault_path);
        if (rc == X12_OK) {
            rc = phi_vault_init_schema(&phi_vault);
        }
        if (rc != X12_OK) {
            if (owns_file) {
                (void)fclose(fp);
            }
            return rc;
        }
        phi_vault_ptr = &phi_vault;
    }

    for (i = 0u; i < input->charges_count && rc == X12_OK; i++) {
        rc = append_charges_file(
            fp,
            input->charges_paths[i],
            input->include_phi,
            phi_vault_ptr
        );
    }
    for (i = 0u; i < input->x837_count && rc == X12_OK; i++) {
        rc = append_x12_file(fp, input->x837_paths[i], "837", input->include_phi, phi_vault_ptr);
    }
    for (i = 0u; i < input->x835_count && rc == X12_OK; i++) {
        rc = append_x12_file(fp, input->x835_paths[i], "835", input->include_phi, phi_vault_ptr);
    }

    if (phi_vault_ptr != NULL) {
        int close_rc = phi_vault_close(phi_vault_ptr);
        if (close_rc != X12_OK && rc == X12_OK) {
            rc = close_rc;
        }
    }
    if (fflush(fp) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (owns_file && fclose(fp) != 0 && rc == X12_OK) {
        rc = X12_ERR_IO;
    }

    return rc;
}

int journal_builder_run_cli(int argc, char **argv)
{
    journal_builder_input_t input;
    const char *out_path = NULL;
    int i;

    journal_builder_input_init(&input);

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--charges") == 0) {
            if (i + 1 >= argc ||
                journal_builder_input_add_charges(&input, argv[++i]) != X12_OK) {
                return -1;
            }
        } else if (strcmp(argv[i], "--837") == 0) {
            if (i + 1 >= argc ||
                journal_builder_input_add_837(&input, argv[++i]) != X12_OK) {
                return -1;
            }
        } else if (strcmp(argv[i], "--835") == 0) {
            if (i + 1 >= argc ||
                journal_builder_input_add_835(&input, argv[++i]) != X12_OK) {
                return -1;
            }
        } else if (strcmp(argv[i], "--include-phi") == 0) {
            input.include_phi = 1;
        } else if (strcmp(argv[i], "--phi-vault") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.phi_vault_path = argv[++i];
        } else {
            return -1;
        }
    }

    if (out_path == NULL ||
        (input.charges_count == 0u && input.x837_count == 0u && input.x835_count == 0u)) {
        return -1;
    }

    return journal_builder_build(&input, out_path);
}
