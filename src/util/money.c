#include "money.h"

#include "x12_parser.h"

#include <limits.h>
#include <stdio.h>

int scribe_money_parse(const char *value, long long *out_cents)
{
    long long dollars = 0;
    long long cents = 0;
    int negative = 0;
    int cent_digits = 0;
    int dollar_digits = 0;
    int round_up = 0;
    const char *cursor;

    if (out_cents == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out_cents = 0;
    if (value == NULL || value[0] == '\0') {
        return X12_OK;
    }

    cursor = value;
    if (*cursor == '-') {
        negative = 1;
        cursor++;
    }

    while (*cursor >= '0' && *cursor <= '9') {
        if (dollars > (LLONG_MAX - 9) / 10) {
            return X12_ERR_INVALID_ARGUMENT;
        }
        dollars = dollars * 10 + (long long)(*cursor - '0');
        dollar_digits++;
        cursor++;
    }
    if (*cursor == '.') {
        cursor++;
        while (*cursor >= '0' && *cursor <= '9') {
            if (cent_digits < 2) {
                cents = cents * 10 + (long long)(*cursor - '0');
                cent_digits++;
            } else if (cent_digits == 2) {
                /* Third fractional digit decides half-up rounding. */
                round_up = (*cursor >= '5');
                cent_digits++;
            }
            cursor++;
        }
    }

    /* Require at least one digit somewhere and no trailing garbage. */
    if (dollar_digits == 0 && cent_digits == 0) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (*cursor != '\0') {
        return X12_ERR_INVALID_ARGUMENT;
    }

    while (cent_digits < 2) {
        cents *= 10;
        cent_digits++;
    }
    cents += round_up;

    if (dollars > (LLONG_MAX - cents) / 100) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out_cents = dollars * 100 + cents;
    if (negative) {
        *out_cents = -*out_cents;
    }
    return X12_OK;
}

void scribe_money_format(long long cents, char *out, size_t out_len)
{
    long long abs_cents = cents;
    const char *sign = "";

    if (cents < 0) {
        sign = "-";
        abs_cents = -cents;
    }

    (void)snprintf(out, out_len, "%s%lld.%02lld", sign, abs_cents / 100, abs_cents % 100);
}
