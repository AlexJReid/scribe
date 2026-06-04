#include "json_scan.h"

#include <stdio.h>
#include <string.h>

static const char *find_value(const char *line, const char *key)
{
    char pattern[96];
    const char *cursor;
    int written;

    if (line == NULL || key == NULL) {
        return NULL;
    }

    written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(pattern)) {
        return NULL;
    }

    cursor = strstr(line, pattern);
    if (cursor == NULL) {
        return NULL;
    }
    cursor += strlen(pattern);
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor != ':') {
        return NULL;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    return cursor;
}

int json_get_string(
    const char *line,
    const char *key,
    char *out,
    size_t out_len
)
{
    const char *cursor;
    const char *start;
    size_t len = 0u;

    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';

    cursor = find_value(line, key);
    if (cursor == NULL || *cursor != '"') {
        return 0;
    }
    cursor++;
    start = cursor;

    while (*cursor != '\0' && *cursor != '"' && len + 1u < out_len) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
        }
        cursor++;
        len = (size_t)(cursor - start);
    }
    if (*cursor != '"' || len >= out_len) {
        return 0;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

int json_get_bool(const char *line, const char *key, int *out)
{
    const char *cursor;

    if (out == NULL) {
        return 0;
    }

    cursor = find_value(line, key);
    if (cursor == NULL) {
        return 0;
    }

    if (strncmp(cursor, "true", 4u) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(cursor, "false", 5u) == 0) {
        *out = 0;
        return 1;
    }

    return 0;
}

int json_get_number_text(
    const char *line,
    const char *key,
    char *out,
    size_t out_len
)
{
    const char *cursor;
    const char *start;
    size_t len = 0u;

    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';

    cursor = find_value(line, key);
    if (cursor == NULL) {
        return 0;
    }

    start = cursor;
    while (*cursor >= '0' && *cursor <= '9' && len + 1u < out_len) {
        cursor++;
        len = (size_t)(cursor - start);
    }
    if (len == 0u || len >= out_len) {
        return 0;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

int json_get_array_string_at(
    const char *line,
    const char *key,
    size_t index,
    char *out,
    size_t out_len
)
{
    char pattern[96];
    const char *cursor;
    const char *start;
    size_t current_index = 0u;
    size_t len;
    int written;

    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';

    if (line == NULL || key == NULL) {
        return 0;
    }

    written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(pattern)) {
        return 0;
    }

    cursor = strstr(line, pattern);
    if (cursor == NULL) {
        return 0;
    }
    cursor = strchr(cursor, '[');
    if (cursor == NULL) {
        return 0;
    }
    cursor++;

    while (*cursor != '\0' && *cursor != ']') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
            cursor++;
        }
        if (*cursor == ']') {
            break;
        }
        if (*cursor != '"') {
            return 0;
        }
        cursor++;
        start = cursor;
        len = 0u;
        while (*cursor != '\0' && *cursor != '"' && len + 1u < out_len) {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor++;
            }
            cursor++;
            len = (size_t)(cursor - start);
        }
        if (*cursor != '"' || len >= out_len) {
            return 0;
        }
        if (current_index == index) {
            memcpy(out, start, len);
            out[len] = '\0';
            return 1;
        }
        current_index++;
        cursor++;
    }

    return 0;
}
