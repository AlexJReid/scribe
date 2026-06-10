#ifndef SCRIBE_TEST_SUPPORT_H
#define SCRIBE_TEST_SUPPORT_H

#include "x12_parser.h"

#include <stdio.h>
#include <string.h>

#ifndef TEST_FIXTURE_DIR
#define TEST_FIXTURE_DIR "tests/fixtures"
#endif

#ifndef TEST_OUTPUT_DIR
#define TEST_OUTPUT_DIR "."
#endif

#define REQUIRE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: requirement failed: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static inline int require_ok(int actual, const char *expr, const char *file, int line)
{
    if (actual == X12_OK) {
        return 0;
    }

    fprintf(
        stderr,
        "%s:%d: requirement failed: %s == X12_OK (actual %d)\n",
        file,
        line,
        expr,
        actual
    );
    return 1;
}

#define REQUIRE_OK(expr) do { \
    int require_ok_actual = (expr); \
    if (require_ok(require_ok_actual, #expr, __FILE__, __LINE__)) { \
        return 1; \
    } \
} while (0)

static inline int require_str_equal(
    const char *actual,
    const char *expected,
    const char *actual_expr,
    const char *expected_expr,
    const char *file,
    int line
)
{
    if (actual != NULL && expected != NULL && strcmp(actual, expected) == 0) {
        return 0;
    }

    fprintf(
        stderr,
        "%s:%d: requirement failed: %s == %s (actual \"%s\", expected \"%s\")\n",
        file,
        line,
        actual_expr,
        expected_expr,
        actual == NULL ? "(null)" : actual,
        expected == NULL ? "(null)" : expected
    );
    return 1;
}

#define REQUIRE_STR(actual, expected) do { \
    if (require_str_equal((actual), (expected), #actual, #expected, __FILE__, __LINE__)) { \
        return 1; \
    } \
} while (0)

static inline int make_path(char *out, size_t out_len, const char *base, const char *name)
{
    int written = snprintf(out, out_len, "%s/%s", base, name);

    if (written < 0 || (size_t)written >= out_len) {
        return 1;
    }

    return 0;
}

static inline int read_file_text(const char *path, char *out, size_t out_len)
{
    FILE *fp;
    size_t read_len;

    if (out_len == 0u) {
        return 1;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 1;
    }

    read_len = fread(out, 1u, out_len - 1u, fp);
    if (ferror(fp)) {
        (void)fclose(fp);
        return 1;
    }

    out[read_len] = '\0';
    if (fclose(fp) != 0) {
        return 1;
    }

    return 0;
}

static inline size_t count_substring(const char *text, const char *needle)
{
    size_t count = 0u;
    size_t needle_len;
    const char *cursor;

    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return 0u;
    }

    cursor = text;
    needle_len = strlen(needle);
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += needle_len;
    }

    return count;
}

#endif
