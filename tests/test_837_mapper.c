#include "event_writer.h"
#include "journal.h"
#include "test_support.h"
#include "x12_mapper_837.h"
#include "x12_reader.h"

#include <string.h>

static int map_fixture(
    const char *fixture_name,
    const char *out_name,
    int binary_journal
)
{
    char input_path[512];
    char out_path[512];
    x12_document_t doc;
    event_writer_t writer;
    int rc;

    REQUIRE(make_path(input_path, sizeof(input_path), TEST_FIXTURE_DIR, fixture_name) == 0);
    REQUIRE(make_path(out_path, sizeof(out_path), TEST_OUTPUT_DIR, out_name) == 0);

    REQUIRE_OK(x12_document_load(input_path, &doc));
    REQUIRE_OK(event_writer_open(&writer, out_path, input_path, "837"));
    if (binary_journal) {
        REQUIRE_OK(journal_write_header(event_writer_stream(&writer)));
        REQUIRE_OK(event_writer_set_binary_journal(&writer, 1));
    }
    rc = x12_map_837_document(&doc, &writer);
    REQUIRE_OK(rc);
    REQUIRE_OK(event_writer_close(&writer));
    x12_document_free(&doc);
    return 0;
}

static int test_service_line_fields_in_ndjson(void)
{
    char out_path[512];
    char output[24000];

    REQUIRE(map_fixture(
                "x12_005010x222_example_01_synthetic.edi",
                "test_837_mapper_service_lines.ndjson",
                0
            ) == 0);
    REQUIRE(make_path(
                out_path,
                sizeof(out_path),
                TEST_OUTPUT_DIR,
                "test_837_mapper_service_lines.ndjson"
            ) == 0);
    REQUIRE(read_file_text(out_path, output, sizeof(output)) == 0);

    REQUIRE(strstr(output, "\"event_type\":\"ClaimServiceLineRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code\":\"99213\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_modifiers\":[]") != NULL);
    REQUIRE(strstr(output, "\"charge_amount\":\"40\"") != NULL);
    REQUIRE(strstr(output, "\"unit_measure_code\":\"UN\"") != NULL);
    REQUIRE(strstr(output, "\"unit_count\":\"1\"") != NULL);
    REQUIRE(strstr(output, "\"diagnosis_pointers\":\"1\"") != NULL);
    REQUIRE(strstr(output, "\"diagnosis_pointers\":\"2\"") != NULL);
    REQUIRE(strstr(output, "\"raw_elements\":[\"HC:99213\",\"40\",\"UN\",\"1\",\"\",\"\",\"1\"]") != NULL);
    return 0;
}

static int test_revenue_line_fields_in_ndjson(void)
{
    char out_path[512];
    char output[16000];

    REQUIRE(map_fixture(
                "sample_837i_revenue_line.edi",
                "test_837_mapper_revenue_lines.ndjson",
                0
            ) == 0);
    REQUIRE(make_path(
                out_path,
                sizeof(out_path),
                TEST_OUTPUT_DIR,
                "test_837_mapper_revenue_lines.ndjson"
            ) == 0);
    REQUIRE(read_file_text(out_path, output, sizeof(output)) == 0);

    REQUIRE(strstr(output, "\"event_type\":\"ClaimServiceLineRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"line_type\":\"SV2\"") != NULL);
    REQUIRE(strstr(output, "\"revenue_code\":\"0450\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code_qualifier\":\"HC\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code\":\"99284\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_modifiers\":[\"25\"]") != NULL);
    REQUIRE(strstr(output, "\"charge_amount\":\"950.00\"") != NULL);
    REQUIRE(strstr(output, "\"unit_measure_code\":\"UN\"") != NULL);
    REQUIRE(strstr(output, "\"unit_count\":\"1\"") != NULL);
    REQUIRE(strstr(output, "\"diagnosis_pointers\":\"\"") != NULL);
    return 0;
}

static int test_modifiers_and_provider_roles_in_ndjson(void)
{
    char out_path[512];
    char output[20000];

    REQUIRE(map_fixture(
                "sample_837_provider_roles.edi",
                "test_837_mapper_provider_roles.ndjson",
                0
            ) == 0);
    REQUIRE(make_path(
                out_path,
                sizeof(out_path),
                TEST_OUTPUT_DIR,
                "test_837_mapper_provider_roles.ndjson"
            ) == 0);
    REQUIRE(read_file_text(out_path, output, sizeof(output)) == 0);

    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedReferringProvider\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedSupervisingProvider\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedFacility\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedAttendingProvider\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedOperatingProvider\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedOtherProvider\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedRenderingProvider\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code\":\"99213\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_modifiers\":[\"25\",\"59\"]") != NULL);
    REQUIRE(count_substring(output, "\"reference_scope\":\"claim\"") >= 6u);
    REQUIRE(strstr(output, "\"reference_scope\":\"service_line\"") != NULL);
    REQUIRE(strstr(output, "\"id_value\":\"1111111111\"") == NULL);
    REQUIRE(strstr(output, "\"id_value\":\"2222222222\"") == NULL);
    REQUIRE(strstr(output, "\"id_value\":\"3333333333\"") == NULL);
    REQUIRE(strstr(output, "\"id_value\":\"4444444444\"") == NULL);
    REQUIRE(strstr(output, "\"id_value\":\"5555555555\"") == NULL);
    REQUIRE(strstr(output, "\"id_value\":\"6666666666\"") == NULL);
    REQUIRE(strstr(output, "\"id_value\":\"7777777777\"") == NULL);
    return 0;
}

static int test_service_line_fields_in_binary_journal(void)
{
    char journal_path[512];
    journal_reader_t reader;
    journal_event_t event;
    char event_type[128];
    char revenue_code[16];
    char procedure_code[32];
    char modifier[16];
    char charge_amount[32];
    char unit_measure_code[16];
    char unit_count[16];
    char diagnosis_pointers[16];
    int saw_service_line = 0;
    int rc;

    REQUIRE(map_fixture("sample_837i_revenue_line.edi", "test_837_mapper_service_lines.journal", 1) == 0);
    REQUIRE(make_path(
                journal_path,
                sizeof(journal_path),
                TEST_OUTPUT_DIR,
                "test_837_mapper_service_lines.journal"
            ) == 0);

    journal_reader_init(&reader);
    REQUIRE_OK(journal_reader_open(&reader, journal_path));
    for (;;) {
        rc = journal_reader_next(&reader, &event);
        REQUIRE_OK(rc);
        if (event.field_count == 0u) {
            break;
        }

        if (!journal_event_get_string(&event, "event_type", event_type, sizeof(event_type)) ||
            strcmp(event_type, "ClaimServiceLineRecorded") != 0) {
            continue;
        }

        REQUIRE(journal_event_get_string(&event, "revenue_code", revenue_code, sizeof(revenue_code)));
        REQUIRE(journal_event_get_string(&event, "procedure_code", procedure_code, sizeof(procedure_code)));
        REQUIRE(journal_event_get_array_string_at(&event, "procedure_modifiers", 0u, modifier, sizeof(modifier)));
        REQUIRE(journal_event_get_string(&event, "charge_amount", charge_amount, sizeof(charge_amount)));
        REQUIRE(journal_event_get_string(
                    &event,
                    "unit_measure_code",
                    unit_measure_code,
                    sizeof(unit_measure_code)
                ));
        REQUIRE(journal_event_get_string(&event, "unit_count", unit_count, sizeof(unit_count)));
        REQUIRE(journal_event_get_string(
                    &event,
                    "diagnosis_pointers",
                    diagnosis_pointers,
                    sizeof(diagnosis_pointers)
                ));
        REQUIRE_STR(revenue_code, "0450");
        REQUIRE_STR(procedure_code, "99284");
        REQUIRE_STR(modifier, "25");
        REQUIRE_STR(charge_amount, "950.00");
        REQUIRE_STR(unit_measure_code, "UN");
        REQUIRE_STR(unit_count, "1");
        REQUIRE_STR(diagnosis_pointers, "");
        saw_service_line = 1;
    }
    REQUIRE_OK(journal_reader_close(&reader));
    REQUIRE(saw_service_line);

    return 0;
}

int main(void)
{
    REQUIRE(test_service_line_fields_in_ndjson() == 0);
    REQUIRE(test_revenue_line_fields_in_ndjson() == 0);
    REQUIRE(test_modifiers_and_provider_roles_in_ndjson() == 0);
    REQUIRE(test_service_line_fields_in_binary_journal() == 0);
    return 0;
}
