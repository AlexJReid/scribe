#ifndef SCRIBE_JSON_SCAN_H
#define SCRIBE_JSON_SCAN_H

#include <stddef.h>

int json_get_string(
    const char *line,
    const char *key,
    char *out,
    size_t out_len
);

int json_get_bool(const char *line, const char *key, int *out);

int json_get_number_text(
    const char *line,
    const char *key,
    char *out,
    size_t out_len
);

int json_get_array_string_at(
    const char *line,
    const char *key,
    size_t index,
    char *out,
    size_t out_len
);

#endif
