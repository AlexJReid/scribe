#include "str_util.h"

#include <string.h>

void scribe_copy_cstr(char *out, size_t out_len, const char *value)
{
    size_t len;

    if (out == NULL || out_len == 0u) {
        return;
    }
    if (value == NULL) {
        value = "";
    }

    len = strlen(value);
    if (len >= out_len) {
        len = out_len - 1u;
    }
    memcpy(out, value, len);
    out[len] = '\0';
}

void scribe_copy_str_slice(char *out, size_t out_len, const char *value, size_t value_len)
{
    if (out == NULL || out_len == 0u) {
        return;
    }
    if (value == NULL) {
        value = "";
        value_len = 0u;
    }
    if (value_len >= out_len) {
        value_len = out_len - 1u;
    }
    memcpy(out, value, value_len);
    out[value_len] = '\0';
}
