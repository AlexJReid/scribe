#ifndef SCRIBE_BALANCE_PROJECTOR_H
#define SCRIBE_BALANCE_PROJECTOR_H

#include "x12_parser.h"

#include <stddef.h>

typedef struct {
    const char *journal_path;
    const char *encounter_id;
    int include_phi;
} balance_projector_input_t;

void balance_projector_input_init(balance_projector_input_t *input);

int balance_projector_project(
    const balance_projector_input_t *input,
    const char *out_path
);

int balance_projector_run_cli(int argc, char **argv);

#endif
