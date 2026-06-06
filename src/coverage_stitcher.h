#ifndef SCRIBE_COVERAGE_STITCHER_H
#define SCRIBE_COVERAGE_STITCHER_H

#include "x12_parser.h"

typedef struct {
    const char *journal_path;
    const char *out_path;
    const char *read_store_path;
    const char *phi_vault_path;
    const char *run_id;
    int include_phi;
} coverage_stitcher_input_t;

void coverage_stitcher_input_init(coverage_stitcher_input_t *input);

int coverage_stitcher_stitch(const coverage_stitcher_input_t *input);
int coverage_stitcher_run_cli(int argc, char **argv);

#endif
