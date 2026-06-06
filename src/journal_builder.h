#ifndef SCRIBE_JOURNAL_BUILDER_H
#define SCRIBE_JOURNAL_BUILDER_H

#include "x12_parser.h"

#include <stddef.h>

#define JOURNAL_BUILDER_MAX_INPUT_FILES 16u

typedef struct {
    const char *charges_paths[JOURNAL_BUILDER_MAX_INPUT_FILES];
    size_t charges_count;
    const char *x837_paths[JOURNAL_BUILDER_MAX_INPUT_FILES];
    size_t x837_count;
    const char *x835_paths[JOURNAL_BUILDER_MAX_INPUT_FILES];
    size_t x835_count;
    int include_phi;
    const char *phi_vault_path;
    const char *run_id;
} journal_builder_input_t;

void journal_builder_input_init(journal_builder_input_t *input);
int journal_builder_input_add_charges(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_837(journal_builder_input_t *input, const char *path);
int journal_builder_input_add_835(journal_builder_input_t *input, const char *path);

int journal_builder_build(
    const journal_builder_input_t *input,
    const char *out_path
);

int journal_builder_run_cli(int argc, char **argv);

#endif
