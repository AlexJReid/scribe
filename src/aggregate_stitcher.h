#ifndef SCRIBE_AGGREGATE_STITCHER_H
#define SCRIBE_AGGREGATE_STITCHER_H

#include "x12_parser.h"

typedef struct {
    const char *journal_path;
    const char *out_path;
    const char *read_store_path;
    const char *encounter_id;
    int include_phi;
} aggregate_stitcher_input_t;

void aggregate_stitcher_input_init(aggregate_stitcher_input_t *input);

int aggregate_stitcher_stitch(const aggregate_stitcher_input_t *input);
int aggregate_stitcher_run_cli(int argc, char **argv);

#endif
