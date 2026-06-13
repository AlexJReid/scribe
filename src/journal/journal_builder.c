#include "journal_builder.h"

#include "event_writer.h"
#include "journal.h"
#include "phi_vault.h"
#include "run_id.h"
#include "x12_mapper_registry.h"
#include "x12_reader.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#define SOURCE_PATH_MAX 4096u
#define JOURNAL_BUILDER_DEFAULT_ZSTD_LEVEL 1

void journal_builder_input_init(journal_builder_input_t *input)
{
    if (input != NULL)
    {
        memset(input, 0, sizeof(*input));
    }
}

int journal_builder_input_add(
    journal_builder_input_t *input,
    const char *type,
    const char *path)
{
    if (input == NULL || type == NULL || path == NULL ||
        x12_mapper_for_type(type) == NULL ||
        input->file_count >= JOURNAL_BUILDER_MAX_INPUT_FILES)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    input->files[input->file_count].type = type;
    input->files[input->file_count].path = path;
    input->file_count++;
    return X12_OK;
}

int journal_builder_input_add_list(
    journal_builder_input_t *input,
    const char *type,
    const char *list_path)
{
    if (input == NULL || type == NULL || list_path == NULL ||
        x12_mapper_for_type(type) == NULL ||
        input->list_count >= JOURNAL_BUILDER_MAX_LISTS)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    input->lists[input->list_count].type = type;
    input->lists[input->list_count].path = list_path;
    input->list_count++;
    return X12_OK;
}

int journal_builder_input_add_270(journal_builder_input_t *input, const char *path)
{
    return journal_builder_input_add(input, "270", path);
}

int journal_builder_input_add_271(journal_builder_input_t *input, const char *path)
{
    return journal_builder_input_add(input, "271", path);
}

int journal_builder_input_add_834(journal_builder_input_t *input, const char *path)
{
    return journal_builder_input_add(input, "834", path);
}

int journal_builder_input_add_837(journal_builder_input_t *input, const char *path)
{
    return journal_builder_input_add(input, "837", path);
}

int journal_builder_input_add_835(journal_builder_input_t *input, const char *path)
{
    return journal_builder_input_add(input, "835", path);
}

static int builder_has_suffix(const char *value, const char *suffix)
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

static int make_temp_path(
    const char *path,
    const char *suffix,
    char *out,
    size_t out_len)
{
    int written;

    if (path == NULL || suffix == NULL || out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    written = snprintf(out, out_len, "%s%s", path, suffix);
    if (written < 0 || (size_t)written >= out_len)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    return X12_OK;
}

static int write_all(FILE *fp, const void *data, size_t len)
{
    if (fp == NULL || (data == NULL && len > 0u))
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (len == 0u)
    {
        return X12_OK;
    }
    if (fwrite(data, 1u, len, fp) != len)
    {
        return X12_ERR_IO;
    }
    return X12_OK;
}

static int compress_file_zstd(const char *input_path, const char *out_path, int level)
{
    FILE *in_fp = NULL;
    FILE *out_fp = NULL;
    ZSTD_CCtx *ctx = NULL;
    void *in_buf = NULL;
    void *out_buf = NULL;
    char temp_out_path[SOURCE_PATH_MAX];
    size_t in_cap;
    size_t out_cap;
    int rc = X12_OK;

    if (input_path == NULL || out_path == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (!builder_has_suffix(out_path, ".zst"))
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (level <= 0)
    {
        level = JOURNAL_BUILDER_DEFAULT_ZSTD_LEVEL;
    }

    rc = make_temp_path(out_path, ".tmp", temp_out_path, sizeof(temp_out_path));
    if (rc != X12_OK)
    {
        return rc;
    }

    in_fp = fopen(input_path, "rb");
    if (in_fp == NULL)
    {
        return X12_ERR_IO;
    }
    out_fp = fopen(temp_out_path, "wb");
    if (out_fp == NULL)
    {
        (void)fclose(in_fp);
        return X12_ERR_IO;
    }

    ctx = ZSTD_createCCtx();
    if (ctx == NULL)
    {
        rc = X12_ERR_NO_MEMORY;
        goto cleanup;
    }
    {
        size_t zstd_rc = ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, level);
        if (ZSTD_isError(zstd_rc))
        {
            rc = X12_ERR_IO;
            goto cleanup;
        }
    }

    in_cap = ZSTD_CStreamInSize();
    out_cap = ZSTD_CStreamOutSize();
    in_buf = malloc(in_cap);
    out_buf = malloc(out_cap);
    if (in_buf == NULL || out_buf == NULL)
    {
        rc = X12_ERR_NO_MEMORY;
        goto cleanup;
    }

    while (rc == X12_OK)
    {
        size_t read_len = fread(in_buf, 1u, in_cap, in_fp);
        int last_chunk = read_len < in_cap;
        ZSTD_EndDirective mode = last_chunk ? ZSTD_e_end : ZSTD_e_continue;
        ZSTD_inBuffer input = {in_buf, read_len, 0u};

        if (read_len == 0u && ferror(in_fp))
        {
            rc = X12_ERR_IO;
            break;
        }

        do
        {
            ZSTD_outBuffer output = {out_buf, out_cap, 0u};
            size_t zstd_rc = ZSTD_compressStream2(ctx, &output, &input, mode);

            if (ZSTD_isError(zstd_rc))
            {
                rc = X12_ERR_IO;
                break;
            }
            rc = write_all(out_fp, out_buf, output.pos);
            if (rc != X12_OK)
            {
                break;
            }
            if (last_chunk && zstd_rc == 0u)
            {
                break;
            }
        } while (input.pos < input.size || last_chunk);

        if (last_chunk)
        {
            break;
        }
    }

cleanup:
    free(in_buf);
    free(out_buf);
    if (ctx != NULL)
    {
        ZSTD_freeCCtx(ctx);
    }
    if (in_fp != NULL && fclose(in_fp) != 0 && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }
    if (out_fp != NULL && fclose(out_fp) != 0 && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }
    if (rc == X12_OK)
    {
        (void)remove(out_path);
        if (rename(temp_out_path, out_path) != 0)
        {
            rc = X12_ERR_IO;
        }
    }
    if (rc != X12_OK)
    {
        (void)remove(temp_out_path);
    }
    return rc;
}

static int source_path_is_absolute(const char *path)
{
    if (path == NULL || path[0] == '\0')
    {
        return 0;
    }
#ifdef _WIN32
    if (path[0] == '/' || path[0] == '\\')
    {
        return 1;
    }
    return path[0] != '\0' &&
           path[1] == ':' &&
           (path[2] == '/' || path[2] == '\\');
#else
    return path[0] == '/';
#endif
}

static void normalize_path_separators(char *path)
{
    size_t i;

    if (path == NULL)
    {
        return;
    }
    for (i = 0u; path[i] != '\0'; i++)
    {
        if (path[i] == '\\')
        {
            path[i] = '/';
        }
    }
}

static int source_getcwd(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    if (_getcwd(out, (int)out_len) == NULL)
    {
        return X12_ERR_IO;
    }
#else
    if (getcwd(out, out_len) == NULL)
    {
        return X12_ERR_IO;
    }
#endif
    normalize_path_separators(out);
    return X12_OK;
}

static int source_join_path(
    const char *base,
    const char *name,
    char *out,
    size_t out_len)
{
    size_t base_len;
    size_t name_len;
    int needs_separator;
    int written;

    if (base == NULL || name == NULL || out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    base_len = strlen(base);
    name_len = strlen(name);
    needs_separator = base_len > 0u &&
                      base[base_len - 1u] != '/' &&
                      base[base_len - 1u] != '\\';
    if (base_len > SIZE_MAX - name_len - (needs_separator ? 2u : 1u))
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    written = snprintf(
        out,
        out_len,
        needs_separator ? "%s/%s" : "%s%s",
        base,
        name);
    if (written < 0 || (size_t)written >= out_len)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    normalize_path_separators(out);
    return X12_OK;
}

static void trim_trailing_separators(char *path)
{
    size_t len;

    if (path == NULL)
    {
        return;
    }

    len = strlen(path);
    while (len > 1u && (path[len - 1u] == '/' || path[len - 1u] == '\\'))
    {
#ifdef _WIN32
        if (len == 3u && path[1] == ':')
        {
            break;
        }
#endif
        path[--len] = '\0';
    }
}

static int source_absolute_path(
    const char *path,
    char *out,
    size_t out_len)
{
    char cwd[SOURCE_PATH_MAX];
    int rc;

    if (path == NULL || out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (source_path_is_absolute(path))
    {
        if (strlen(path) >= out_len)
        {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(out, path, strlen(path) + 1u);
        normalize_path_separators(out);
        return X12_OK;
    }

    rc = source_getcwd(cwd, sizeof(cwd));
    if (rc != X12_OK)
    {
        return rc;
    }
    return source_join_path(cwd, path, out, out_len);
}

static int source_path_under_root(
    const char *path_abs,
    const char *root_abs,
    const char **out_relative)
{
    size_t root_len;
    const char *relative;

    if (path_abs == NULL || root_abs == NULL || out_relative == NULL)
    {
        return 0;
    }

    root_len = strlen(root_abs);
    if (root_len == 0u)
    {
        return 0;
    }
    if (strcmp(root_abs, "/") == 0)
    {
        *out_relative = path_abs + 1u;
        return 1;
    }
    if (strncmp(path_abs, root_abs, root_len) != 0)
    {
        return 0;
    }
    if (path_abs[root_len] != '\0' &&
        path_abs[root_len] != '/' &&
        path_abs[root_len] != '\\')
    {
        return 0;
    }

    relative = path_abs + root_len;
    while (*relative == '/' || *relative == '\\')
    {
        relative++;
    }
    if (*relative == '\0')
    {
        return 0;
    }
    *out_relative = relative;
    return 1;
}

static const char *source_basename(const char *path)
{
    const char *slash;
    const char *backslash;

    if (path == NULL)
    {
        return "";
    }

    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    if (slash == NULL || (backslash != NULL && backslash > slash))
    {
        slash = backslash;
    }
    return slash == NULL ? path : slash + 1;
}

static int path_has_parent_component(const char *path)
{
    const char *cursor = path;

    if (path == NULL)
    {
        return 0;
    }

    while (*cursor != '\0')
    {
        while (*cursor == '/' || *cursor == '\\')
        {
            cursor++;
        }
        if (cursor[0] == '.' &&
            cursor[1] == '.' &&
            (cursor[2] == '\0' || cursor[2] == '/' || cursor[2] == '\\'))
        {
            return 1;
        }
        while (*cursor != '\0' && *cursor != '/' && *cursor != '\\')
        {
            cursor++;
        }
    }

    return 0;
}

static int copy_portable_source_path(
    const char *path,
    char *out,
    size_t out_len)
{
    size_t i;
    size_t j = 0u;

    if (path == NULL || out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\'))
    {
        path += 2;
    }
    while (path[0] == '/' || path[0] == '\\')
    {
        path++;
    }

    for (i = 0u; path[i] != '\0'; i++)
    {
        char ch = path[i] == '\\' ? '/' : path[i];

        if (j + 1u >= out_len)
        {
            return X12_ERR_BUFFER_TOO_SMALL;
        }
        out[j++] = ch;
    }
    out[j] = '\0';
    if (out[0] == '\0')
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    return X12_OK;
}

static int format_source_file(
    const char *source_root_abs,
    const char *path,
    char *out,
    size_t out_len)
{
    char path_abs[SOURCE_PATH_MAX];
    const char *relative;
    int rc;

    if (source_root_abs == NULL || path == NULL || out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = source_absolute_path(path, path_abs, sizeof(path_abs));
    if (rc != X12_OK)
    {
        return rc;
    }
    trim_trailing_separators(path_abs);

    if (source_path_under_root(path_abs, source_root_abs, &relative) &&
        !path_has_parent_component(relative))
    {
        return copy_portable_source_path(relative, out, out_len);
    }
    if (!source_path_is_absolute(path) && !path_has_parent_component(path))
    {
        return copy_portable_source_path(path, out, out_len);
    }
    return copy_portable_source_path(source_basename(path), out, out_len);
}

static int append_x12_file(
    FILE *fp,
    const char *path,
    const char *type,
    int include_phi,
    const char *run_id,
    const char *source_root_abs,
    phi_vault_t *phi_vault)
{
    x12_document_t doc;
    event_writer_t writer;
    char source_file[SOURCE_PATH_MAX];
    int rc;

    rc = format_source_file(source_root_abs, path, source_file, sizeof(source_file));
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = x12_document_load(path, &doc);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = event_writer_open_stream(&writer, fp, source_file, type);
    if (rc != X12_OK)
    {
        x12_document_free(&doc);
        return rc;
    }
    event_writer_set_include_phi(&writer, include_phi);
    event_writer_set_run_id(&writer, run_id);
    rc = event_writer_set_mode(&writer, EVENT_WRITER_MODE_JOURNAL);
    if (rc != X12_OK)
    {
        (void)event_writer_close(&writer);
        x12_document_free(&doc);
        return rc;
    }
    event_writer_set_phi_vault(&writer, phi_vault);

    {
        x12_mapper_fn map = x12_mapper_for_type(type);
        rc = map != NULL ? map(&doc, &writer) : X12_ERR_UNSUPPORTED;
    }

    {
        int close_rc = event_writer_close(&writer);
        if (close_rc != X12_OK && rc == X12_OK)
        {
            rc = close_rc;
        }
    }
    x12_document_free(&doc);
    return rc;
}

static void trim_line_end(char *line)
{
    size_t len;

    if (line == NULL)
    {
        return;
    }

    len = strlen(line);
    while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r'))
    {
        line[len - 1u] = '\0';
        len--;
    }
}

static int append_x12_file_list(
    FILE *fp,
    const char *list_path,
    const char *type,
    int include_phi,
    const char *run_id,
    const char *source_root_abs,
    phi_vault_t *phi_vault)
{
    FILE *list_fp;
    char path[4096];
    int rc = X12_OK;

    if (list_path == NULL)
    {
        return X12_OK;
    }

    list_fp = fopen(list_path, "rb");
    if (list_fp == NULL)
    {
        return X12_ERR_IO;
    }

    while (fgets(path, sizeof(path), list_fp) != NULL)
    {
        if (strchr(path, '\n') == NULL && !feof(list_fp))
        {
            rc = X12_ERR_BUFFER_TOO_SMALL;
            break;
        }
        trim_line_end(path);
        if (path[0] == '\0')
        {
            continue;
        }

        rc = append_x12_file(
            fp,
            path,
            type,
            include_phi,
            run_id,
            source_root_abs,
            phi_vault);
        if (rc != X12_OK)
        {
            break;
        }
    }

    if (ferror(list_fp) && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }
    if (fclose(list_fp) != 0 && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

static int open_journal_output(
    const char *out_path,
    int append,
    FILE **out)
{
    FILE *fp;
    long size;
    int rc;

    if (out_path == NULL || out == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    *out = NULL;
    if (!append)
    {
        fp = fopen(out_path, "wb");
        if (fp == NULL)
        {
            return X12_ERR_IO;
        }
        rc = journal_write_header(fp);
        if (rc != X12_OK)
        {
            (void)fclose(fp);
            return rc;
        }
        *out = fp;
        return X12_OK;
    }

    fp = fopen(out_path, "ab+");
    if (fp == NULL)
    {
        return X12_ERR_IO;
    }
    if (fseek(fp, 0L, SEEK_END) != 0)
    {
        (void)fclose(fp);
        return X12_ERR_IO;
    }
    size = ftell(fp);
    if (size < 0)
    {
        (void)fclose(fp);
        return X12_ERR_IO;
    }
    if (size == 0L)
    {
        rc = journal_write_header(fp);
    }
    else
    {
        if (fseek(fp, 0L, SEEK_SET) != 0)
        {
            (void)fclose(fp);
            return X12_ERR_IO;
        }
        rc = journal_read_header(fp);
        if (rc == X12_OK && fseek(fp, 0L, SEEK_END) != 0)
        {
            rc = X12_ERR_IO;
        }
    }
    if (rc != X12_OK)
    {
        (void)fclose(fp);
        return rc;
    }

    *out = fp;
    return X12_OK;
}

int journal_builder_build(
    const journal_builder_input_t *input,
    const char *out_path)
{
    FILE *fp;
    phi_vault_t phi_vault;
    phi_vault_t *phi_vault_ptr = NULL;
    char generated_run_id[96];
    char source_root_abs[SOURCE_PATH_MAX];
    char raw_out_path[SOURCE_PATH_MAX];
    const char *write_path;
    const char *run_id;
    const char *source_root;
    size_t i;
    int rc = X12_OK;
    int owns_file = 0;

    if (input == NULL || out_path == NULL || strcmp(out_path, "-") == 0)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (input->append && input->compress_zstd)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    write_path = out_path;
    if (input->compress_zstd)
    {
        if (!builder_has_suffix(out_path, ".zst"))
        {
            return X12_ERR_INVALID_ARGUMENT;
        }
        rc = make_temp_path(out_path, ".raw", raw_out_path, sizeof(raw_out_path));
        if (rc != X12_OK)
        {
            return rc;
        }
        write_path = raw_out_path;
        (void)remove(write_path);
    }

    run_id = input->run_id;
    if (run_id == NULL || run_id[0] == '\0')
    {
        rc = scribe_run_id_generate(generated_run_id, sizeof(generated_run_id));
        if (rc != X12_OK)
        {
            return rc;
        }
        run_id = generated_run_id;
    }

    source_root = input->source_root;
    if (source_root == NULL || source_root[0] == '\0')
    {
        source_root = ".";
    }
    if (strcmp(source_root, ".") == 0)
    {
        rc = source_getcwd(source_root_abs, sizeof(source_root_abs));
        if (rc != X12_OK)
        {
            return rc;
        }
    }
    else
    {
        rc = source_absolute_path(source_root, source_root_abs, sizeof(source_root_abs));
        if (rc != X12_OK)
        {
            return rc;
        }
    }
    trim_trailing_separators(source_root_abs);

    rc = open_journal_output(write_path, input->append, &fp);
    owns_file = 1;
    if (rc != X12_OK)
    {
        if (input->compress_zstd)
        {
            (void)remove(write_path);
        }
        return rc;
    }

    phi_vault_init(&phi_vault);
    if (input->phi_vault_path != NULL)
    {
        rc = phi_vault_open(&phi_vault, input->phi_vault_path);
        if (rc == X12_OK)
        {
            rc = phi_vault_init_schema(&phi_vault);
        }
        if (rc != X12_OK)
        {
            if (phi_vault.db != NULL)
            {
                (void)phi_vault_close(&phi_vault);
            }
            if (owns_file)
            {
                (void)fclose(fp);
            }
            if (input->compress_zstd)
            {
                (void)remove(write_path);
            }
            return rc;
        }
        phi_vault_ptr = &phi_vault;
    }

    for (i = 0u; i < input->file_count && rc == X12_OK; i++)
    {
        rc = append_x12_file(
            fp,
            input->files[i].path,
            input->files[i].type,
            input->include_phi,
            run_id,
            source_root_abs,
            phi_vault_ptr);
    }
    for (i = 0u; i < input->list_count && rc == X12_OK; i++)
    {
        rc = append_x12_file_list(
            fp,
            input->lists[i].path,
            input->lists[i].type,
            input->include_phi,
            run_id,
            source_root_abs,
            phi_vault_ptr);
    }

    if (phi_vault_ptr != NULL)
    {
        int close_rc = phi_vault_close(phi_vault_ptr);
        if (close_rc != X12_OK && rc == X12_OK)
        {
            rc = close_rc;
        }
    }
    if (fflush(fp) != 0 && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }
    if (owns_file && fclose(fp) != 0 && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }
    owns_file = 0;

    if (input->compress_zstd && rc == X12_OK)
    {
        rc = compress_file_zstd(
            write_path,
            out_path,
            input->zstd_level);
    }
    if (input->compress_zstd)
    {
        (void)remove(write_path);
    }

    return rc;
}

/*
 * Recognize --NNN and --NNN-list for every type the mapper registry knows
 * about. Returns 1 on match, 0 if the flag belongs to nobody, -1 on error
 * (missing argument, unsupported type).
 */
static int try_match_type_flag(
    journal_builder_input_t *input,
    int argc,
    char **argv,
    int *i)
{
    size_t entry_count;
    const x12_mapper_entry_t *entries = x12_mapper_table(&entry_count);
    const char *flag = argv[*i];
    size_t j;

    for (j = 0u; j < entry_count; j++)
    {
        const char *type = entries[j].type;
        char buf[16];

        (void)snprintf(buf, sizeof(buf), "--%s", type);
        if (strcmp(flag, buf) == 0)
        {
            if (*i + 1 >= argc)
            {
                return -1;
            }
            return journal_builder_input_add(input, type, argv[++(*i)]) == X12_OK
                       ? 1
                       : -1;
        }

        (void)snprintf(buf, sizeof(buf), "--%s-list", type);
        if (strcmp(flag, buf) == 0)
        {
            if (*i + 1 >= argc)
            {
                return -1;
            }
            return journal_builder_input_add_list(input, type, argv[++(*i)]) == X12_OK
                       ? 1
                       : -1;
        }
    }

    return 0;
}

int journal_builder_run_cli(int argc, char **argv)
{
    journal_builder_input_t input;
    const char *out_path = NULL;
    int i;

    journal_builder_input_init(&input);

    for (i = 2; i < argc; i++)
    {
        int match = try_match_type_flag(&input, argc, argv, &i);

        if (match < 0)
        {
            return -1;
        }
        if (match == 1)
        {
            continue;
        }

        if (strcmp(argv[i], "--out") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            out_path = argv[++i];
        }
        else if (strcmp(argv[i], "--include-phi") == 0)
        {
            input.include_phi = 1;
        }
        else if (strcmp(argv[i], "--append") == 0)
        {
            input.append = 1;
        }
        else if (strcmp(argv[i], "--phi-vault") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            input.phi_vault_path = argv[++i];
        }
        else if (strcmp(argv[i], "--run-id") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            input.run_id = argv[++i];
        }
        else if (strcmp(argv[i], "--source-root") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            input.source_root = argv[++i];
        }
        else if (strcmp(argv[i], "--compress") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            if (strcmp(argv[++i], "zstd") != 0)
            {
                return -1;
            }
            input.compress_zstd = 1;
        }
        else if (strcmp(argv[i], "--zstd-level") == 0)
        {
            char *end = NULL;
            long level;

            if (i + 1 >= argc)
            {
                return -1;
            }
            level = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || level < 1L || level > 22L)
            {
                return -1;
            }
            input.zstd_level = (int)level;
        }
        else
        {
            return -1;
        }
    }

    if (out_path == NULL || (input.file_count == 0u && input.list_count == 0u))
    {
        return -1;
    }

    return journal_builder_build(&input, out_path);
}
