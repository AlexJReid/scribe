#ifndef SCRIBE_JOURNAL_BUILDER_H
#define SCRIBE_JOURNAL_BUILDER_H

#include "x12_parser.h"

#include <stddef.h>

#define JOURNAL_BUILDER_MAX_INPUT_FILES 80u
#define JOURNAL_BUILDER_MAX_LISTS 16u

typedef struct
{
    const char *type;
    const char *path;
} journal_builder_file_t;

typedef struct
{
    journal_builder_file_t files[JOURNAL_BUILDER_MAX_INPUT_FILES];
    size_t file_count;
    journal_builder_file_t lists[JOURNAL_BUILDER_MAX_LISTS];
    size_t list_count;
    int include_phi;
    int append;
    int compress_zstd;
    int zstd_level;
    const char *phi_vault_path;
    const char *run_id;
    const char *source_root;
} journal_builder_input_t;

void journal_builder_input_init(journal_builder_input_t *input);

int journal_builder_input_add(
    journal_builder_input_t *input,
    const char *type,
    const char *path);

int journal_builder_input_add_list(
    journal_builder_input_t *input,
    const char *type,
    const char *list_path);

/* Backwards-compatible per-type shims for test convenience. */
int journal_builder_input_add_270(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_271(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_834(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_837(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_835(journal_builder_input_t *input, const char *path);

int journal_builder_build(
    const journal_builder_input_t *input,
    const char *out_path);

int journal_builder_run_cli(int argc, char **argv);

#endif
