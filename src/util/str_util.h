#ifndef SCRIBE_STR_UTIL_H
#define SCRIBE_STR_UTIL_H

#include <stddef.h>

/* Copy a NUL-terminated string into a fixed-size buffer, truncating if
 * necessary. A NULL value is treated as the empty string. The output is
 * always NUL-terminated when out_len > 0. */
void scribe_copy_cstr(char *out, size_t out_len, const char *value);

/* Like scribe_copy_cstr but copies at most value_len bytes from value
 * (which need not be NUL-terminated). */
void scribe_copy_str_slice(char *out, size_t out_len, const char *value, size_t value_len);

#endif
