#ifndef SCRIBE_JOURNAL_BUILDER_H
#define SCRIBE_JOURNAL_BUILDER_H

#include "x12_parser.h"

#include <stddef.h>

#define JOURNAL_BUILDER_MAX_INPUT_FILES 16u

typedef struct {
    const char *x270_paths[JOURNAL_BUILDER_MAX_INPUT_FILES];
    size_t x270_count;
    const char *x270_list_path;
    const char *x271_paths[JOURNAL_BUILDER_MAX_INPUT_FILES];
    size_t x271_count;
    const char *x271_list_path;
    const char *x834_paths[JOURNAL_BUILDER_MAX_INPUT_FILES];
    size_t x834_count;
    const char *x834_list_path;
    const char *x837_paths[JOURNAL_BUILDER_MAX_INPUT_FILES];
    size_t x837_count;
    const char *x837_list_path;
    const char *x835_paths[JOURNAL_BUILDER_MAX_INPUT_FILES];
    size_t x835_count;
    const char *x835_list_path;
    int include_phi;
    int append;
    const char *phi_vault_path;
    const char *run_id;
} journal_builder_input_t;

void journal_builder_input_init(journal_builder_input_t *input);
int journal_builder_input_add_270(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_271(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_834(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_837(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_835(journal_builder_input_t *input, const char *path);

int journal_builder_build(
    const journal_builder_input_t *input,
    const char *out_path
);

int journal_builder_run_cli(int argc, char **argv);

#endif
