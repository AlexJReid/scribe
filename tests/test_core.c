#include "event_writer.h"
#include "journal.h"
#include "json_scan.h"
#include "test_support.h"
#include "tokenise.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static int make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST ? 0 : 1;
#else
    return mkdir(path, 0777) == 0 || errno == EEXIST ? 0 : 1;
#endif
}

static int write_test_journal_segment(const char *path, const char *event_type)
{
    FILE *fp;
    journal_record_builder_t builder;
    int rc;

    fp = fopen(path, "wb");
    REQUIRE(fp != NULL);
    REQUIRE_OK(journal_write_header(fp));

    journal_record_builder_init(&builder);
    REQUIRE_OK(journal_record_builder_reset(&builder));
    REQUIRE_OK(journal_record_add_cstring(&builder, "event_type", event_type));
    REQUIRE_OK(journal_record_add_cstring(&builder, "source_transaction", "837"));
    REQUIRE_OK(journal_write_record(fp, &builder, NULL, NULL));
    journal_record_builder_free(&builder);

    rc = fclose(fp);
    REQUIRE(rc == 0);
    return 0;
}

static int test_journal_reader_carries_source_context(void)
{
    char journal_path[512];
    char value[512];
    FILE *fp;
    journal_record_builder_t builder;
    journal_reader_t reader;
    journal_event_t event;

    REQUIRE(make_path(journal_path, sizeof(journal_path), TEST_OUTPUT_DIR, "compact_context.journal") == 0);
    (void)remove(journal_path);

    fp = fopen(journal_path, "wb");
    REQUIRE(fp != NULL);
    REQUIRE_OK(journal_write_header(fp));

    journal_record_builder_init(&builder);
    REQUIRE_OK(journal_record_builder_reset(&builder));
    REQUIRE_OK(journal_record_add_cstring(&builder, "event_type", "FirstEvent"));
    REQUIRE_OK(journal_record_add_cstring(&builder, "source_file", "/inbound/drop-001.edi"));
    REQUIRE_OK(journal_record_add_cstring(&builder, "source_transaction", "837"));
    REQUIRE_OK(journal_record_add_cstring(&builder, "source_drop_id", "837:000000001:1:0001"));
    REQUIRE_OK(journal_record_add_cstring(&builder, "run_id", "ingest-run"));
    REQUIRE_OK(journal_record_add_cstring(&builder, "isa13", "000000001"));
    REQUIRE_OK(journal_record_add_cstring(&builder, "gs06", "1"));
    REQUIRE_OK(journal_record_add_cstring(&builder, "st02", "0001"));
    REQUIRE_OK(journal_record_add_u64(&builder, "source_segment_index", 8u));
    REQUIRE_OK(journal_record_add_u64(&builder, "source_byte_offset", 333u));
    REQUIRE_OK(journal_write_record(fp, &builder, NULL, NULL));

    REQUIRE_OK(journal_record_builder_reset(&builder));
    REQUIRE_OK(journal_record_add_cstring(&builder, "event_type", "SecondEvent"));
    REQUIRE_OK(journal_record_add_u64(&builder, "source_segment_index", 9u));
    REQUIRE_OK(journal_record_add_u64(&builder, "source_byte_offset", 369u));
    REQUIRE_OK(journal_write_record(fp, &builder, NULL, NULL));
    journal_record_builder_free(&builder);
    REQUIRE(fclose(fp) == 0);

    journal_reader_init(&reader);
    REQUIRE_OK(journal_reader_open(&reader, journal_path));
    REQUIRE_OK(journal_reader_next(&reader, &event));
    REQUIRE(event.record_len > 0u);
    REQUIRE(journal_event_get_string(&event, "source_file", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "/inbound/drop-001.edi");

    REQUIRE_OK(journal_reader_next(&reader, &event));
    REQUIRE(event.record_len > 0u);
    REQUIRE(journal_event_get_string(&event, "event_type", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "SecondEvent");
    REQUIRE(journal_event_get_string(&event, "source_file", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "/inbound/drop-001.edi");
    REQUIRE(journal_event_get_string(&event, "source_transaction", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "837");
    REQUIRE(journal_event_get_string(&event, "source_drop_id", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "837:000000001:1:0001");
    REQUIRE(journal_event_get_string(&event, "run_id", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "ingest-run");
    REQUIRE(journal_event_get_string(&event, "isa13", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "000000001");
    REQUIRE(journal_event_get_string(&event, "gs06", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "1");
    REQUIRE(journal_event_get_string(&event, "st02", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "0001");
    REQUIRE(journal_event_get_number_text(&event, "source_segment_index", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "9");

    REQUIRE_OK(journal_reader_next(&reader, &event));
    REQUIRE(event.record_len == 0u);
    REQUIRE_OK(journal_reader_close(&reader));

    (void)remove(journal_path);
    return 0;
}

static int test_journal_reader_reads_legacy_magic(void)
{
    char journal_path[512];
    char value[64];
    FILE *fp;
    journal_record_builder_t builder;
    journal_reader_t reader;
    journal_event_t event;

    REQUIRE(make_path(journal_path, sizeof(journal_path), TEST_OUTPUT_DIR, "legacy_magic.journal") == 0);
    (void)remove(journal_path);

    fp = fopen(journal_path, "wb");
    REQUIRE(fp != NULL);
    REQUIRE(fwrite("SCRIBEJ3", 1u, 8u, fp) == 8u);
    journal_record_builder_init(&builder);
    REQUIRE_OK(journal_record_builder_reset(&builder));
    REQUIRE_OK(journal_record_add_cstring(&builder, "event_type", "LegacyEvent"));
    REQUIRE_OK(journal_write_record(fp, &builder, NULL, NULL));
    journal_record_builder_free(&builder);
    REQUIRE(fclose(fp) == 0);

    journal_reader_init(&reader);
    REQUIRE_OK(journal_reader_open(&reader, journal_path));
    REQUIRE_OK(journal_reader_next(&reader, &event));
    REQUIRE(event.record_len > 0u);
    REQUIRE(journal_event_get_string(&event, "event_type", value, sizeof(value)) == 1);
    REQUIRE_STR(value, "LegacyEvent");
    REQUIRE_OK(journal_reader_close(&reader));

    (void)remove(journal_path);
    return 0;
}

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

static int test_journal_reader_reads_partition_directory(void)
{
    char root_path[512];
    char later_dir[512];
    char earlier_dir[512];
    char later_path[640];
    char earlier_path[640];
    char event_type[64];
    journal_reader_t reader;
    journal_event_t event;

    REQUIRE(make_path(root_path, sizeof(root_path), TEST_OUTPUT_DIR, "journal_parts") == 0);
    REQUIRE(snprintf(later_dir, sizeof(later_dir), "%s/20260602", root_path) > 0);
    REQUIRE(snprintf(earlier_dir, sizeof(earlier_dir), "%s/20260601", root_path) > 0);
    REQUIRE(snprintf(later_path, sizeof(later_path), "%s/drop-b.journal", later_dir) > 0);
    REQUIRE(snprintf(earlier_path, sizeof(earlier_path), "%s/drop-a.journal", earlier_dir) > 0);

    REQUIRE(make_dir(root_path) == 0);
    REQUIRE(make_dir(later_dir) == 0);
    REQUIRE(make_dir(earlier_dir) == 0);
    (void)remove(later_path);
    (void)remove(earlier_path);

    REQUIRE(write_test_journal_segment(later_path, "LaterDrop") == 0);
    REQUIRE(write_test_journal_segment(earlier_path, "EarlierDrop") == 0);

    journal_reader_init(&reader);
    REQUIRE_OK(journal_reader_open(&reader, root_path));

    REQUIRE_OK(journal_reader_next(&reader, &event));
    REQUIRE(event.record_len > 0u);
    REQUIRE(event.segment_path != NULL);
    REQUIRE(strstr(event.segment_path, "20260601") != NULL);
    REQUIRE(journal_event_get_string(&event, "event_type", event_type, sizeof(event_type)) == 1);
    REQUIRE_STR(event_type, "EarlierDrop");

    REQUIRE_OK(journal_reader_next(&reader, &event));
    REQUIRE(event.record_len > 0u);
    REQUIRE(event.segment_path != NULL);
    REQUIRE(strstr(event.segment_path, "20260602") != NULL);
    REQUIRE(journal_event_get_string(&event, "event_type", event_type, sizeof(event_type)) == 1);
    REQUIRE_STR(event_type, "LaterDrop");

    REQUIRE_OK(journal_reader_next(&reader, &event));
    REQUIRE(event.record_len == 0u);
    REQUIRE_OK(journal_reader_close(&reader));

    return 0;
}

int main(void)
{
    REQUIRE(test_json_escaping() == 0);
    REQUIRE(test_json_scan_unescapes_strings() == 0);
    REQUIRE(test_tokenise_format() == 0);
    REQUIRE(test_journal_reader_reads_partition_directory() == 0);
    REQUIRE(test_journal_reader_carries_source_context() == 0);
    REQUIRE(test_journal_reader_reads_legacy_magic() == 0);
    return 0;
}
