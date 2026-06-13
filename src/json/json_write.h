#ifndef SCRIBE_JSON_WRITE_H
#define SCRIBE_JSON_WRITE_H

#include "x12_parser.h"

#include "yyjson.h"

#include <stddef.h>
#include <stdio.h>

typedef struct
{
    yyjson_mut_doc *doc;
    yyjson_mut_val *root;
} json_writer_t;

int json_writer_init_object(json_writer_t *writer);
void json_writer_free(json_writer_t *writer);

yyjson_mut_val *json_writer_root(json_writer_t *writer);
yyjson_mut_val *json_writer_add_object(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key);
yyjson_mut_val *json_writer_add_array(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key);
yyjson_mut_val *json_writer_array_add_object(
    json_writer_t *writer,
    yyjson_mut_val *arr);

int json_writer_add_string(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    const char *value);
int json_writer_add_bool(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    int value);
int json_writer_add_size(
    json_writer_t *writer,
    yyjson_mut_val *obj,
    const char *key,
    size_t value);
int json_writer_array_add_string(
    json_writer_t *writer,
    yyjson_mut_val *arr,
    const char *value);
int json_writer_array_add_size(
    json_writer_t *writer,
    yyjson_mut_val *arr,
    size_t value);

int json_writer_write_cstring(
    const json_writer_t *writer,
    char *out,
    size_t out_len);
int json_writer_write_fp(
    const json_writer_t *writer,
    FILE *fp,
    int newline);

#endif
