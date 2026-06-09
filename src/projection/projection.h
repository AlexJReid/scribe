#ifndef SCRIBE_PROJECTION_H
#define SCRIBE_PROJECTION_H

#include <stdio.h>

typedef int (*projection_cli_run_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *summary;
    projection_cli_run_fn run_cli;
} projection_plugin_t;

const projection_plugin_t *projection_find(const char *name);
void projection_write_usage(FILE *fp);

#endif
