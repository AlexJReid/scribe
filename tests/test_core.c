#include "event_writer.h"
#include "json_scan.h"
#include "test_support.h"
#include "tokenise.h"

#include <stdio.h>
#include <string.h>

static int test_json_escaping(void)
{
    FILE *fp;
    char output[128];
    const char input[] = "a\"b\\c\n";

    fp = tmpfile();
    REQUIRE(fp != NULL);
    REQUIRE_OK(event_writer_write_json_string(fp, input, strlen(input)));
    REQUIRE(fflush(fp) == 0);
    REQUIRE(fseek(fp, 0, SEEK_SET) == 0);
    REQUIRE(fgets(output, sizeof(output), fp) != NULL);
    REQUIRE_STR(output, "\"a\\\"b\\\\c\\n\"");
    REQUIRE(fclose(fp) == 0);

    return 0;
}

static int test_json_scan_unescapes_strings(void)
{
    char value[128];
    int synthetic = 0;
    const char line[] =
        "{\"description\":\"a\\\"b\\\\c\\n\","
        "\"synthetic\":true,"
        "\"raw_elements\":[\"x\",\"y\\tz\"]}";

    REQUIRE(json_get_string(line, "description", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "a\"b\\c\n");
    REQUIRE(json_get_bool(line, "synthetic", &synthetic) == 1);
    REQUIRE(synthetic == 1);
    REQUIRE(json_get_array_string_at(line, "raw_elements", 1u, value, sizeof(value)) == 1);
    REQUIRE_STR(value, "y\tz");

    return 0;
}

static int test_tokenise_format(void)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t raw;

    raw.ptr = "ABC123";
    raw.len = strlen(raw.ptr);

    REQUIRE_OK(tokenise_value(TOK_CLAIM_ID, raw, token, sizeof(token)));
    REQUIRE_STR(token, "f58260c3ffcdfaff81c42473f162e481");

    return 0;
}

int main(void)
{
    REQUIRE(test_json_escaping() == 0);
    REQUIRE(test_json_scan_unescapes_strings() == 0);
    REQUIRE(test_tokenise_format() == 0);
    return 0;
}
