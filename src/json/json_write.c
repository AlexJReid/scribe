#include "json_write.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int json_writer_check(int ok)
{
    return ok ? X12_OK : X12_ERR_NO_MEMORY;
}

int json_writer_init_object(json_writer_t *writer)
{
    yyjson_mut_doc *doc;
    yyjson_mut_val *root;

    if (writer == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    writer->doc = NULL;
    writer->root = NULL;

    doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    root = yyjson_mut_obj(doc);
    if (root == NULL)
    {
        yyjson_mut_doc_free(doc);
        return X12_ERR_NO_MEMORY;
    }

    yyjson_mut_doc_set_root(doc, root);
    writer->doc = doc;
    writer->root = root;
    return X12_OK;
}

void json_writer_free(json_writer_t *writer)
{
    if (writer == NULL)
    {
        return;
    }

    if (writer->doc != NULL)
    {
        yyjson_mut_doc_free(writer->doc);
    }
    writer->doc = NULL;
    writer->root = NULL;
}

yyjson_mut_val *json_writer_root(json_writer_t *writer)
{
    return writer != NULL ? writer->root : NULL;
}

yyjson_mut_val *json_writer_add_object(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key)
{
    if (writer == NULL || writer->doc == NULL || obj == NULL || key == NULL)
    {
        return NULL;
    }

    return yyjson_mut_obj_add_obj(writer->doc, obj, key);
}

yyjson_mut_val *json_writer_add_array(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key)
{
    if (writer == NULL || writer->doc == NULL || obj == NULL || key == NULL)
    {
        return NULL;
    }

    return yyjson_mut_obj_add_arr(writer->doc, obj, key);
}

yyjson_mut_val *json_writer_array_add_object(
    json_writer_t *writer,
    yyjson_mut_val *arr)
{
    if (writer == NULL || writer->doc == NULL || arr == NULL)
    {
        return NULL;
    }

    return yyjson_mut_arr_add_obj(writer->doc, arr);
}

int json_writer_add_string(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    const char *value)
{
    if (value == NULL)
    {
        value = "";
    }
    if (writer == NULL || writer->doc == NULL || obj == NULL || key == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return json_writer_check(
        yyjson_mut_obj_add_strcpy(writer->doc, obj, key, value));
}

int json_writer_add_bool(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    int value)
{
    if (writer == NULL || writer->doc == NULL || obj == NULL || key == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return json_writer_check(
        yyjson_mut_obj_add_bool(writer->doc, obj, key, value ? true : false));
}

int json_writer_add_size(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    size_t value)
{
    if (writer == NULL || writer->doc == NULL || obj == NULL || key == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return json_writer_check(
        yyjson_mut_obj_add_uint(writer->doc, obj, key, (uint64_t)value));
}

int json_writer_array_add_string(
    json_writer_t *writer,
    yyjson_mut_val *arr,
    const char *value)
{
    if (value == NULL)
    {
        value = "";
    }
    if (writer == NULL || writer->doc == NULL || arr == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return json_writer_check(
        yyjson_mut_arr_add_strcpy(writer->doc, arr, value));
}

int json_writer_array_add_size(
    json_writer_t *writer,
    yyjson_mut_val *arr,
    size_t value)
{
    if (writer == NULL || writer->doc == NULL || arr == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return json_writer_check(
        yyjson_mut_arr_add_uint(writer->doc, arr, (uint64_t)value));
}

int json_writer_write_cstring(
    const json_writer_t *writer,
    char *out,
    size_t out_len)
{
    char *json;
    size_t len;

    if (writer == NULL || writer->doc == NULL || out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';

    json = yyjson_mut_write(writer->doc, 0, &len);
    if (json == NULL)
    {
        return X12_ERR_NO_MEMORY;
    }
    if (len >= out_len)
    {
        free(json);
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, json, len);
    out[len] = '\0';
    free(json);
    return X12_OK;
}

int json_writer_write_fp(
    const json_writer_t *writer,
    FILE *fp,
    int newline)
{
    yyjson_write_flag flags = 0;

    if (writer == NULL || writer->doc == NULL || fp == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (newline)
    {
        flags |= YYJSON_WRITE_NEWLINE_AT_END;
    }
    if (!yyjson_mut_write_fp(fp, writer->doc, flags, NULL, NULL))
    {
        return X12_ERR_IO;
    }

    return X12_OK;
}

int json_read_string(
    yyjson_val *obj,
    const char *key,
    char *out,
    size_t out_len)
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

    value = yyjson_obj_get(obj, key);
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
