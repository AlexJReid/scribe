#include "json_read.h"

#include "x12_parser.h"

#include "yyjson.h"

#include <stdlib.h>
#include <string.h>

struct json_reader
{
    yyjson_doc *doc;
};

int json_reader_open(
    json_reader_t **reader,
    const char *json,
    size_t len,
    json_value_t *out_root)
{
    json_reader_t *r;
    yyjson_val *root;

    if (out_root != NULL)
    {
        *out_root = NULL;
    }
    if (reader == NULL || json == NULL || out_root == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *reader = NULL;

    r = calloc(1u, sizeof(*r));
    if (r == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }

    r->doc = yyjson_read(json, len, 0);
    if (r->doc == NULL)
    {
        free(r);
        return X12_ERR_INVALID_ARGUMENT;
    }

    root = yyjson_doc_get_root(r->doc);
    if (root == NULL)
    {
        yyjson_doc_free(r->doc);
        free(r);
        return X12_ERR_INVALID_ARGUMENT;
    }

    *reader = r;
    *out_root = (json_value_t)root;
    return X12_OK;
}

void json_reader_close(json_reader_t *reader)
{
    if (reader == NULL)
    {
        return;
    }
    if (reader->doc != NULL)
    {
        yyjson_doc_free(reader->doc);
    }
    free(reader);
}

json_value_t json_object_get(json_value_t obj, const char *key)
{
    if (obj == NULL || key == NULL)
    {
        return NULL;
    }
    return (json_value_t)yyjson_obj_get((yyjson_val *)obj, key);
}

int json_is_object(json_value_t value)
{
    return value != NULL && yyjson_is_obj((yyjson_val *)value);
}

int json_is_array(json_value_t value)
{
    return value != NULL && yyjson_is_arr((yyjson_val *)value);
}

int json_read_string(void *obj, const char *key, char *out, size_t out_len)
{
    yyjson_val *value;
    const char *str;
    size_t len;

    if (out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    if (obj == NULL || key == NULL)
    {
        return X12_OK;
    }

    value = yyjson_obj_get((yyjson_val *)obj, key);
    if (value == NULL || !yyjson_is_str(value))
    {
        return X12_OK;
    }

    str = yyjson_get_str(value);
    len = yyjson_get_len(value);
    if (len >= out_len)
    {
        len = out_len - 1u;
    }
    memcpy(out, str, len);
    out[len] = '\0';
    return X12_OK;
}

int json_object_get_string(
    json_value_t obj,
    const char *key,
    char *out,
    size_t out_len)
{
    return json_read_string((void *)obj, key, out, out_len);
}

size_t json_object_get_size(json_value_t obj, const char *key, size_t fallback)
{
    yyjson_val *value;

    if (obj == NULL || key == NULL)
    {
        return fallback;
    }
    value = yyjson_obj_get((yyjson_val *)obj, key);
    if (value == NULL || !yyjson_is_uint(value))
    {
        return fallback;
    }
    return (size_t)yyjson_get_uint(value);
}

int json_object_get_bool(json_value_t obj, const char *key, int fallback)
{
    yyjson_val *value;

    if (obj == NULL || key == NULL)
    {
        return fallback;
    }
    value = yyjson_obj_get((yyjson_val *)obj, key);
    if (value == NULL || !yyjson_is_bool(value))
    {
        return fallback;
    }
    return yyjson_get_bool(value) ? 1 : 0;
}

size_t json_array_count(json_value_t arr)
{
    if (arr == NULL || !yyjson_is_arr((yyjson_val *)arr))
    {
        return 0u;
    }
    return yyjson_arr_size((yyjson_val *)arr);
}

json_value_t json_array_get(json_value_t arr, size_t i)
{
    if (arr == NULL || !yyjson_is_arr((yyjson_val *)arr))
    {
        return NULL;
    }
    return (json_value_t)yyjson_arr_get((yyjson_val *)arr, i);
}

int json_value_as_string(json_value_t value, char *out, size_t out_len)
{
    const char *str;
    size_t len;

    if (out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    if (value == NULL || !yyjson_is_str((yyjson_val *)value))
    {
        return X12_OK;
    }

    str = yyjson_get_str((yyjson_val *)value);
    len = yyjson_get_len((yyjson_val *)value);
    if (len >= out_len)
    {
        len = out_len - 1u;
    }
    memcpy(out, str, len);
    out[len] = '\0';
    return X12_OK;
}
