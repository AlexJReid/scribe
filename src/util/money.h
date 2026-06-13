#ifndef SCRIBE_MONEY_H
#define SCRIBE_MONEY_H

#include <stddef.h>

/*
 * Money is handled as signed integer cents to keep ledger arithmetic exact.
 *
 * scribe_money_parse converts a money string ("123.45", "-7", "0.05") into
 * signed cents. It is strict: trailing non-numeric characters, a lone "-", or
 * "." with no digits are rejected (returns non-zero). A third fractional digit
 * is rounded half-up. Overflow is detected and reported. An empty or NULL
 * string is treated as "absent" -> 0 cents with a success return, matching the
 * way optional snapshot fields are read.
 *
 * Returns 0 (X12_OK) on success, or a negative X12_ERR_* code on a malformed
 * or overflowing value.
 */
int scribe_money_parse(const char *value, long long *out_cents);

/* Format signed cents as a money string ("123.45", "-7.00") into a
 * fixed-size buffer. Always NUL-terminated when out_len > 0. */
void scribe_money_format(long long cents, char *out, size_t out_len);

#endif
