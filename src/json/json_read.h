#ifndef SCRIBE_JSON_READ_H
#define SCRIBE_JSON_READ_H

#include <stddef.h>

/* Read mirror of json_write. Confines the JSON parser to src/json/: callers
 * walk a parsed document through opaque json_value_t handles and never include
 * yyjson.h. Every accessor is guarded -- a missing key or wrong-typed value
 * yields the supplied fallback (or an empty string / NULL handle), never a
 * crash -- so domain code reads a snapshot without open-coding type checks.
 *
 * Lifetime: the document returned by json_reader_open owns all json_value_t
 * handles derived from it; they are valid until json_reader_close. */

typedef struct json_reader json_reader_t;

/* Opaque handle to a value inside an open document. Compares against NULL. */
typedef const void *json_value_t;

/* Parse json[0..len) into reader and yield the root value via out_root. On
 * success returns X12_OK with a non-NULL *out_root; the caller must pair every
 * successful open with json_reader_close. On parse failure or a non-NULL but
 * empty document, returns an error and leaves *out_root NULL. */
int json_reader_open(
    json_reader_t **reader,
    const char *json,
    size_t len,
    json_value_t *out_root);

/* Free the document and everything derived from it. Safe on NULL / a reader
 * whose open failed. */
void json_reader_close(json_reader_t *reader);

/* Member named key of an object value. NULL if obj is NULL, not an object, or
 * has no such member. The result is not type-checked -- guard with the
 * json_is_* / json_get_* helpers below. */
json_value_t json_object_get(json_value_t obj, const char *key);

int json_is_object(json_value_t value);
int json_is_array(json_value_t value);

/* Copy string member key into out (always NUL-terminated when out_len > 0),
 * truncating to fit. Missing key or non-string value -> empty string + X12_OK;
 * only a NULL/zero-length out is an error. */
int json_object_get_string(
    json_value_t obj,
    const char *key,
    char *out,
    size_t out_len);

/* Read string member key into a fixed-size char-array field, deriving the
 * buffer size from the field itself so the destination is named exactly once.
 * dst MUST be a real array (char dst[N]) -- a pointer would take sizeof the
 * pointer, not the buffer. Use this for the common "copy into a struct field"
 * case instead of repeating `field, sizeof(field)`. */
#define JSON_GET_FIELD(obj, key, dst) \
    json_object_get_string((obj), (key), (dst), sizeof(dst))

/* Unsigned-integer member key, or fallback when missing / not a uint. */
size_t json_object_get_size(json_value_t obj, const char *key, size_t fallback);

/* Boolean member key (returned as 0/1), or fallback when missing / not a bool. */
int json_object_get_bool(json_value_t obj, const char *key, int fallback);

/* Element count of an array value; 0 if arr is NULL or not an array. */
size_t json_array_count(json_value_t arr);

/* Element i of an array value, or NULL if arr is not an array or i is out of
 * range. */
json_value_t json_array_get(json_value_t arr, size_t i);

/* Copy a bare (non-object) string array element into out (always
 * NUL-terminated when out_len > 0), truncating to fit. A non-string value ->
 * empty string. Returns X12_OK except for a NULL/zero-length out. */
int json_value_as_string(json_value_t value, char *out, size_t out_len);

/* Lower-level string copy used by json_object_get_string. Takes a raw yyjson
 * value (declared as void* so this header stays parser-free); callers in
 * src/json/ pass a yyjson_val*. Prefer json_object_get_string outside src/json/.
 * Copies string member key into out (always NUL-terminated when out_len > 0),
 * truncating to fit. Missing key or non-string value -> empty string + X12_OK;
 * only a NULL/zero-length out is an error. */
int json_read_string(void *obj, const char *key, char *out, size_t out_len);

#endif
