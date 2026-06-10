#include "journal_builder.h"

#include "event_writer.h"
#include "journal.h"
#include "phi_vault.h"
#include "run_id.h"
#include "x12_mapper_270_271.h"
#include "x12_mapper_834.h"
#include "x12_mapper_835.h"
#include "x12_mapper_837.h"
#include "x12_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void journal_builder_input_init(journal_builder_input_t *input)
{
    if (input != NULL) {
        memset(input, 0, sizeof(*input));
    }
}

int journal_builder_input_add_270(journal_builder_input_t *input, const char *path)
{
    if (input == NULL || path == NULL ||
        input->x270_count >= JOURNAL_BUILDER_MAX_INPUT_FILES) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    input->x270_paths[input->x270_count++] = path;
    return X12_OK;
}

int journal_builder_input_add_271(journal_builder_input_t *input, const char *path)
{
    if (input == NULL || path == NULL ||
        input->x271_count >= JOURNAL_BUILDER_MAX_INPUT_FILES) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    input->x271_paths[input->x271_count++] = path;
    return X12_OK;
}

int journal_builder_input_add_834(journal_builder_input_t *input, const char *path)
{
    if (input == NULL || path == NULL ||
        input->x834_count >= JOURNAL_BUILDER_MAX_INPUT_FILES) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    input->x834_paths[input->x834_count++] = path;
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

static int append_x12_file(
    FILE *fp,
    const char *path,
    const char *type,
    int include_phi,
    const char *run_id,
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
    event_writer_set_run_id(&writer, run_id);
    rc = event_writer_set_binary_journal(&writer, 1);
    if (rc != X12_OK) {
        (void)event_writer_close(&writer);
        x12_document_free(&doc);
        return rc;
    }
    event_writer_set_phi_vault(&writer, phi_vault, path);

    if (strcmp(type, "270") == 0) {
        rc = x12_map_270_document(&doc, &writer);
    } else if (strcmp(type, "271") == 0) {
        rc = x12_map_271_document(&doc, &writer);
    } else if (strcmp(type, "834") == 0) {
        rc = x12_map_834_document(&doc, &writer);
    } else if (strcmp(type, "837") == 0) {
        rc = x12_map_837_document(&doc, &writer);
    } else if (strcmp(type, "835") == 0) {
        rc = x12_map_835_document(&doc, &writer);
    } else {
        rc = X12_ERR_UNSUPPORTED;
    }

    {
        int close_rc = event_writer_close(&writer);
        if (close_rc != X12_OK && rc == X12_OK) {
            rc = close_rc;
        }
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
    char generated_run_id[96];
    const char *run_id;
    size_t i;
    int rc = X12_OK;
    int owns_file = 0;

    if (input == NULL || out_path == NULL || strcmp(out_path, "-") == 0) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    run_id = input->run_id;
    if (run_id == NULL || run_id[0] == '\0') {
        rc = scribe_run_id_generate(generated_run_id, sizeof(generated_run_id));
        if (rc != X12_OK) {
            return rc;
        }
        run_id = generated_run_id;
    }

    fp = fopen(out_path, "wb");
    owns_file = 1;
    if (fp == NULL) {
        return X12_ERR_IO;
    }
    rc = journal_write_header(fp);
    if (rc != X12_OK) {
        (void)fclose(fp);
        return rc;
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

    for (i = 0u; i < input->x837_count && rc == X12_OK; i++) {
        rc = append_x12_file(
            fp,
            input->x837_paths[i],
            "837",
            input->include_phi,
            run_id,
            phi_vault_ptr
        );
    }
    for (i = 0u; i < input->x835_count && rc == X12_OK; i++) {
        rc = append_x12_file(
            fp,
            input->x835_paths[i],
            "835",
            input->include_phi,
            run_id,
            phi_vault_ptr
        );
    }
    for (i = 0u; i < input->x834_count && rc == X12_OK; i++) {
        rc = append_x12_file(
            fp,
            input->x834_paths[i],
            "834",
            input->include_phi,
            run_id,
            phi_vault_ptr
        );
    }
    for (i = 0u; i < input->x270_count && rc == X12_OK; i++) {
        rc = append_x12_file(
            fp,
            input->x270_paths[i],
            "270",
            input->include_phi,
            run_id,
            phi_vault_ptr
        );
    }
    for (i = 0u; i < input->x271_count && rc == X12_OK; i++) {
        rc = append_x12_file(
            fp,
            input->x271_paths[i],
            "271",
            input->include_phi,
            run_id,
            phi_vault_ptr
        );
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
        } else if (strcmp(argv[i], "--270") == 0) {
            if (i + 1 >= argc ||
                journal_builder_input_add_270(&input, argv[++i]) != X12_OK) {
                return -1;
            }
        } else if (strcmp(argv[i], "--271") == 0) {
            if (i + 1 >= argc ||
                journal_builder_input_add_271(&input, argv[++i]) != X12_OK) {
                return -1;
            }
        } else if (strcmp(argv[i], "--834") == 0) {
            if (i + 1 >= argc ||
                journal_builder_input_add_834(&input, argv[++i]) != X12_OK) {
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
        } else if (strcmp(argv[i], "--run-id") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            input.run_id = argv[++i];
        } else {
            return -1;
        }
    }

    if (out_path == NULL ||
        (input.x270_count == 0u &&
         input.x271_count == 0u &&
         input.x834_count == 0u &&
         input.x837_count == 0u &&
         input.x835_count == 0u)) {
        return -1;
    }

    return journal_builder_build(&input, out_path);
}
