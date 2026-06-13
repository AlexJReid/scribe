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
        REQUIRE_OK(journal_write_header(event_writer_underlying_file(&writer)));
        REQUIRE_OK(event_writer_set_mode(&writer, EVENT_WRITER_MODE_JOURNAL));
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
    REQUIRE(strstr(output, "\"diagnosis_pointers\":[\"1\"]") != NULL);
    REQUIRE(strstr(output, "\"diagnosis_pointers\":[\"2\"]") != NULL);
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
    REQUIRE(strstr(output, "\"diagnosis_pointers\":[]") != NULL);
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
    REQUIRE(strstr(output, "\"event_type\":\"ClaimProviderTaxonomyRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"provider_context\":\"referring_provider\"") != NULL);
    REQUIRE(strstr(output, "\"provider_role_code\":\"RF\"") != NULL);
    REQUIRE(strstr(output, "\"provider_taxonomy_code\":\"207Q00000X\"") != NULL);
    REQUIRE(strstr(output, "\"provider_context\":\"rendering_provider\"") != NULL);
    REQUIRE(strstr(output, "\"reference_scope\":\"service_line\"") != NULL);
    REQUIRE(strstr(output, "\"provider_taxonomy_code\":\"207R00000X\"") != NULL);
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

static int test_claim_envelope_and_party_context_in_ndjson(void)
{
    char out_path[512];
    char output[50000];

    REQUIRE(map_fixture(
                "x12_005010x222_example_01_synthetic.edi",
                "test_837_mapper_claim_context.ndjson",
                0
            ) == 0);
    REQUIRE(make_path(
                out_path,
                sizeof(out_path),
                TEST_OUTPUT_DIR,
                "test_837_mapper_claim_context.ndjson"
            ) == 0);
    REQUIRE(read_file_text(out_path, output, sizeof(output)) == 0);

    REQUIRE(strstr(output, "\"event_type\":\"ClaimObserved\"") != NULL);
    REQUIRE(strstr(output, "\"facility_type_code\":\"11\"") != NULL);
    REQUIRE(strstr(output, "\"facility_code_qualifier\":\"B\"") != NULL);
    REQUIRE(strstr(output, "\"claim_frequency_type_code\":\"1\"") != NULL);
    REQUIRE(strstr(output, "\"provider_signature_indicator\":\"Y\"") != NULL);
    REQUIRE(strstr(output, "\"assignment_or_plan_participation_code\":\"A\"") != NULL);
    REQUIRE(strstr(output, "\"benefits_assignment_certification_indicator\":\"Y\"") != NULL);
    REQUIRE(strstr(output, "\"release_of_information_code\":\"I\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimProviderTaxonomyRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"provider_context\":\"billing_provider\"") != NULL);
    REQUIRE(strstr(output, "\"provider_role_code\":\"BI\"") != NULL);
    REQUIRE(strstr(output, "\"reference_identification_qualifier\":\"PXC\"") != NULL);
    REQUIRE(strstr(output, "\"provider_taxonomy_code\":\"203BF0100Y\"") != NULL);

    REQUIRE(strstr(output, "\"event_type\":\"ClaimSubscriberInformationRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"party_scope\":\"subscriber\"") != NULL);
    REQUIRE(strstr(output, "\"payer_responsibility_sequence_number_code\":\"P\"") != NULL);
    REQUIRE(strstr(output, "\"claim_filing_indicator_code\":\"CI\"") != NULL);
    REQUIRE(strstr(output, "\"insured_group_or_policy_number\":\"GROUP-837P\"") == NULL);

    REQUIRE(strstr(output, "\"event_type\":\"ClaimPatientInformationRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"party_scope\":\"patient\"") != NULL);
    REQUIRE(strstr(output, "\"individual_relationship_code\":\"19\"") != NULL);

    REQUIRE(count_substring(output, "\"event_type\":\"ClaimDemographicsRecorded\"") == 2u);
    REQUIRE(strstr(output, "\"date_of_birth\":\"19430501\"") == NULL);
    REQUIRE(strstr(output, "\"date_of_birth\":\"19730501\"") == NULL);
    REQUIRE(strstr(output, "\"gender_code\":\"F\"") != NULL);
    REQUIRE(strstr(output, "\"gender_code\":\"M\"") != NULL);

    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferenceRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"reference_scope\":\"claim\"") != NULL);
    REQUIRE(strstr(output, "\"reference_qualifier\":\"D9\"") != NULL);
    REQUIRE(strstr(output, "\"reference_identification\":\"SYNTHCLEARING001\"") == NULL);
    REQUIRE(strstr(output, "\"reference_scope\":\"service_line\"") != NULL);
    REQUIRE(strstr(output, "\"reference_qualifier\":\"6R\"") != NULL);
    REQUIRE(strstr(output, "\"service_line_number\":\"1\"") != NULL);
    REQUIRE(strstr(output, "\"reference_identification\":\"LINECTRL1\"") == NULL);
    return 0;
}

static int test_billing_provider_taxonomy_does_not_carry_to_next_provider(void)
{
    char out_path[512];
    char output[30000];

    REQUIRE(map_fixture(
                "sample_837_billing_provider_switch.edi",
                "test_837_mapper_billing_provider_switch.ndjson",
                0
            ) == 0);
    REQUIRE(make_path(
                out_path,
                sizeof(out_path),
                TEST_OUTPUT_DIR,
                "test_837_mapper_billing_provider_switch.ndjson"
            ) == 0);
    REQUIRE(read_file_text(out_path, output, sizeof(output)) == 0);

    REQUIRE(count_substring(output, "\"event_type\":\"ClaimObserved\"") == 2u);
    REQUIRE(count_substring(output, "\"event_type\":\"ClaimProviderTaxonomyRecorded\"") == 1u);
    REQUIRE(count_substring(output, "\"provider_taxonomy_code\":\"TAXONOMY1\"") == 1u);
    REQUIRE(strstr(output, "\"total_charge_amount\":\"225.50\"") != NULL);
    return 0;
}

static int test_institutional_hi_components_in_ndjson(void)
{
    char out_path[512];
    char output[20000];

    REQUIRE(map_fixture(
                "sample_837i_hi_components.edi",
                "test_837_mapper_hi_components.ndjson",
                0
            ) == 0);
    REQUIRE(make_path(
                out_path,
                sizeof(out_path),
                TEST_OUTPUT_DIR,
                "test_837_mapper_hi_components.ndjson"
            ) == 0);
    REQUIRE(read_file_text(out_path, output, sizeof(output)) == 0);

    REQUIRE(strstr(output, "\"event_type\":\"ClaimDiagnosesRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"principal_diagnosis_code\":\"R07.9\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimInstitutionalInformationRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"admission_type_code\":\"1\"") != NULL);
    REQUIRE(strstr(output, "\"admission_source_code\":\"7\"") != NULL);
    REQUIRE(strstr(output, "\"patient_status_code\":\"30\"") != NULL);
    REQUIRE(strstr(output, "\"date_qualifier\":\"435\"") != NULL);
    REQUIRE(strstr(output, "\"date_value\":\"20260601\"") != NULL);
    REQUIRE(strstr(output, "\"date_qualifier\":\"096\"") != NULL);
    REQUIRE(strstr(output, "\"date_value\":\"20260604\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimHealthcareCodeRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_kind\":\"condition_code\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_qualifier\":\"BG\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code\":\"01\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_kind\":\"occurrence_code\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_qualifier\":\"BH\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_date_format\":\"D8\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_date_value\":\"20260601\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_kind\":\"value_code\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_qualifier\":\"BE\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_amount\":\"2500.00\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_components\":[\"BE\",\"A1\",\"\",\"\",\"2500.00\"]") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_kind\":\"institutional_procedure\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_qualifier\":\"BBR\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code\":\"0WQF0ZZ\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_date_value\":\"20260602\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_kind\":\"diagnosis_related_group\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code_qualifier\":\"DR\"") != NULL);
    REQUIRE(strstr(output, "\"healthcare_code\":\"470\"") != NULL);
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
    char diagnosis_pointer[16];
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
        REQUIRE(!journal_event_get_array_string_at(
                    &event,
                    "diagnosis_pointers",
                    0u,
                    diagnosis_pointer,
                    sizeof(diagnosis_pointer)
                ));
        REQUIRE_STR(revenue_code, "0450");
        REQUIRE_STR(procedure_code, "99284");
        REQUIRE_STR(modifier, "25");
        REQUIRE_STR(charge_amount, "950.00");
        REQUIRE_STR(unit_measure_code, "UN");
        REQUIRE_STR(unit_count, "1");
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
    REQUIRE(test_claim_envelope_and_party_context_in_ndjson() == 0);
    REQUIRE(test_billing_provider_taxonomy_does_not_carry_to_next_provider() == 0);
    REQUIRE(test_institutional_hi_components_in_ndjson() == 0);
    REQUIRE(test_service_line_fields_in_binary_journal() == 0);
    return 0;
}
