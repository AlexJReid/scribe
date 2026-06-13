#ifndef SCRIBE_DELTA_EXPORTER_H
#define SCRIBE_DELTA_EXPORTER_H

#include "x12_parser.h"

#include <stddef.h>

typedef struct
{
    const char *read_store_path;
    const char *out_path;
    long long after_sequence;
    size_t limit;
} scribe_delta_exporter_input_t;

void scribe_delta_exporter_input_init(scribe_delta_exporter_input_t *input);

int scribe_delta_exporter_export(const scribe_delta_exporter_input_t *input);
int scribe_delta_exporter_run_cli(int argc, char **argv);

#endif
