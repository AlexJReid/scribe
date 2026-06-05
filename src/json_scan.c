#include "json_scan.h"

#include "yyjson.h"

#include <stdlib.h>
#include <string.h>

static yyjson_val *read_key(
    const char *line,
    const char *key,
    yyjson_doc **out_doc
)
{
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *value;

    if (out_doc != NULL) {
        *out_doc = NULL;
    }
    if (line == NULL || key == NULL || out_doc == NULL) {
        return NULL;
    }

    doc = yyjson_read(line, strlen(line), 0);
    if (doc == NULL) {
        return NULL;
    }

    root = yyjson_doc_get_root(doc);
    value = yyjson_obj_get(root, key);
    if (value == NULL) {
        yyjson_doc_free(doc);
        return NULL;
    }

    *out_doc = doc;
    return value;
}

static int copy_json_string(yyjson_val *value, char *out, size_t out_len)
{
    const char *str;
    size_t len;

    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';

    if (!yyjson_is_str(value)) {
        return 0;
    }

    str = yyjson_get_str(value);
    len = yyjson_get_len(value);
    if (str == NULL || len >= out_len) {
        return 0;
    }

    memcpy(out, str, len);
    out[len] = '\0';
    return 1;
}

int json_get_string(
    const char *line,
    const char *key,
    char *out,
    size_t out_len
)
{
    yyjson_doc *doc;
    yyjson_val *value;
    int ok;

    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';

    value = read_key(line, key, &doc);
    if (value == NULL) {
        return 0;
    }

    ok = copy_json_string(value, out, out_len);
    yyjson_doc_free(doc);
    return ok;
}

int json_get_bool(const char *line, const char *key, int *out)
{
    yyjson_doc *doc;
    yyjson_val *value;

    if (out == NULL) {
        return 0;
    }

    value = read_key(line, key, &doc);
    if (value == NULL) {
        return 0;
    }

    if (!yyjson_is_bool(value)) {
        yyjson_doc_free(doc);
        return 0;
    }

    *out = yyjson_get_bool(value) ? 1 : 0;
    yyjson_doc_free(doc);
    return 1;
}

int json_get_number_text(
    const char *line,
    const char *key,
    char *out,
    size_t out_len
)
{
    yyjson_doc *doc;
    yyjson_val *value;
    char *json;
    size_t len;

    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';

    value = read_key(line, key, &doc);
    if (value == NULL) {
        return 0;
    }

    if (yyjson_is_str(value)) {
        int ok = copy_json_string(value, out, out_len);
        yyjson_doc_free(doc);
        return ok;
    }
    if (!yyjson_is_num(value)) {
        yyjson_doc_free(doc);
        return 0;
    }

    json = yyjson_val_write(value, 0, &len);
    if (json == NULL) {
        yyjson_doc_free(doc);
        return 0;
    }
    if (len >= out_len) {
        free(json);
        yyjson_doc_free(doc);
        return 0;
    }

    memcpy(out, json, len);
    out[len] = '\0';
    free(json);
    yyjson_doc_free(doc);
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
    yyjson_doc *doc;
    yyjson_val *array;
    yyjson_val *value;
    int ok;

    if (out == NULL || out_len == 0u) {
        return 0;
    }
    out[0] = '\0';

    array = read_key(line, key, &doc);
    if (array == NULL) {
        return 0;
    }
    if (!yyjson_is_arr(array)) {
        yyjson_doc_free(doc);
        return 0;
    }

    value = yyjson_arr_get(array, index);
    ok = copy_json_string(value, out, out_len);
    yyjson_doc_free(doc);
    return ok;
}
