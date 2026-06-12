#include "aggregate_stitcher.h"
#include "balance_projector.h"
#include "coverage_stitcher.h"
#include "event_writer.h"
#include "journal.h"
#include "journal_builder.h"
#include "json_scan.h"
#include "phi_vault.h"
#include "store.h"
#include "test_support.h"
#include "tokenise.h"
#include "x12_mapper_270_271.h"
#include "x12_mapper_834.h"
#include "x12_mapper_835.h"
#include "x12_mapper_837.h"
#include "x12_reader.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int copy_sqlite_column_text(sqlite3_stmt *stmt, int column, char *out, size_t out_len)
{
    const unsigned char *value;
    int len;

    if (stmt == NULL || out == NULL || out_len == 0u) {
        return 1;
    }

    value = sqlite3_column_text(stmt, column);
    if (value == NULL) {
        value = (const unsigned char *)"";
    }
    len = sqlite3_column_bytes(stmt, column);
    if (len < 0 || (size_t)len >= out_len) {
        return 1;
    }

    memcpy(out, value, (size_t)len);
    out[len] = '\0';
    return 0;
}

static int phi_mapping_source_drops(
    phi_vault_t *vault,
    const char *namespace_name,
    const char *token,
    char *first_source_drop_id,
    size_t first_len,
    char *last_source_drop_id,
    size_t last_len
)
{
    sqlite3 *db;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int step_rc;
    int out_rc = 1;

    if (vault == NULL || namespace_name == NULL || token == NULL ||
        first_source_drop_id == NULL || last_source_drop_id == NULL) {
        return 1;
    }

    db = (sqlite3 *)vault->db;
    if (db == NULL) {
        return 1;
    }

    rc = sqlite3_prepare_v2(
        db,
        "SELECT first_source_drop_id, last_source_drop_id "
        "FROM phi_mappings WHERE namespace = ? AND token = ?;",
        -1,
        &stmt,
        NULL
    );
    if (rc != SQLITE_OK) {
        return 1;
    }
    rc = sqlite3_bind_text(stmt, 1, namespace_name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(stmt, 2, token, -1, SQLITE_TRANSIENT);
    }
    step_rc = rc == SQLITE_OK ? sqlite3_step(stmt) : SQLITE_DONE;
    if (step_rc == SQLITE_ROW &&
        copy_sqlite_column_text(stmt, 0, first_source_drop_id, first_len) == 0 &&
        copy_sqlite_column_text(stmt, 1, last_source_drop_id, last_len) == 0) {
        out_rc = 0;
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK) {
        return 1;
    }

    return out_rc;
}

static int parse_fixture_to_output(
    const char *fixture_name,
    const char *transaction_type,
    const char *out_name,
    char *output,
    size_t output_len
)
{
    char input_path[512];
    char out_path[512];
    x12_document_t doc;
    event_writer_t writer;
    int rc;

    REQUIRE(make_path(input_path, sizeof(input_path), TEST_FIXTURE_DIR, fixture_name) == 0);
    REQUIRE(make_path(out_path, sizeof(out_path), TEST_OUTPUT_DIR, out_name) == 0);

    rc = x12_document_load(input_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, out_path, input_path, transaction_type);
    REQUIRE_OK(rc);

    if (strcmp(transaction_type, "270") == 0) {
        rc = x12_map_270_document(&doc, &writer);
    } else if (strcmp(transaction_type, "271") == 0) {
        rc = x12_map_271_document(&doc, &writer);
    } else if (strcmp(transaction_type, "834") == 0) {
        rc = x12_map_834_document(&doc, &writer);
    } else if (strcmp(transaction_type, "837") == 0) {
        rc = x12_map_837_document(&doc, &writer);
    } else if (strcmp(transaction_type, "835") == 0) {
        rc = x12_map_835_document(&doc, &writer);
    } else {
        rc = X12_ERR_INVALID_ARGUMENT;
    }

    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(out_path, output, output_len) == 0);
    return 0;
}

static int test_837_claim_event(void)
{
    char input_path[512];
    char out_path[512];
    char phi_out_path[512];
    char output[16000];
    char phi_output[16000];
    x12_document_t doc;
    event_writer_t writer;
    int rc;

    REQUIRE(make_path(input_path, sizeof(input_path), TEST_FIXTURE_DIR, "sample_837.edi") == 0);
    REQUIRE(make_path(out_path, sizeof(out_path), TEST_OUTPUT_DIR, "sample_837.ndjson") == 0);
    REQUIRE(make_path(phi_out_path, sizeof(phi_out_path), TEST_OUTPUT_DIR, "sample_837_phi.ndjson") == 0);

    rc = x12_document_load(input_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, out_path, input_path, "837");
    REQUIRE_OK(rc);
    rc = x12_map_837_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(out_path, output, sizeof(output)) == 0);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimObserved\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedBillingProvider\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedSubscriber\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedPatient\"") != NULL);
    REQUIRE(strstr(output, "\"claim_id\":\"40d5e288b97e8a83d8f2fa18541f14f4\"") != NULL);
    REQUIRE(strstr(output, "\"payload\":{\"claim_id\":\"40d5e288b97e8a83d8f2fa18541f14f4\",\"reference_scope\":\"claim\",\"service_line_number\":\"\",\"entity_type\":\"2\",\"id_qualifier\":\"XX\",\"id_value\":\"5da034fe41b57b72eb4e35d75de77366\"") != NULL);
    REQUIRE(strstr(output, "\"payload\":{\"claim_id\":\"40d5e288b97e8a83d8f2fa18541f14f4\",\"reference_scope\":\"claim\",\"service_line_number\":\"\",\"entity_type\":\"1\",\"id_qualifier\":\"MI\",\"id_value\":\"6ead5d9ab487004819b146b59f6b36f8\"") != NULL);
    REQUIRE(strstr(output, "\"payload\":{\"claim_id\":\"40d5e288b97e8a83d8f2fa18541f14f4\",\"reference_scope\":\"claim\",\"service_line_number\":\"\",\"entity_type\":\"1\",\"id_qualifier\":\"MI\",\"id_value\":\"e5867d5b51d5031be0e06d660c1fd50a\"") != NULL);
    REQUIRE(strstr(output, "\"claim_id\":\"CLM123\"") == NULL);
    REQUIRE(strstr(output, "\"claim_id_token\"") == NULL);
    REQUIRE(strstr(output, "\"last_name_or_org\"") == NULL);
    REQUIRE(strstr(output, "\"first_name\"") == NULL);
    REQUIRE(strstr(output, "\"id_value\":\"SUB12345\"") == NULL);
    REQUIRE(strstr(output, "\"id_value_token\"") == NULL);
    REQUIRE(strstr(output, "\"total_charge_amount\":\"125.50\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimDateRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"date_scope\":\"claim\"") != NULL);
    REQUIRE(strstr(output, "\"date_qualifier\":\"434\"") != NULL);
    REQUIRE(strstr(output, "\"date_format\":\"RD8\"") != NULL);
    REQUIRE(strstr(output, "\"date_value\":\"20260601-20260602\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimDiagnosesRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"principal_diagnosis_code\":\"K21.9\"") != NULL);
    REQUIRE(strstr(output, "\"other_diagnosis_codes\":[\"R51\"]") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimServiceLineRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"line_type\":\"SV1\"") != NULL);
    REQUIRE(strstr(output, "\"service_line_number\":\"1\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code_qualifier\":\"HC\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code_set\":\"CPT/HCPCS\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code\":\"99213\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimLineDateRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"date_scope\":\"service_line\"") != NULL);
    REQUIRE(strstr(output, "\"date_qualifier\":\"472\"") != NULL);
    REQUIRE(strstr(output, "\"date_value\":\"20260601\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"BillingProviderReferenced\"") == NULL);
    REQUIRE(strstr(output, "\"event_type\":\"SubscriberReferenced\"") == NULL);
    REQUIRE(strstr(output, "\"event_type\":\"PatientReferenced\"") == NULL);
    REQUIRE(strstr(output, "\"event_type\":\"DateObserved\"") == NULL);
    REQUIRE(strstr(output, "\"event_type\":\"DiagnosisObserved\"") == NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ServiceLineObserved\"") == NULL);

    rc = x12_document_load(input_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, phi_out_path, input_path, "837");
    REQUIRE_OK(rc);
    event_writer_set_include_phi(&writer, 1);
    rc = x12_map_837_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(phi_out_path, phi_output, sizeof(phi_output)) == 0);
    REQUIRE(strstr(phi_output, "\"claim_id\":\"CLM123\"") != NULL);
    REQUIRE(strstr(phi_output, "\"last_name_or_org\":\"DOE\"") != NULL);
    REQUIRE(strstr(phi_output, "\"first_name\":\"JANE\"") != NULL);
    REQUIRE(strstr(phi_output, "\"id_value\":\"SUB12345\"") != NULL);
    REQUIRE(strstr(phi_output, "\"claim_id_token\":\"40d5e288b97e8a83d8f2fa18541f14f4\"") != NULL);
    REQUIRE(strstr(phi_output, "\"id_value_token\":\"6ead5d9ab487004819b146b59f6b36f8\"") != NULL);
    return 0;
}

static int test_x12_005010x222_example_01_shape(void)
{
    char output[50000];

    REQUIRE(parse_fixture_to_output(
                "x12_005010x222_example_01_synthetic.edi",
                "837",
                "x12_005010x222_example_01.ndjson",
                output,
                sizeof(output)
            ) == 0);
    REQUIRE(strstr(output, "\"source_drop_id\":\"837:000000201:201:0021\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimObserved\"") != NULL);
    REQUIRE(strstr(output, "\"total_charge_amount\":\"100\"") != NULL);
    REQUIRE(strstr(output, "\"claim_id\":\"X12EXAMPLE01\"") == NULL);
    REQUIRE(strstr(output, "\"last_name_or_org\":\"NOVA\"") == NULL);
    REQUIRE(strstr(output, "\"first_name\":\"JAMIE\"") == NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedBillingProvider\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedSubscriber\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimReferencedPatient\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"ClaimDiagnosesRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"principal_diagnosis_code\":\"J02.9\"") != NULL);
    REQUIRE(strstr(output, "\"other_diagnosis_codes\":[\"R51.9\"]") != NULL);
    REQUIRE(strstr(output, "\"raw_diagnosis_elements\":[\"BK:J029\",\"BF:R519\"]") != NULL);
    REQUIRE(count_substring(output, "\"event_type\":\"ClaimServiceLineRecorded\"") == 4u);
    REQUIRE(count_substring(output, "\"event_type\":\"ClaimLineDateRecorded\"") == 4u);
    REQUIRE(strstr(output, "\"procedure_code\":\"99213\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code\":\"87070\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code\":\"99214\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code\":\"86663\"") != NULL);
    REQUIRE(strstr(output, "\"date_value\":\"20260603\"") != NULL);
    REQUIRE(strstr(output, "\"date_value\":\"20260610\"") != NULL);

    return 0;
}

static int test_journal_builder_file_list(void)
{
    char fixture_path[512];
    char list_path[512];
    char journal_path[512];
    FILE *list_fp;
    journal_builder_input_t journal_input;
    journal_reader_t reader;
    journal_event_t event;
    char event_type[128];
    char source_file[512];
    char source_transaction[32];
    size_t claim_observed_count = 0u;
    int rc;

    REQUIRE(make_path(fixture_path, sizeof(fixture_path), TEST_FIXTURE_DIR, "sample_837.edi") == 0);
    REQUIRE(make_path(list_path, sizeof(list_path), TEST_OUTPUT_DIR, "sample_837_list.txt") == 0);
    REQUIRE(make_path(journal_path, sizeof(journal_path), TEST_OUTPUT_DIR, "sample_837_list.journal") == 0);

    list_fp = fopen(list_path, "wb");
    REQUIRE(list_fp != NULL);
    REQUIRE(fprintf(list_fp, "%s\n%s\n\n", fixture_path, fixture_path) > 0);
    REQUIRE(fclose(list_fp) == 0);

    (void)remove(journal_path);
    journal_builder_input_init(&journal_input);
    journal_input.run_id = "file-list-test";
    journal_input.source_root = TEST_FIXTURE_DIR;
    journal_input.x837_list_path = list_path;
    REQUIRE_OK(journal_builder_build(&journal_input, journal_path));

    journal_reader_init(&reader);
    REQUIRE_OK(journal_reader_open(&reader, journal_path));
    while (1) {
        rc = journal_reader_next(&reader, &event);
        REQUIRE_OK(rc);
        if (event.record_len == 0u) {
            break;
        }
        event_type[0] = '\0';
        source_file[0] = '\0';
        source_transaction[0] = '\0';
        (void)journal_event_get_string(&event, "event_type", event_type, sizeof(event_type));
        (void)journal_event_get_string(&event, "source_file", source_file, sizeof(source_file));
        (void)journal_event_get_string(
            &event,
            "source_transaction",
            source_transaction,
            sizeof(source_transaction)
        );
        if (strcmp(source_transaction, "837") == 0 &&
            strcmp(event_type, "ClaimObserved") == 0) {
            REQUIRE_STR(source_file, "sample_837.edi");
            claim_observed_count++;
        }
    }
    REQUIRE_OK(journal_reader_close(&reader));
    REQUIRE(claim_observed_count == 2u);

    (void)remove(list_path);
    (void)remove(journal_path);
    return 0;
}

static int test_journal_builder_compressed_zstd(void)
{
    char fixture_path[512];
    char journal_path[512];
    char raw_temp_path[640];
    journal_builder_input_t journal_input;
    journal_reader_t reader;
    journal_event_t event;
    char event_type[128];
    char source_file[512];
    size_t claim_observed_count = 0u;
    int rc;

    REQUIRE(make_path(fixture_path, sizeof(fixture_path), TEST_FIXTURE_DIR, "sample_837.edi") == 0);
    REQUIRE(make_path(journal_path, sizeof(journal_path), TEST_OUTPUT_DIR, "sample_837_zstd.journal.zst") == 0);
    REQUIRE(snprintf(raw_temp_path, sizeof(raw_temp_path), "%s.raw", journal_path) > 0);
    (void)remove(journal_path);
    (void)remove(raw_temp_path);

    journal_builder_input_init(&journal_input);
    journal_input.run_id = "zstd-test";
    journal_input.source_root = TEST_FIXTURE_DIR;
    journal_input.compress_zstd = 1;
    REQUIRE_OK(journal_builder_input_add_837(&journal_input, fixture_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_path));

    journal_reader_init(&reader);
    REQUIRE_OK(journal_reader_open(&reader, journal_path));
    while (1) {
        rc = journal_reader_next(&reader, &event);
        REQUIRE_OK(rc);
        if (event.record_len == 0u) {
            break;
        }
        event_type[0] = '\0';
        source_file[0] = '\0';
        (void)journal_event_get_string(&event, "event_type", event_type, sizeof(event_type));
        (void)journal_event_get_string(&event, "source_file", source_file, sizeof(source_file));
        if (strcmp(event_type, "ClaimObserved") == 0) {
            REQUIRE_STR(source_file, "sample_837.edi");
            claim_observed_count++;
        }
    }
    REQUIRE_OK(journal_reader_close(&reader));
    REQUIRE(claim_observed_count == 1u);

    (void)remove(journal_path);
    (void)remove(raw_temp_path);
    return 0;
}

static int test_834_ins_event(void)
{
    char input_path[512];
    char out_path[512];
    char phi_out_path[512];
    char output[8192];
    char phi_output[8192];
    x12_document_t doc;
    event_writer_t writer;
    int rc;

    REQUIRE(make_path(input_path, sizeof(input_path), TEST_FIXTURE_DIR, "sample_834.edi") == 0);
    REQUIRE(make_path(out_path, sizeof(out_path), TEST_OUTPUT_DIR, "sample_834.ndjson") == 0);
    REQUIRE(make_path(phi_out_path, sizeof(phi_out_path), TEST_OUTPUT_DIR, "sample_834_phi.ndjson") == 0);

    rc = x12_document_load(input_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, out_path, input_path, "834");
    REQUIRE_OK(rc);
    rc = x12_map_834_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(out_path, output, sizeof(output)) == 0);
    REQUIRE(strstr(output, "\"event_type\":\"MemberEnrollmentChanged\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"MemberReferenced\"") != NULL);
    REQUIRE(strstr(output, "\"id_value\":\"1a1dbacf37d1e1998645cf82e8fccc15\"") != NULL);
    REQUIRE(strstr(output, "\"id_value\":\"MEM12345\"") == NULL);
    REQUIRE(strstr(output, "\"id_value_token\"") == NULL);
    REQUIRE(strstr(output, "\"last_name_or_org\"") == NULL);
    REQUIRE(strstr(output, "\"first_name\"") == NULL);
    REQUIRE(strstr(output, "\"relationship_code\":\"18\"") != NULL);
    REQUIRE(strstr(output, "\"maintenance_type_code\":\"021\"") != NULL);
    REQUIRE(strstr(output, "\"benefit_status_code\":\"A\"") != NULL);

    rc = x12_document_load(input_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, phi_out_path, input_path, "834");
    REQUIRE_OK(rc);
    event_writer_set_include_phi(&writer, 1);
    rc = x12_map_834_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(phi_out_path, phi_output, sizeof(phi_output)) == 0);
    REQUIRE(strstr(phi_output, "\"last_name_or_org\":\"DOE\"") != NULL);
    REQUIRE(strstr(phi_output, "\"first_name\":\"JOHN\"") != NULL);
    REQUIRE(strstr(phi_output, "\"id_value\":\"MEM12345\"") != NULL);
    REQUIRE(strstr(phi_output, "\"id_value_token\":\"1a1dbacf37d1e1998645cf82e8fccc15\"") != NULL);
    return 0;
}

static int test_270_271_eligibility_events(void)
{
    char x834_path[512];
    char x270_path[512];
    char x271_path[512];
    char x270_out_path[512];
    char x271_out_path[512];
    char x270_phi_out_path[512];
    char x271_phi_out_path[512];
    char journal_path[512];
    char output_270[20000];
    char output_271[24000];
    char phi_output_270[20000];
    char phi_output_271[24000];
    char member_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_token[TOKENISE_MAX_TOKEN_LEN];
    char provider_token[TOKENISE_MAX_TOKEN_LEN];
    char dob_token[TOKENISE_MAX_TOKEN_LEN];
    char member_snippet[128];
    char payer_snippet[128];
    char provider_snippet[128];
    char dob_snippet[128];
    x12_str_t raw;
    x12_document_t doc;
    event_writer_t writer;
    journal_builder_input_t journal_input;
    journal_reader_t reader;
    journal_event_t event;
    char event_type[128];
    char source_transaction[32];
    char member_value[128];
    int saw_834 = 0;
    int saw_270 = 0;
    int saw_271 = 0;
    int rc;

    REQUIRE(make_path(x834_path, sizeof(x834_path), TEST_FIXTURE_DIR, "sample_834.edi") == 0);
    REQUIRE(make_path(x270_path, sizeof(x270_path), TEST_FIXTURE_DIR, "sample_270.edi") == 0);
    REQUIRE(make_path(x271_path, sizeof(x271_path), TEST_FIXTURE_DIR, "sample_271.edi") == 0);
    REQUIRE(make_path(x270_out_path, sizeof(x270_out_path), TEST_OUTPUT_DIR, "sample_270.ndjson") == 0);
    REQUIRE(make_path(x271_out_path, sizeof(x271_out_path), TEST_OUTPUT_DIR, "sample_271.ndjson") == 0);
    REQUIRE(make_path(x270_phi_out_path, sizeof(x270_phi_out_path), TEST_OUTPUT_DIR, "sample_270_phi.ndjson") == 0);
    REQUIRE(make_path(x271_phi_out_path, sizeof(x271_phi_out_path), TEST_OUTPUT_DIR, "sample_271_phi.ndjson") == 0);
    REQUIRE(make_path(journal_path, sizeof(journal_path), TEST_OUTPUT_DIR, "coverage_context.journal") == 0);

    raw.ptr = "SUB12345";
    raw.len = strlen(raw.ptr);
    REQUIRE_OK(tokenise_value(TOK_MEMBER_ID, raw, member_token, sizeof(member_token)));
    raw.ptr = "842610001";
    raw.len = strlen(raw.ptr);
    REQUIRE_OK(tokenise_value(TOK_PAYER_ID, raw, payer_token, sizeof(payer_token)));
    raw.ptr = "1234567893";
    raw.len = strlen(raw.ptr);
    REQUIRE_OK(tokenise_value(TOK_PROVIDER_ID, raw, provider_token, sizeof(provider_token)));
    raw.ptr = "19800101";
    raw.len = strlen(raw.ptr);
    REQUIRE_OK(tokenise_value(TOK_MEMBER_DOB, raw, dob_token, sizeof(dob_token)));
    REQUIRE(snprintf(member_snippet, sizeof(member_snippet), "\"member_id\":\"%s\"", member_token) > 0);
    REQUIRE(snprintf(payer_snippet, sizeof(payer_snippet), "\"payer_id\":\"%s\"", payer_token) > 0);
    REQUIRE(snprintf(provider_snippet, sizeof(provider_snippet), "\"provider_id\":\"%s\"", provider_token) > 0);
    REQUIRE(snprintf(dob_snippet, sizeof(dob_snippet), "\"date_of_birth\":\"%s\"", dob_token) > 0);

    rc = x12_document_load(x270_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, x270_out_path, x270_path, "270");
    REQUIRE_OK(rc);
    rc = x12_map_270_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(x270_out_path, output_270, sizeof(output_270)) == 0);
    REQUIRE(strstr(output_270, "\"source_transaction\":\"270\"") != NULL);
    REQUIRE(strstr(output_270, "\"event_type\":\"EligibilityInquiryObserved\"") != NULL);
    REQUIRE(strstr(output_270, "\"eligibility_id\":\"ELIG270001\"") != NULL);
    REQUIRE(strstr(output_270, "\"transaction_set_purpose_code\":\"13\"") != NULL);
    REQUIRE(strstr(output_270, "\"event_type\":\"EligibilityInquiryPartyReferenced\"") != NULL);
    REQUIRE(strstr(output_270, payer_snippet) != NULL);
    REQUIRE(strstr(output_270, provider_snippet) != NULL);
    REQUIRE(strstr(output_270, member_snippet) != NULL);
    REQUIRE(strstr(output_270, "\"event_type\":\"EligibilityInquiryTraceRecorded\"") != NULL);
    REQUIRE(strstr(output_270, "\"trace_number\":\"TRACE270001\"") != NULL);
    REQUIRE(strstr(output_270, "\"event_type\":\"EligibilityInquiryDemographicsObserved\"") != NULL);
    REQUIRE(strstr(output_270, dob_snippet) != NULL);
    REQUIRE(strstr(output_270, "\"event_type\":\"EligibilityInquiryServiceTypeRequested\"") != NULL);
    REQUIRE(strstr(output_270, "\"service_type_code\":\"30\"") != NULL);
    REQUIRE(strstr(output_270, "\"member_id\":\"SUB12345\"") == NULL);
    REQUIRE(strstr(output_270, "\"id_value\":\"SUB12345\"") == NULL);
    REQUIRE(strstr(output_270, "\"date_of_birth\":\"19800101\"") == NULL);
    REQUIRE(strstr(output_270, "\"last_name_or_org\"") == NULL);
    REQUIRE(strstr(output_270, "\"first_name\"") == NULL);
    REQUIRE(strstr(output_270, "\"gender_code\"") == NULL);

    rc = x12_document_load(x270_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, x270_phi_out_path, x270_path, "270");
    REQUIRE_OK(rc);
    event_writer_set_include_phi(&writer, 1);
    rc = x12_map_270_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(x270_phi_out_path, phi_output_270, sizeof(phi_output_270)) == 0);
    REQUIRE(strstr(phi_output_270, "\"last_name_or_org\":\"DOE\"") != NULL);
    REQUIRE(strstr(phi_output_270, "\"first_name\":\"JANE\"") != NULL);
    REQUIRE(strstr(phi_output_270, "\"member_id\":\"SUB12345\"") != NULL);
    REQUIRE(strstr(phi_output_270, "\"member_id_token\"") != NULL);
    REQUIRE(strstr(phi_output_270, member_token) != NULL);
    REQUIRE(strstr(phi_output_270, "\"date_of_birth\":\"19800101\"") != NULL);
    REQUIRE(strstr(phi_output_270, "\"date_of_birth_token\"") != NULL);
    REQUIRE(strstr(phi_output_270, dob_token) != NULL);
    REQUIRE(strstr(phi_output_270, "\"gender_code\":\"F\"") != NULL);

    rc = x12_document_load(x271_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, x271_out_path, x271_path, "271");
    REQUIRE_OK(rc);
    rc = x12_map_271_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(x271_out_path, output_271, sizeof(output_271)) == 0);
    REQUIRE(strstr(output_271, "\"source_transaction\":\"271\"") != NULL);
    REQUIRE(strstr(output_271, "\"event_type\":\"EligibilityResponseObserved\"") != NULL);
    REQUIRE(strstr(output_271, "\"eligibility_id\":\"ELIG271001\"") != NULL);
    REQUIRE(strstr(output_271, "\"transaction_set_purpose_code\":\"11\"") != NULL);
    REQUIRE(strstr(output_271, "\"event_type\":\"EligibilityResponsePartyReferenced\"") != NULL);
    REQUIRE(strstr(output_271, member_snippet) != NULL);
    REQUIRE(strstr(output_271, "\"event_type\":\"EligibilityBenefitObserved\"") != NULL);
    REQUIRE(strstr(output_271, "\"eligibility_or_benefit_information_code\":\"1\"") != NULL);
    REQUIRE(strstr(output_271, "\"service_type_code\":\"30\"") != NULL);
    REQUIRE(strstr(output_271, "\"monetary_amount\":\"20.00\"") != NULL);
    REQUIRE(strstr(output_271, "\"in_plan_network_indicator\":\"Y\"") != NULL);
    REQUIRE(strstr(output_271, "\"event_type\":\"EligibilityResponseDateRecorded\"") != NULL);
    REQUIRE(strstr(output_271, "\"date_scope\":\"benefit\"") != NULL);
    REQUIRE(strstr(output_271, "\"date_qualifier\":\"346\"") != NULL);
    REQUIRE(strstr(output_271, "\"date_qualifier\":\"347\"") != NULL);
    REQUIRE(strstr(output_271, "\"member_id\":\"SUB12345\"") == NULL);
    REQUIRE(strstr(output_271, "\"date_of_birth\":\"19800101\"") == NULL);
    REQUIRE(strstr(output_271, "\"last_name_or_org\"") == NULL);
    REQUIRE(strstr(output_271, "\"first_name\"") == NULL);

    rc = x12_document_load(x271_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, x271_phi_out_path, x271_path, "271");
    REQUIRE_OK(rc);
    event_writer_set_include_phi(&writer, 1);
    rc = x12_map_271_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(x271_phi_out_path, phi_output_271, sizeof(phi_output_271)) == 0);
    REQUIRE(strstr(phi_output_271, "\"last_name_or_org\":\"DOE\"") != NULL);
    REQUIRE(strstr(phi_output_271, "\"first_name\":\"JANE\"") != NULL);
    REQUIRE(strstr(phi_output_271, "\"member_id\":\"SUB12345\"") != NULL);
    REQUIRE(strstr(phi_output_271, "\"member_id_token\"") != NULL);
    REQUIRE(strstr(phi_output_271, member_token) != NULL);
    REQUIRE(strstr(phi_output_271, "\"date_of_birth\":\"19800101\"") != NULL);
    REQUIRE(strstr(phi_output_271, "\"date_of_birth_token\"") != NULL);
    REQUIRE(strstr(phi_output_271, dob_token) != NULL);

    (void)remove(journal_path);
    journal_builder_input_init(&journal_input);
    journal_input.run_id = "coverage-context-test";
    REQUIRE_OK(journal_builder_input_add_834(&journal_input, x834_path));
    REQUIRE_OK(journal_builder_input_add_270(&journal_input, x270_path));
    REQUIRE_OK(journal_builder_input_add_271(&journal_input, x271_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_path));

    journal_reader_init(&reader);
    REQUIRE_OK(journal_reader_open(&reader, journal_path));
    while (1) {
        rc = journal_reader_next(&reader, &event);
        REQUIRE_OK(rc);
        if (event.record_len == 0u) {
            break;
        }
        event_type[0] = '\0';
        source_transaction[0] = '\0';
        (void)journal_event_get_string(&event, "event_type", event_type, sizeof(event_type));
        (void)journal_event_get_string(
            &event,
            "source_transaction",
            source_transaction,
            sizeof(source_transaction)
        );
        if (strcmp(source_transaction, "834") == 0 &&
            strcmp(event_type, "MemberEnrollmentChanged") == 0) {
            saw_834 = 1;
        }
        if (strcmp(source_transaction, "270") == 0 &&
            strcmp(event_type, "EligibilityInquiryServiceTypeRequested") == 0) {
            saw_270 = 1;
        }
        if (strcmp(source_transaction, "271") == 0 &&
            strcmp(event_type, "EligibilityBenefitObserved") == 0) {
            REQUIRE(journal_event_get_string(&event, "member_id", member_value, sizeof(member_value)) == 1);
            REQUIRE_STR(member_value, member_token);
            saw_271 = 1;
        }
    }
    REQUIRE_OK(journal_reader_close(&reader));
    REQUIRE(saw_834);
    REQUIRE(saw_270);
    REQUIRE(saw_271);
    (void)remove(journal_path);

    return 0;
}

static int test_835_remittance_events(void)
{
    char input_path[512];
    char out_path[512];
    char phi_out_path[512];
    char output[12000];
    char phi_output[12000];
    char payer_claim_control_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_claim_control_snippet[128];
    x12_str_t payer_claim_control_raw;
    x12_document_t doc;
    event_writer_t writer;
    int rc;

    REQUIRE(make_path(input_path, sizeof(input_path), TEST_FIXTURE_DIR, "sample_835.edi") == 0);
    REQUIRE(make_path(out_path, sizeof(out_path), TEST_OUTPUT_DIR, "sample_835.ndjson") == 0);
    REQUIRE(make_path(phi_out_path, sizeof(phi_out_path), TEST_OUTPUT_DIR, "sample_835_phi.ndjson") == 0);
    payer_claim_control_raw.ptr = "PAYERCLM123";
    payer_claim_control_raw.len = strlen(payer_claim_control_raw.ptr);
    REQUIRE_OK(tokenise_value(
                TOK_PAYER_CLAIM_CONTROL_NUMBER,
                payer_claim_control_raw,
                payer_claim_control_token,
                sizeof(payer_claim_control_token)
            ));
    REQUIRE(snprintf(
                payer_claim_control_snippet,
                sizeof(payer_claim_control_snippet),
                "\"payer_claim_control_number\":\"%s\"",
                payer_claim_control_token
            ) > 0);

    rc = x12_document_load(input_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, out_path, input_path, "835");
    REQUIRE_OK(rc);
    rc = x12_map_835_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(out_path, output, sizeof(output)) == 0);
    REQUIRE(strstr(output, "\"source_transaction\":\"835\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"RemittanceAdviceObserved\"") != NULL);
    REQUIRE(strstr(output, "\"remittance_id\":\"EFT123456\"") != NULL);
    REQUIRE(strstr(output, "\"payment_amount\":\"92.14\"") != NULL);
    REQUIRE(strstr(output, "\"payment_method_code\":\"CHK\"") != NULL);
    REQUIRE(strstr(output, "\"payment_date\":\"20260602\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"RemittancePartyReferenced\"") != NULL);
    REQUIRE(strstr(output, "\"entity_identifier_code\":\"PR\"") != NULL);
    REQUIRE(strstr(output, "\"entity_identifier_code\":\"PE\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"RemittanceClaimPaymentObserved\"") != NULL);
    REQUIRE(strstr(output, "\"claim_id\":\"40d5e288b97e8a83d8f2fa18541f14f4\"") != NULL);
    REQUIRE(strstr(output, "\"claim_id\":\"CLM123\"") == NULL);
    REQUIRE(strstr(output, "\"claim_id_token\"") == NULL);
    REQUIRE(strstr(output, "\"claim_status_code\":\"1\"") != NULL);
    REQUIRE(strstr(output, "\"paid_amount\":\"92.14\"") != NULL);
    REQUIRE(strstr(output, "\"patient_responsibility_amount\":\"20.00\"") != NULL);
    REQUIRE(strstr(output, payer_claim_control_snippet) != NULL);
    REQUIRE(strstr(output, "\"payer_claim_control_number\":\"PAYERCLM123\"") == NULL);
    REQUIRE(strstr(output, "\"payer_claim_control_number_token\"") == NULL);
    REQUIRE(strstr(output, "\"event_type\":\"RemittanceClaimReferencedPatient\"") != NULL);
    REQUIRE(strstr(output, "\"id_value\":\"e5867d5b51d5031be0e06d660c1fd50a\"") != NULL);
    REQUIRE(strstr(output, "\"id_value\":\"PAT67890\"") == NULL);
    REQUIRE(strstr(output, "\"last_name_or_org\"") == NULL);
    REQUIRE(strstr(output, "\"first_name\"") == NULL);
    REQUIRE(strstr(output, "\"event_type\":\"RemittanceServiceLinePaymentObserved\"") != NULL);
    REQUIRE(strstr(output, "\"service_line_number\":\"1\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code_set\":\"CPT/HCPCS\"") != NULL);
    REQUIRE(strstr(output, "\"procedure_code\":\"99213\"") != NULL);
    REQUIRE(strstr(output, "\"line_charge_amount\":\"125.50\"") != NULL);
    REQUIRE(strstr(output, "\"line_paid_amount\":\"92.14\"") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"RemittanceAdjustmentObserved\"") != NULL);
    REQUIRE(strstr(output, "\"adjustment_scope\":\"claim\"") != NULL);
    REQUIRE(strstr(output, "\"adjustment_scope\":\"service_line\"") != NULL);
    REQUIRE(strstr(output, "\"adjustment_group_code\":\"CO\"") != NULL);
    REQUIRE(strstr(output, "\"reason_codes\":[\"45\"]") != NULL);
    REQUIRE(strstr(output, "\"amounts\":[\"13.36\"]") != NULL);
    REQUIRE(strstr(output, "\"event_type\":\"RemittanceDateRecorded\"") != NULL);
    REQUIRE(strstr(output, "\"date_scope\":\"transaction\"") != NULL);
    REQUIRE(strstr(output, "\"date_scope\":\"service_line\"") != NULL);
    REQUIRE(strstr(output, "\"payload\":{\"remittance_id\":\"EFT123456\",\"claim_id\":\"40d5e288b97e8a83d8f2fa18541f14f4\",\"date_scope\":\"service_line\"") != NULL);
    REQUIRE(strstr(output, "\"date_qualifier\":\"472\"") != NULL);
    REQUIRE(strstr(output, "\"date_value\":\"20260601\"") != NULL);

    rc = x12_document_load(input_path, &doc);
    REQUIRE_OK(rc);
    rc = event_writer_open(&writer, phi_out_path, input_path, "835");
    REQUIRE_OK(rc);
    event_writer_set_include_phi(&writer, 1);
    rc = x12_map_835_document(&doc, &writer);
    REQUIRE_OK(rc);
    rc = event_writer_close(&writer);
    REQUIRE_OK(rc);
    x12_document_free(&doc);

    REQUIRE(read_file_text(phi_out_path, phi_output, sizeof(phi_output)) == 0);
    REQUIRE(strstr(phi_output, "\"claim_id\":\"CLM123\"") != NULL);
    REQUIRE(strstr(phi_output, "\"claim_id_token\":\"40d5e288b97e8a83d8f2fa18541f14f4\"") != NULL);
    REQUIRE(strstr(phi_output, "\"payer_claim_control_number\":\"PAYERCLM123\"") != NULL);
    REQUIRE(strstr(phi_output, "\"payer_claim_control_number_token\"") != NULL);
    REQUIRE(strstr(phi_output, payer_claim_control_token) != NULL);
    REQUIRE(strstr(phi_output, "\"name\":\"ACME HEALTH PLAN\"") != NULL);
    REQUIRE(strstr(phi_output, "\"last_name_or_org\":\"DOE\"") != NULL);
    REQUIRE(strstr(phi_output, "\"first_name\":\"JANE\"") != NULL);
    REQUIRE(strstr(phi_output, "\"id_value\":\"PAT67890\"") != NULL);
    REQUIRE(strstr(phi_output, "\"id_value_token\":\"e5867d5b51d5031be0e06d660c1fd50a\"") != NULL);

    return 0;
}

static int test_stroke_encounter_fixture_set(void)
{
    char facility_837_output[30000];
    char facility_835_output[24000];
    char professional_837_output[30000];
    char professional_835_output[24000];

    REQUIRE(parse_fixture_to_output(
                "stroke_encounter/facility_837.edi",
                "837",
                "stroke_facility_837.ndjson",
                facility_837_output,
                sizeof(facility_837_output)
            ) == 0);
    REQUIRE(strstr(facility_837_output, "\"event_type\":\"ClaimObserved\"") != NULL);
    REQUIRE(strstr(facility_837_output, "\"total_charge_amount\":\"2350.00\"") != NULL);
    REQUIRE(strstr(facility_837_output, "\"procedure_code\":\"70450\"") != NULL);
    REQUIRE(strstr(facility_837_output, "\"procedure_code\":\"70460\"") != NULL);
    REQUIRE(strstr(facility_837_output, "\"procedure_code\":\"70551\"") != NULL);
    REQUIRE(strstr(facility_837_output, "\"claim_id\":\"CLM-STROKE-RAD-FAC-001\"") == NULL);
    REQUIRE(strstr(facility_837_output, "\"last_name_or_org\":\"REID\"") == NULL);

    REQUIRE(parse_fixture_to_output(
                "stroke_encounter/facility_835.edi",
                "835",
                "stroke_facility_835.ndjson",
                facility_835_output,
                sizeof(facility_835_output)
            ) == 0);
    REQUIRE(strstr(facility_835_output, "\"event_type\":\"RemittanceClaimPaymentObserved\"") != NULL);
    REQUIRE(strstr(facility_835_output, "\"total_charge_amount\":\"2350.00\"") != NULL);
    REQUIRE(strstr(facility_835_output, "\"paid_amount\":\"1450.00\"") != NULL);
    REQUIRE(strstr(facility_835_output, "\"patient_responsibility_amount\":\"350.00\"") != NULL);
    REQUIRE(strstr(facility_835_output, "\"line_paid_amount\":\"800.00\"") != NULL);
    REQUIRE(strstr(facility_835_output, "\"payer_claim_control_number\":\"PAYER-STROKE-FAC-001\"") == NULL);

    REQUIRE(parse_fixture_to_output(
                "stroke_encounter/professional_837.edi",
                "837",
                "stroke_professional_837.ndjson",
                professional_837_output,
                sizeof(professional_837_output)
            ) == 0);
    REQUIRE(strstr(professional_837_output, "\"event_type\":\"ClaimReferencedRenderingProvider\"") != NULL);
    REQUIRE(strstr(professional_837_output, "\"total_charge_amount\":\"390.00\"") != NULL);
    REQUIRE(strstr(professional_837_output, "\"procedure_code\":\"70450\"") != NULL);
    REQUIRE(strstr(professional_837_output, "\"procedure_code\":\"70460\"") != NULL);
    REQUIRE(strstr(professional_837_output, "\"procedure_code\":\"70551\"") != NULL);

    REQUIRE(parse_fixture_to_output(
                "stroke_encounter/professional_835.edi",
                "835",
                "stroke_professional_835.ndjson",
                professional_835_output,
                sizeof(professional_835_output)
            ) == 0);
    REQUIRE(strstr(professional_835_output, "\"event_type\":\"RemittanceClaimPaymentObserved\"") != NULL);
    REQUIRE(strstr(professional_835_output, "\"total_charge_amount\":\"390.00\"") != NULL);
    REQUIRE(strstr(professional_835_output, "\"paid_amount\":\"260.00\"") != NULL);
    REQUIRE(strstr(professional_835_output, "\"patient_responsibility_amount\":\"40.00\"") != NULL);
    REQUIRE(strstr(professional_835_output, "\"line_paid_amount\":\"120.00\"") != NULL);
    REQUIRE(strstr(professional_835_output, "\"payer_claim_control_number\":\"PAYER-STROKE-PRO-001\"") == NULL);

    return 0;
}

static int test_incremental_claim_stitch_from_source_drops(void)
{
    char facility_837_path[512];
    char facility_835_path[512];
    char journal_837_path[512];
    char journal_835_path[512];
    char read_store_path[512];
    char read_store_wal_path[560];
    char read_store_shm_path[560];
    char first_out_path[512];
    char second_out_path[512];
    char first_out[64000];
    char second_out[64000];
    journal_builder_input_t journal_input;
    aggregate_stitcher_input_t stitch_input;

    REQUIRE(make_path(facility_837_path, sizeof(facility_837_path), TEST_FIXTURE_DIR, "stroke_encounter/facility_837.edi") == 0);
    REQUIRE(make_path(facility_835_path, sizeof(facility_835_path), TEST_FIXTURE_DIR, "stroke_encounter/facility_835.edi") == 0);
    REQUIRE(make_path(journal_837_path, sizeof(journal_837_path), TEST_OUTPUT_DIR, "incremental_facility_837.journal") == 0);
    REQUIRE(make_path(journal_835_path, sizeof(journal_835_path), TEST_OUTPUT_DIR, "incremental_facility_835.journal") == 0);
    REQUIRE(make_path(read_store_path, sizeof(read_store_path), TEST_OUTPUT_DIR, "incremental_claim_store.sqlite") == 0);
    REQUIRE(make_path(first_out_path, sizeof(first_out_path), TEST_OUTPUT_DIR, "incremental_claim_first.ndjson") == 0);
    REQUIRE(make_path(second_out_path, sizeof(second_out_path), TEST_OUTPUT_DIR, "incremental_claim_second.ndjson") == 0);
    REQUIRE(snprintf(read_store_wal_path, sizeof(read_store_wal_path), "%s-wal", read_store_path) > 0);
    REQUIRE(snprintf(read_store_shm_path, sizeof(read_store_shm_path), "%s-shm", read_store_path) > 0);

    (void)remove(journal_837_path);
    (void)remove(journal_835_path);
    (void)remove(read_store_path);
    (void)remove(read_store_wal_path);
    (void)remove(read_store_shm_path);

    journal_builder_input_init(&journal_input);
    journal_input.run_id = "incremental-837-drop";
    REQUIRE_OK(journal_builder_input_add_837(&journal_input, facility_837_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_837_path));

    journal_builder_input_init(&journal_input);
    journal_input.run_id = "incremental-835-drop";
    REQUIRE_OK(journal_builder_input_add_835(&journal_input, facility_835_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_835_path));

    aggregate_stitcher_input_init(&stitch_input);
    stitch_input.journal_path = journal_837_path;
    stitch_input.read_store_path = read_store_path;
    stitch_input.out_path = first_out_path;
    stitch_input.incremental = 1;
    stitch_input.run_id = "incremental-stitch-837";
    REQUIRE_OK(aggregate_stitcher_stitch(&stitch_input));
    REQUIRE(read_file_text(first_out_path, first_out, sizeof(first_out)) == 0);
    REQUIRE(strstr(first_out, "\"aggregate_id\":\"claim:8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(first_out, "\"version\":1") != NULL);
    REQUIRE(strstr(first_out, "\"has_837\":true") != NULL);
    REQUIRE(strstr(first_out, "\"has_835\":true") == NULL);
    REQUIRE(strstr(first_out, "\"source_event_count\":14") != NULL);

    aggregate_stitcher_input_init(&stitch_input);
    stitch_input.journal_path = journal_835_path;
    stitch_input.read_store_path = read_store_path;
    stitch_input.out_path = second_out_path;
    stitch_input.incremental = 1;
    stitch_input.run_id = "incremental-stitch-835";
    REQUIRE_OK(aggregate_stitcher_stitch(&stitch_input));
    REQUIRE(read_file_text(second_out_path, second_out, sizeof(second_out)) == 0);
    REQUIRE(strstr(second_out, "\"aggregate_id\":\"claim:8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(second_out, "\"version\":2") != NULL);
    REQUIRE(strstr(second_out, "\"has_837\":true") != NULL);
    REQUIRE(strstr(second_out, "\"has_835\":true") != NULL);
    REQUIRE(strstr(second_out, "\"source_event_count\":28") != NULL);
    REQUIRE(strstr(second_out, "\"submitted_service_line_count\":3") != NULL);
    REQUIRE(strstr(second_out, "\"remittance_service_line_count\":3") != NULL);
    REQUIRE(count_substring(second_out, "\"event_type\":\"ClaimAggregateUpdated\"") == 1u);

    (void)remove(journal_837_path);
    (void)remove(journal_835_path);
    (void)remove(read_store_path);
    (void)remove(read_store_wal_path);
    (void)remove(read_store_shm_path);

    return 0;
}

static int test_incremental_coverage_stitch_from_source_drops(void)
{
    char coverage_834_path[512];
    char eligibility_270_path[512];
    char journal_834_path[512];
    char journal_270_path[512];
    char read_store_path[512];
    char read_store_wal_path[560];
    char read_store_shm_path[560];
    char first_out_path[512];
    char second_out_path[512];
    char aggregate_id[160];
    char member_token[TOKENISE_MAX_TOKEN_LEN];
    char latest_coverage[65536];
    char *first_out = NULL;
    char *second_out = NULL;
    x12_str_t raw;
    journal_builder_input_t journal_input;
    coverage_stitcher_input_t stitch_input;
    scribe_store_t store;
    size_t latest_version = 0u;

    REQUIRE_ALLOC(first_out, 64000u);
    REQUIRE_ALLOC(second_out, 64000u);

    raw.ptr = "SUB-STROKE-001";
    raw.len = strlen(raw.ptr);
    REQUIRE_OK(tokenise_value(TOK_MEMBER_ID, raw, member_token, sizeof(member_token)));
    REQUIRE(snprintf(aggregate_id, sizeof(aggregate_id), "member_coverage:%s", member_token) > 0);

    REQUIRE(make_path(coverage_834_path, sizeof(coverage_834_path), TEST_FIXTURE_DIR, "stroke_encounter/coverage_834.edi") == 0);
    REQUIRE(make_path(eligibility_270_path, sizeof(eligibility_270_path), TEST_FIXTURE_DIR, "stroke_encounter/eligibility_270.edi") == 0);
    REQUIRE(make_path(journal_834_path, sizeof(journal_834_path), TEST_OUTPUT_DIR, "incremental_coverage_834.journal") == 0);
    REQUIRE(make_path(journal_270_path, sizeof(journal_270_path), TEST_OUTPUT_DIR, "incremental_coverage_270.journal") == 0);
    REQUIRE(make_path(read_store_path, sizeof(read_store_path), TEST_OUTPUT_DIR, "incremental_coverage_store.sqlite") == 0);
    REQUIRE(make_path(first_out_path, sizeof(first_out_path), TEST_OUTPUT_DIR, "incremental_coverage_first.ndjson") == 0);
    REQUIRE(make_path(second_out_path, sizeof(second_out_path), TEST_OUTPUT_DIR, "incremental_coverage_second.ndjson") == 0);
    REQUIRE(snprintf(read_store_wal_path, sizeof(read_store_wal_path), "%s-wal", read_store_path) > 0);
    REQUIRE(snprintf(read_store_shm_path, sizeof(read_store_shm_path), "%s-shm", read_store_path) > 0);

    (void)remove(journal_834_path);
    (void)remove(journal_270_path);
    (void)remove(read_store_path);
    (void)remove(read_store_wal_path);
    (void)remove(read_store_shm_path);

    journal_builder_input_init(&journal_input);
    journal_input.run_id = "incremental-834-drop";
    REQUIRE_OK(journal_builder_input_add_834(&journal_input, coverage_834_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_834_path));

    journal_builder_input_init(&journal_input);
    journal_input.run_id = "incremental-270-drop";
    REQUIRE_OK(journal_builder_input_add_270(&journal_input, eligibility_270_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_270_path));

    coverage_stitcher_input_init(&stitch_input);
    stitch_input.journal_path = journal_834_path;
    stitch_input.read_store_path = read_store_path;
    stitch_input.out_path = first_out_path;
    stitch_input.incremental = 1;
    stitch_input.run_id = "incremental-coverage-834";
    REQUIRE_OK(coverage_stitcher_stitch(&stitch_input));
    REQUIRE(read_file_text(first_out_path, first_out, 64000u) == 0);
    REQUIRE(count_substring(first_out, "\"event_type\":\"MemberCoverageUpdated\"") == 1u);
    REQUIRE(strstr(first_out, aggregate_id) != NULL);
    REQUIRE(strstr(first_out, "\"version\":1") != NULL);
    REQUIRE(strstr(first_out, "\"source_event_count\":3") != NULL);
    REQUIRE(strstr(first_out, "\"health_coverage\"") != NULL);

    coverage_stitcher_input_init(&stitch_input);
    stitch_input.journal_path = journal_270_path;
    stitch_input.read_store_path = read_store_path;
    stitch_input.out_path = second_out_path;
    stitch_input.incremental = 1;
    stitch_input.run_id = "incremental-coverage-270";
    REQUIRE_OK(coverage_stitcher_stitch(&stitch_input));
    REQUIRE(read_file_text(second_out_path, second_out, 64000u) == 0);
    REQUIRE(count_substring(second_out, "\"event_type\":\"MemberCoverageUpdated\"") == 1u);
    REQUIRE(strstr(second_out, aggregate_id) != NULL);
    REQUIRE(strstr(second_out, "\"version\":2") != NULL);
    REQUIRE(strstr(second_out, "\"source_event_count\":9") != NULL);
    REQUIRE(strstr(second_out, "\"service_request_count\":3") != NULL);

    scribe_store_init(&store);
    REQUIRE_OK(scribe_store_open(&store, read_store_path));
    REQUIRE_OK(scribe_store_init_schema(&store));
    REQUIRE_OK(scribe_store_get_latest_member_coverage(
                &store,
                aggregate_id,
                &latest_version,
                latest_coverage,
                sizeof(latest_coverage)
            ));
    REQUIRE(latest_version == 2u);
    REQUIRE(strstr(latest_coverage, "\"source_event_count\":9") != NULL);
    REQUIRE_OK(scribe_store_close(&store));

    (void)remove(journal_834_path);
    (void)remove(journal_270_path);
    (void)remove(read_store_path);
    (void)remove(read_store_wal_path);
    (void)remove(read_store_shm_path);

    TEST_FREE_ALLOCATIONS();
    return 0;
}

static int test_837_claim_context_aggregate_state(void)
{
    char fixture_path[512];
    char journal_path[512];
    char aggregate_path[512];
    char aggregates[131072];
    journal_builder_input_t journal_input;
    aggregate_stitcher_input_t stitch_input;

    REQUIRE(make_path(
                fixture_path,
                sizeof(fixture_path),
                TEST_FIXTURE_DIR,
                "x12_005010x222_example_01_synthetic.edi"
            ) == 0);
    REQUIRE(make_path(
                journal_path,
                sizeof(journal_path),
                TEST_OUTPUT_DIR,
                "claim_context_837.journal"
            ) == 0);
    REQUIRE(make_path(
                aggregate_path,
                sizeof(aggregate_path),
                TEST_OUTPUT_DIR,
                "claim_context_aggregate.ndjson"
            ) == 0);

    (void)remove(journal_path);
    (void)remove(aggregate_path);

    journal_builder_input_init(&journal_input);
    journal_input.run_id = "claim-context-837";
    REQUIRE_OK(journal_builder_input_add_837(&journal_input, fixture_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_path));

    aggregate_stitcher_input_init(&stitch_input);
    stitch_input.journal_path = journal_path;
    stitch_input.out_path = aggregate_path;
    stitch_input.run_id = "claim-context-stitch";
    REQUIRE_OK(aggregate_stitcher_stitch(&stitch_input));
    REQUIRE(read_file_text(aggregate_path, aggregates, sizeof(aggregates)) == 0);

    REQUIRE(strstr(aggregates, "\"event_type\":\"ClaimAggregateUpdated\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_envelope\":{\"total_charge_amount\":\"100\",\"facility_type_code\":\"11\",\"facility_code_qualifier\":\"B\",\"claim_frequency_type_code\":\"1\"") != NULL);
    REQUIRE(strstr(aggregates, "\"provider_signature_indicator\":\"Y\"") != NULL);
    REQUIRE(strstr(aggregates, "\"assignment_or_plan_participation_code\":\"A\"") != NULL);
    REQUIRE(strstr(aggregates, "\"benefits_assignment_certification_indicator\":\"Y\"") != NULL);
    REQUIRE(strstr(aggregates, "\"release_of_information_code\":\"I\"") != NULL);
    REQUIRE(strstr(aggregates, "\"subscriber\":{\"payer_responsibility_sequence_number_code\":\"P\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_filing_indicator_code\":\"CI\"") != NULL);
    REQUIRE(strstr(aggregates, "\"patient\":{\"payer_responsibility_sequence_number_code\":\"\",\"individual_relationship_code\":\"19\"") != NULL);
    REQUIRE(strstr(aggregates, "\"gender_code\":\"F\"") != NULL);
    REQUIRE(strstr(aggregates, "\"gender_code\":\"M\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_dates\":[{\"date_qualifier\":\"434\",\"date_format\":\"RD8\",\"date_value\":\"20260603-20260610\"}]") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_references\":[{\"reference_scope\":\"claim\",\"service_line_number\":\"\",\"reference_qualifier\":\"D9\"") != NULL);
    REQUIRE(strstr(aggregates, "\"references\":[{\"reference_scope\":\"service_line\",\"service_line_number\":\"1\",\"reference_qualifier\":\"6R\"") != NULL);
    REQUIRE(strstr(aggregates, "\"diagnoses\":{\"principal_diagnosis_code\":\"J02.9\",\"other_diagnosis_codes\":[\"R51.9\"]}") != NULL);
    REQUIRE(strstr(aggregates, "\"healthcare_codes\":[") != NULL);
    REQUIRE(strstr(aggregates, "\"healthcare_code_qualifier\":\"BK\"") != NULL);
    REQUIRE(strstr(aggregates, "\"healthcare_code_qualifier\":\"BF\"") != NULL);
    REQUIRE(strstr(aggregates, "\"provider_taxonomies\":[{\"reference_scope\":\"claim\",\"service_line_number\":\"\",\"provider_context\":\"billing_provider\",\"provider_role_code\":\"BI\"") != NULL);
    REQUIRE(strstr(aggregates, "\"provider_taxonomy_code\":\"203BF0100Y\"") != NULL);
    REQUIRE(strstr(aggregates, "GROUP-837P") == NULL);
    REQUIRE(strstr(aggregates, "SYNTHCLEARING001") == NULL);
    REQUIRE(strstr(aggregates, "LINECTRL1") == NULL);
    REQUIRE(strstr(aggregates, "19430501") == NULL);
    REQUIRE(strstr(aggregates, "19730501") == NULL);

    (void)remove(journal_path);
    (void)remove(aggregate_path);
    return 0;
}

static int test_837_institutional_context_aggregate_state(void)
{
    char fixture_path[512];
    char journal_path[512];
    char aggregate_path[512];
    char aggregates[131072];
    journal_builder_input_t journal_input;
    aggregate_stitcher_input_t stitch_input;

    REQUIRE(make_path(
                fixture_path,
                sizeof(fixture_path),
                TEST_FIXTURE_DIR,
                "sample_837i_hi_components.edi"
            ) == 0);
    REQUIRE(make_path(
                journal_path,
                sizeof(journal_path),
                TEST_OUTPUT_DIR,
                "institutional_context_837.journal"
            ) == 0);
    REQUIRE(make_path(
                aggregate_path,
                sizeof(aggregate_path),
                TEST_OUTPUT_DIR,
                "institutional_context_aggregate.ndjson"
            ) == 0);

    (void)remove(journal_path);
    (void)remove(aggregate_path);

    journal_builder_input_init(&journal_input);
    journal_input.run_id = "institutional-context-837";
    REQUIRE_OK(journal_builder_input_add_837(&journal_input, fixture_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_path));

    aggregate_stitcher_input_init(&stitch_input);
    stitch_input.journal_path = journal_path;
    stitch_input.out_path = aggregate_path;
    stitch_input.run_id = "institutional-context-stitch";
    REQUIRE_OK(aggregate_stitcher_stitch(&stitch_input));
    REQUIRE(read_file_text(aggregate_path, aggregates, sizeof(aggregates)) == 0);

    REQUIRE(strstr(aggregates, "\"event_type\":\"ClaimAggregateUpdated\"") != NULL);
    REQUIRE(strstr(aggregates, "\"institutional_claim\":{\"admission_type_code\":\"1\",\"admission_source_code\":\"7\",\"patient_status_code\":\"30\",\"admission_date\":\"20260601\",\"discharge_date\":\"20260604\",\"diagnosis_related_group_code\":\"470\"}") != NULL);
    REQUIRE(strstr(aggregates, "\"healthcare_code_kind\":\"condition_code\"") != NULL);
    REQUIRE(strstr(aggregates, "\"healthcare_code_kind\":\"occurrence_code\"") != NULL);
    REQUIRE(strstr(aggregates, "\"healthcare_code_kind\":\"value_code\"") != NULL);
    REQUIRE(strstr(aggregates, "\"healthcare_code_amount\":\"2500.00\"") != NULL);
    REQUIRE(strstr(aggregates, "\"healthcare_code_kind\":\"diagnosis_related_group\"") != NULL);
    REQUIRE(strstr(aggregates, "\"healthcare_code\":\"470\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_dates\":[") != NULL);
    REQUIRE(strstr(aggregates, "\"date_qualifier\":\"435\"") != NULL);
    REQUIRE(strstr(aggregates, "\"date_qualifier\":\"096\"") != NULL);
    REQUIRE(strstr(aggregates, "CLM401") == NULL);
    REQUIRE(strstr(aggregates, "SUB44444") == NULL);

    (void)remove(journal_path);
    (void)remove(aggregate_path);
    return 0;
}

static int test_stroke_balance_projection_from_journal(void)
{
    char coverage_834_path[512];
    char eligibility_270_path[512];
    char eligibility_271_path[512];
    char facility_837_path[512];
    char facility_835_path[512];
    char professional_837_path[512];
    char professional_835_path[512];
    char rehab_837_path[512];
    char rehab_835_path[512];
    char neurology_837_path[512];
    char neurology_835_path[512];
    char journal_path[512];
    char nonphi_journal_path[512];
    char aggregate_path[512];
    char nonphi_aggregate_path[512];
    char resolved_phi_aggregate_path[512];
    char notification_path[512];
    char nonphi_read_store_path[512];
    char nonphi_read_store_wal_path[560];
    char nonphi_read_store_shm_path[560];
    char resolved_phi_read_store_path[512];
    char resolved_phi_read_store_wal_path[560];
    char resolved_phi_read_store_shm_path[560];
    char projection_path[512];
    char nonphi_projection_path[512];
    char phi_vault_path[512];
    char phi_vault_wal_path[560];
    char phi_vault_shm_path[560];
    enum {
        AGGREGATES_LEN = 320000,
        NOTIFICATIONS_LEN = 96000,
        LATEST_AGGREGATE_LEN = 65536,
        PROJECTION_LEN = 96000
    };
    char *aggregates = NULL;
    char *notifications = NULL;
    char *latest_aggregate = NULL;
    char *projection = NULL;
    char resolved[256];
    char indexed_event_ids[16][SCRIBE_STORE_ID_MAX];
    char aggregate_ids[4][SCRIBE_STORE_ID_MAX];
    char first_source_drop_id[SCRIBE_STORE_ID_MAX];
    char last_source_drop_id[SCRIBE_STORE_ID_MAX];
    char patient_name_token[TOKENISE_MAX_TOKEN_LEN];
    char event_type[128];
    char source_transaction[32];
    x12_str_t conflicting_raw;
    x12_str_t patient_name_raw;
    journal_builder_input_t journal_input;
    journal_reader_t journal_reader;
    journal_event_t journal_event;
    aggregate_stitcher_input_t stitch_input;
    balance_projector_input_t projection_input;
    phi_vault_t phi_vault;
    scribe_store_t read_store;
    scribe_event_locator_t indexed_locator;
    scribe_source_drop_t indexed_source_drop;
    size_t indexed_event_count = 0u;
    size_t aggregate_id_count = 0u;
    size_t latest_version = 0u;
    int saw_stroke_834 = 0;
    int saw_stroke_270 = 0;
    int saw_stroke_271 = 0;
    int rc;

    REQUIRE_ALLOC(aggregates, AGGREGATES_LEN);
    REQUIRE_ALLOC(notifications, NOTIFICATIONS_LEN);
    REQUIRE_ALLOC(latest_aggregate, LATEST_AGGREGATE_LEN);
    REQUIRE_ALLOC(projection, PROJECTION_LEN);

    REQUIRE(make_path(coverage_834_path, sizeof(coverage_834_path), TEST_FIXTURE_DIR, "stroke_encounter/coverage_834.edi") == 0);
    REQUIRE(make_path(eligibility_270_path, sizeof(eligibility_270_path), TEST_FIXTURE_DIR, "stroke_encounter/eligibility_270.edi") == 0);
    REQUIRE(make_path(eligibility_271_path, sizeof(eligibility_271_path), TEST_FIXTURE_DIR, "stroke_encounter/eligibility_271.edi") == 0);
    REQUIRE(make_path(facility_837_path, sizeof(facility_837_path), TEST_FIXTURE_DIR, "stroke_encounter/facility_837.edi") == 0);
    REQUIRE(make_path(facility_835_path, sizeof(facility_835_path), TEST_FIXTURE_DIR, "stroke_encounter/facility_835.edi") == 0);
    REQUIRE(make_path(professional_837_path, sizeof(professional_837_path), TEST_FIXTURE_DIR, "stroke_encounter/professional_837.edi") == 0);
    REQUIRE(make_path(professional_835_path, sizeof(professional_835_path), TEST_FIXTURE_DIR, "stroke_encounter/professional_835.edi") == 0);
    REQUIRE(make_path(rehab_837_path, sizeof(rehab_837_path), TEST_FIXTURE_DIR, "stroke_encounter/rehab_837.edi") == 0);
    REQUIRE(make_path(rehab_835_path, sizeof(rehab_835_path), TEST_FIXTURE_DIR, "stroke_encounter/rehab_835.edi") == 0);
    REQUIRE(make_path(neurology_837_path, sizeof(neurology_837_path), TEST_FIXTURE_DIR, "stroke_encounter/neurology_837.edi") == 0);
    REQUIRE(make_path(neurology_835_path, sizeof(neurology_835_path), TEST_FIXTURE_DIR, "stroke_encounter/neurology_835.edi") == 0);
    REQUIRE(make_path(journal_path, sizeof(journal_path), TEST_OUTPUT_DIR, "stroke_encounter.journal") == 0);
    REQUIRE(make_path(nonphi_journal_path, sizeof(nonphi_journal_path), TEST_OUTPUT_DIR, "stroke_encounter_nonphi.journal") == 0);
    REQUIRE(make_path(aggregate_path, sizeof(aggregate_path), TEST_OUTPUT_DIR, "stroke_claim_aggregates.ndjson") == 0);
    REQUIRE(make_path(nonphi_aggregate_path, sizeof(nonphi_aggregate_path), TEST_OUTPUT_DIR, "stroke_claim_aggregates_nonphi.ndjson") == 0);
    REQUIRE(make_path(resolved_phi_aggregate_path, sizeof(resolved_phi_aggregate_path), TEST_OUTPUT_DIR, "stroke_claim_aggregates_resolved_phi.ndjson") == 0);
    REQUIRE(make_path(notification_path, sizeof(notification_path), TEST_OUTPUT_DIR, "stroke_notifications.ndjson") == 0);
    REQUIRE(make_path(nonphi_read_store_path, sizeof(nonphi_read_store_path), TEST_OUTPUT_DIR, "stroke_read_store_nonphi.sqlite") == 0);
    REQUIRE(make_path(resolved_phi_read_store_path, sizeof(resolved_phi_read_store_path), TEST_OUTPUT_DIR, "stroke_read_store_resolved_phi.sqlite") == 0);
    REQUIRE(make_path(projection_path, sizeof(projection_path), TEST_OUTPUT_DIR, "stroke_balance_projection.json") == 0);
    REQUIRE(make_path(nonphi_projection_path, sizeof(nonphi_projection_path), TEST_OUTPUT_DIR, "stroke_balance_projection_nonphi.json") == 0);
    REQUIRE(make_path(phi_vault_path, sizeof(phi_vault_path), TEST_OUTPUT_DIR, "stroke_phi_vault.sqlite") == 0);
    REQUIRE(snprintf(nonphi_read_store_wal_path, sizeof(nonphi_read_store_wal_path), "%s-wal", nonphi_read_store_path) > 0);
    REQUIRE(snprintf(nonphi_read_store_shm_path, sizeof(nonphi_read_store_shm_path), "%s-shm", nonphi_read_store_path) > 0);
    REQUIRE(snprintf(resolved_phi_read_store_wal_path, sizeof(resolved_phi_read_store_wal_path), "%s-wal", resolved_phi_read_store_path) > 0);
    REQUIRE(snprintf(resolved_phi_read_store_shm_path, sizeof(resolved_phi_read_store_shm_path), "%s-shm", resolved_phi_read_store_path) > 0);
    REQUIRE(snprintf(phi_vault_wal_path, sizeof(phi_vault_wal_path), "%s-wal", phi_vault_path) > 0);
    REQUIRE(snprintf(phi_vault_shm_path, sizeof(phi_vault_shm_path), "%s-shm", phi_vault_path) > 0);

    journal_builder_input_init(&journal_input);
    journal_input.include_phi = 1;
    journal_input.run_id = "ingest-test-run";
    journal_input.source_root = TEST_FIXTURE_DIR;
    REQUIRE_OK(journal_builder_input_add_834(&journal_input, coverage_834_path));
    REQUIRE_OK(journal_builder_input_add_270(&journal_input, eligibility_270_path));
    REQUIRE_OK(journal_builder_input_add_271(&journal_input, eligibility_271_path));
    REQUIRE_OK(journal_builder_input_add_837(&journal_input, facility_837_path));
    REQUIRE_OK(journal_builder_input_add_837(&journal_input, professional_837_path));
    REQUIRE_OK(journal_builder_input_add_837(&journal_input, rehab_837_path));
    REQUIRE_OK(journal_builder_input_add_837(&journal_input, neurology_837_path));
    REQUIRE_OK(journal_builder_input_add_835(&journal_input, facility_835_path));
    REQUIRE_OK(journal_builder_input_add_835(&journal_input, professional_835_path));
    REQUIRE_OK(journal_builder_input_add_835(&journal_input, rehab_835_path));
    REQUIRE_OK(journal_builder_input_add_835(&journal_input, neurology_835_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_path));

    journal_reader_init(&journal_reader);
    REQUIRE_OK(journal_reader_open(&journal_reader, journal_path));
    while (1) {
        rc = journal_reader_next(&journal_reader, &journal_event);
        REQUIRE_OK(rc);
        if (journal_event.record_len == 0u) {
            break;
        }
        event_type[0] = '\0';
        source_transaction[0] = '\0';
        (void)journal_event_get_string(&journal_event, "event_type", event_type, sizeof(event_type));
        (void)journal_event_get_string(
            &journal_event,
            "source_transaction",
            source_transaction,
            sizeof(source_transaction)
        );
        if (strcmp(source_transaction, "834") == 0 &&
            strcmp(event_type, "MemberEnrollmentChanged") == 0) {
            saw_stroke_834 = 1;
        }
        if (strcmp(source_transaction, "270") == 0 &&
            strcmp(event_type, "EligibilityInquiryServiceTypeRequested") == 0) {
            saw_stroke_270 = 1;
        }
        if (strcmp(source_transaction, "271") == 0 &&
            strcmp(event_type, "EligibilityBenefitObserved") == 0) {
            saw_stroke_271 = 1;
        }
    }
    REQUIRE_OK(journal_reader_close(&journal_reader));
    REQUIRE(saw_stroke_834);
    REQUIRE(saw_stroke_270);
    REQUIRE(saw_stroke_271);

    aggregate_stitcher_input_init(&stitch_input);
    (void)remove(resolved_phi_read_store_path);
    (void)remove(resolved_phi_read_store_wal_path);
    (void)remove(resolved_phi_read_store_shm_path);
    stitch_input.journal_path = journal_path;
    stitch_input.out_path = aggregate_path;
    stitch_input.notify_out_path = notification_path;
    stitch_input.read_store_path = resolved_phi_read_store_path;
    stitch_input.include_phi = 1;
    stitch_input.run_id = "stitch-test-run";
    REQUIRE_OK(aggregate_stitcher_stitch(&stitch_input));
    REQUIRE(read_file_text(aggregate_path, aggregates, AGGREGATES_LEN) == 0);
    REQUIRE(read_file_text(notification_path, notifications, NOTIFICATIONS_LEN) == 0);

    REQUIRE(strstr(aggregates, "\"event_type\":\"ClaimAggregateUpdated\"") != NULL);
    REQUIRE(strstr(aggregates, "\"run_id\":\"stitch-test-run\"") != NULL);
    REQUIRE(strstr(aggregates, "\"source_run_id\":\"ingest-test-run\"") != NULL);
    REQUIRE(count_substring(aggregates, "\"event_type\":\"ClaimAggregateUpdated\"") == 8u);
    REQUIRE(strstr(notifications, "\"event_type\":\"AggregateVersionRecorded\"") != NULL);
    REQUIRE(strstr(notifications, "\"ok\":true") != NULL);
    REQUIRE(count_substring(notifications, "\"event_type\":\"AggregateVersionRecorded\"") == 8u);
    REQUIRE(strstr(notifications, "\"run_id\":\"stitch-test-run\"") != NULL);
    REQUIRE(strstr(notifications, "\"source_run_id\":\"ingest-test-run\"") != NULL);
    REQUIRE(strstr(notifications, "\"notification_id\":\"claim:8259c238232f9585e95fc8f45b0bb410:2\"") != NULL);
    REQUIRE(strstr(notifications, "\"aggregate_id\":\"claim:8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(notifications, "\"version\":2") != NULL);
    REQUIRE(strstr(notifications, "\"updated_by_journal_offset\"") != NULL);
    REQUIRE(strstr(notifications, "\"updated_by_journal_length\"") != NULL);
    REQUIRE(strstr(notifications, "CLM-STROKE") == NULL);
    REQUIRE(strstr(notifications, "PAYER-STROKE") == NULL);
    REQUIRE(strstr(notifications, "PAT-STROKE") == NULL);
    REQUIRE(strstr(notifications, "SUB-STROKE") == NULL);
    REQUIRE(strstr(notifications, "REID") == NULL);
    REQUIRE(strstr(notifications, "ALEX") == NULL);
    REQUIRE(strstr(aggregates, "\"aggregate_type\":\"claim\"") != NULL);
    REQUIRE(strstr(aggregates, "\"aggregate_id\":\"claim:8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"CLM-STROKE-RAD-FAC-001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"CLM-STROKE-RAD-PRO-001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"CLM-STROKE-REHAB-001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"CLM-STROKE-NEURO-001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"patient_id\":\"PAT-STROKE-001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"patient_id_token\":\"483f7b234ed109f0e2323052f22e4e59\"") != NULL);
    REQUIRE(strstr(aggregates, "\"patient_last_name_or_org\":\"REID\"") != NULL);
    REQUIRE(strstr(aggregates, "\"patient_first_name\":\"ALEX\"") != NULL);
    REQUIRE(strstr(aggregates, "\"patient_name_token\":\"4b108ab4544b581362f5809685352233\"") != NULL);
    REQUIRE(strstr(aggregates, "\"version\":1") != NULL);
    REQUIRE(strstr(aggregates, "\"version\":2") != NULL);
    REQUIRE(strstr(aggregates, "\"version\":3") == NULL);
    REQUIRE(strstr(aggregates, "\"updated_by_event_id\"") != NULL);
    REQUIRE(strstr(aggregates, "\"update_scope\":\"source_drop\"") != NULL);
    REQUIRE(strstr(aggregates, "\"source_drop_id\":\"837:000000101:101:0001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"source_drop_id\":\"835:000000102:102:0001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"compacted_source_event_count\":14") != NULL);
    REQUIRE(strstr(aggregates, "\"applied_event_ids\":[") != NULL);
    REQUIRE(strstr(aggregates, "\"update_event_ids\":[") != NULL);
    REQUIRE(strstr(aggregates, "\"source_event_ids\":[") == NULL);
    REQUIRE(strstr(aggregates, "\"updated_source_event_ids\":[") == NULL);
    REQUIRE(strstr(aggregates, "\"has_837\":true") != NULL);
    REQUIRE(strstr(aggregates, "\"has_835\":true") != NULL);
    REQUIRE(strstr(aggregates, "\"submitted_service_line_count\":3") != NULL);
    REQUIRE(strstr(aggregates, "\"remittance_service_line_count\":3") != NULL);
    REQUIRE(strstr(aggregates, "\"adjustment_count\":6") != NULL);
    REQUIRE(strstr(aggregates, "\"service_lines\":[") != NULL);
    REQUIRE(strstr(aggregates, "\"submitted\":{\"line_type\":\"SV1\",\"procedure_code_qualifier\":\"HC\",\"procedure_code_set\":\"CPT/HCPCS\",\"charge_amount\":\"300.00\",\"unit_measure_code\":\"UN\",\"unit_count\":\"3\",\"service_date\":\"20260617\"}") != NULL);
    REQUIRE(strstr(aggregates, "\"remittance\":{\"procedure_code_qualifier\":\"HC\",\"procedure_code_set\":\"CPT/HCPCS\",\"line_charge_amount\":\"300.00\",\"line_paid_amount\":\"180.00\",\"paid_service_unit_count\":\"3\",\"service_date\":\"20260617\"}") != NULL);
    REQUIRE(strstr(aggregates, "\"match_method\":\"procedure_charge_date\"") != NULL);
    REQUIRE(strstr(aggregates, "\"adjustments\":[{\"adjustment_group_code\":\"CO\",\"reason_codes\":[\"45\"],\"amounts\":[\"60.00\"],\"quantities\":[\"\"]},{\"adjustment_group_code\":\"PR\",\"reason_codes\":[\"3\"],\"amounts\":[\"60.00\"],\"quantities\":[\"\"]}]") != NULL);
    REQUIRE(strstr(aggregates, "\"payer_claim_control_number\":\"PAYER-STROKE-FAC-001\"") != NULL);

    balance_projector_input_init(&projection_input);
    projection_input.read_store_path = resolved_phi_read_store_path;
    projection_input.include_phi = 1;
    REQUIRE_OK(balance_projector_project(&projection_input, projection_path));
    REQUIRE(read_file_text(projection_path, projection, PROJECTION_LEN) == 0);

    REQUIRE(strstr(projection, "\"event_type\":\"ClaimBalanceProjected\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_id\":\"CLM-STROKE-RAD-FAC-001\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_id\":\"CLM-STROKE-RAD-PRO-001\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_id\":\"CLM-STROKE-REHAB-001\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_id\":\"CLM-STROKE-NEURO-001\"") != NULL);
    REQUIRE(strstr(projection, "\"match_method\":\"procedure_charge_date\"") != NULL);
    REQUIRE(strstr(projection, "\"entry_type\":\"billed_charge\"") != NULL);
    REQUIRE(strstr(projection, "\"entry_type\":\"payer_payment\"") != NULL);
    REQUIRE(strstr(projection, "\"entry_type\":\"contractual_adjustment\"") != NULL);
    REQUIRE(strstr(projection, "\"entry_type\":\"patient_responsibility\"") != NULL);
    REQUIRE(strstr(projection, "\"total_billed\":\"3720.00\"") != NULL);
    REQUIRE(strstr(projection, "\"payer_paid\":\"2340.00\"") != NULL);
    REQUIRE(strstr(projection, "\"contractual_adjustments\":\"830.00\"") != NULL);
    REQUIRE(strstr(projection, "\"patient_responsibility\":\"550.00\"") != NULL);
    REQUIRE(strstr(projection, "\"current_balance\":\"550.00\"") != NULL);

    (void)remove(phi_vault_path);
    (void)remove(phi_vault_wal_path);
    (void)remove(phi_vault_shm_path);

    journal_input.include_phi = 0;
    journal_input.phi_vault_path = phi_vault_path;
    REQUIRE_OK(journal_builder_build(&journal_input, nonphi_journal_path));

    phi_vault_init(&phi_vault);
    REQUIRE_OK(phi_vault_open(&phi_vault, phi_vault_path));
    REQUIRE_OK(phi_vault_init_schema(&phi_vault));
    REQUIRE_OK(phi_vault_resolve(
                &phi_vault,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                "test",
                "unit",
                resolved,
                sizeof(resolved)
            ));
    REQUIRE_STR(resolved, "CLM-STROKE-RAD-FAC-001");
    REQUIRE(phi_mapping_source_drops(
                &phi_vault,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                first_source_drop_id,
                sizeof(first_source_drop_id),
                last_source_drop_id,
                sizeof(last_source_drop_id)
            ) == 0);
    REQUIRE_STR(first_source_drop_id, "837:000000101:101:0001");
    REQUIRE_STR(last_source_drop_id, "835:000000102:102:0001");
    REQUIRE_OK(phi_vault_resolve(
                &phi_vault,
                "payer_claim_control_number",
                "edf29f09740ab104da309e2b036e14d1",
                "test",
                "unit",
                resolved,
                sizeof(resolved)
            ));
    REQUIRE_STR(resolved, "PAYER-STROKE-FAC-001");
    REQUIRE_OK(phi_vault_resolve(
                &phi_vault,
                "patient_id_name",
                "483f7b234ed109f0e2323052f22e4e59",
                "test",
                "unit",
                resolved,
                sizeof(resolved)
            ));
    REQUIRE_STR(resolved, "REID|ALEX");
    patient_name_raw.ptr = "REID|ALEX";
    patient_name_raw.len = strlen(patient_name_raw.ptr);
    REQUIRE_OK(tokenise_value(
                TOK_PATIENT_NAME,
                patient_name_raw,
                patient_name_token,
                sizeof(patient_name_token)
            ));
    REQUIRE_OK(phi_vault_resolve(
                &phi_vault,
                "patient_name",
                patient_name_token,
                "test",
                "unit",
                resolved,
                sizeof(resolved)
            ));
    REQUIRE_STR(resolved, "REID|ALEX");
    conflicting_raw.ptr = "DIFFERENT-RAW-CLAIM";
    conflicting_raw.len = strlen(conflicting_raw.ptr);
    REQUIRE(phi_vault_put_mapping(
                &phi_vault,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                conflicting_raw,
                "test-conflict"
            ) == X12_ERR_CONFLICT);
    REQUIRE_OK(phi_vault_close(&phi_vault));
    (void)remove(resolved_phi_read_store_path);
    (void)remove(resolved_phi_read_store_wal_path);
    (void)remove(resolved_phi_read_store_shm_path);
    stitch_input.journal_path = nonphi_journal_path;
    stitch_input.out_path = resolved_phi_aggregate_path;
    stitch_input.read_store_path = resolved_phi_read_store_path;
    stitch_input.phi_vault_path = phi_vault_path;
    stitch_input.include_phi = 1;
    REQUIRE_OK(aggregate_stitcher_stitch(&stitch_input));
    scribe_store_init(&read_store);
    REQUIRE_OK(scribe_store_open(&read_store, resolved_phi_read_store_path));
    REQUIRE_OK(scribe_store_init_schema(&read_store));
    REQUIRE_OK(scribe_store_get_latest_claim_aggregate(
                &read_store,
                "claim:8259c238232f9585e95fc8f45b0bb410",
                &latest_version,
                latest_aggregate,
                LATEST_AGGREGATE_LEN
            ));
    REQUIRE(latest_version == 2u);
    REQUIRE(strstr(latest_aggregate, "\"contains_phi\":true") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"claim_id\":\"CLM-STROKE-RAD-FAC-001\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"claim_id_token\":\"8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"payer_claim_control_number\":\"PAYER-STROKE-FAC-001\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"payer_claim_control_number_token\":\"edf29f09740ab104da309e2b036e14d1\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"patient_id\":\"PAT-STROKE-001\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"patient_id_token\":\"483f7b234ed109f0e2323052f22e4e59\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"patient_last_name_or_org\":\"REID\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"patient_first_name\":\"ALEX\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"service_lines\":[") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"match_method\":\"procedure_charge_date\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"line_paid_amount\":\"250.00\"") != NULL);
    REQUIRE_OK(scribe_store_find_claim_aggregate_ids_by_key(
                &read_store,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                aggregate_ids,
                4u,
                &aggregate_id_count
            ));
    REQUIRE(aggregate_id_count == 1u);
    REQUIRE_STR(aggregate_ids[0], "claim:8259c238232f9585e95fc8f45b0bb410");
    REQUIRE_OK(scribe_store_find_claim_aggregate_ids_by_key(
                &read_store,
                "claim_id_token",
                "8259c238232f9585e95fc8f45b0bb410",
                aggregate_ids,
                4u,
                &aggregate_id_count
            ));
    REQUIRE(aggregate_id_count == 0u);
    REQUIRE_OK(scribe_store_find_claim_aggregate_ids_by_key(
                &read_store,
                "claim_id_raw",
                "8259c238232f9585e95fc8f45b0bb410",
                aggregate_ids,
                4u,
                &aggregate_id_count
            ));
    REQUIRE(aggregate_id_count == 0u);
    REQUIRE_OK(scribe_store_find_claim_aggregate_ids_by_key(
                &read_store,
                "claim_id_raw",
                "CLM-STROKE-RAD-FAC-001",
                aggregate_ids,
                4u,
                &aggregate_id_count
            ));
    REQUIRE(aggregate_id_count == 1u);
    REQUIRE_STR(aggregate_ids[0], "claim:8259c238232f9585e95fc8f45b0bb410");
    REQUIRE_OK(scribe_store_find_claim_aggregate_ids_by_key(
                &read_store,
                "payer_claim_control_number",
                "edf29f09740ab104da309e2b036e14d1",
                aggregate_ids,
                4u,
                &aggregate_id_count
            ));
    REQUIRE(aggregate_id_count == 1u);
    REQUIRE_STR(aggregate_ids[0], "claim:8259c238232f9585e95fc8f45b0bb410");
    REQUIRE_OK(scribe_store_find_claim_aggregate_ids_by_key(
                &read_store,
                "payer_claim_control_number_raw",
                "edf29f09740ab104da309e2b036e14d1",
                aggregate_ids,
                4u,
                &aggregate_id_count
            ));
    REQUIRE(aggregate_id_count == 0u);
    REQUIRE_OK(scribe_store_find_claim_aggregate_ids_by_key(
                &read_store,
                "payer_claim_control_number_raw",
                "PAYER-STROKE-FAC-001",
                aggregate_ids,
                4u,
                &aggregate_id_count
            ));
    REQUIRE(aggregate_id_count == 1u);
    REQUIRE_STR(aggregate_ids[0], "claim:8259c238232f9585e95fc8f45b0bb410");
    REQUIRE_OK(scribe_store_close(&read_store));
    (void)remove(nonphi_read_store_path);
    (void)remove(nonphi_read_store_wal_path);
    (void)remove(nonphi_read_store_shm_path);
    (void)remove(resolved_phi_read_store_path);
    (void)remove(resolved_phi_read_store_wal_path);
    (void)remove(resolved_phi_read_store_shm_path);

    stitch_input.journal_path = nonphi_journal_path;
    stitch_input.out_path = nonphi_aggregate_path;
    stitch_input.read_store_path = nonphi_read_store_path;
    stitch_input.phi_vault_path = NULL;
    stitch_input.include_phi = 0;
    REQUIRE_OK(aggregate_stitcher_stitch(&stitch_input));
    REQUIRE(read_file_text(nonphi_aggregate_path, aggregates, AGGREGATES_LEN) == 0);

    REQUIRE(count_substring(aggregates, "\"event_type\":\"ClaimAggregateUpdated\"") == 8u);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(aggregates, "\"payer_claim_control_number\":\"edf29f09740ab104da309e2b036e14d1\"") != NULL);
    REQUIRE(strstr(aggregates, "\"service_lines\":[") != NULL);
    REQUIRE(strstr(aggregates, "\"procedure_code\":\"97110\"") != NULL);
    REQUIRE(strstr(aggregates, "\"match_method\":\"procedure_charge_date\"") != NULL);
    REQUIRE(strstr(aggregates, "CLM-STROKE") == NULL);
    REQUIRE(strstr(aggregates, "PAYER-STROKE") == NULL);
    REQUIRE(strstr(aggregates, "PAT-STROKE") == NULL);
    REQUIRE(strstr(aggregates, "SUB-STROKE") == NULL);
    REQUIRE(strstr(aggregates, "REID") == NULL);
    REQUIRE(strstr(aggregates, "ALEX") == NULL);

    scribe_store_init(&read_store);
    REQUIRE_OK(scribe_store_open(&read_store, nonphi_read_store_path));
    REQUIRE_OK(scribe_store_init_schema(&read_store));
    REQUIRE_OK(scribe_store_get_latest_claim_aggregate(
                &read_store,
                "claim:8259c238232f9585e95fc8f45b0bb410",
                &latest_version,
                latest_aggregate,
                LATEST_AGGREGATE_LEN
            ));
    REQUIRE(latest_version == 2u);
    REQUIRE(strstr(latest_aggregate, "\"contains_phi\":false") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"has_835\":true") != NULL);
    REQUIRE(strstr(latest_aggregate, "\"claim_id\":\"8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(latest_aggregate, "CLM-STROKE") == NULL);
    REQUIRE(strstr(latest_aggregate, "PAYER-STROKE") == NULL);
    REQUIRE(strstr(latest_aggregate, "REID") == NULL);
    REQUIRE(strstr(latest_aggregate, "ALEX") == NULL);
    REQUIRE_OK(scribe_store_find_event_ids_by_key(
                &read_store,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                indexed_event_ids,
                16u,
                &indexed_event_count
            ));
    REQUIRE(indexed_event_count > 0u);
    REQUIRE_OK(scribe_store_get_event_locator(
                &read_store,
                indexed_event_ids[0],
                &indexed_locator
            ));
    REQUIRE(indexed_locator.source_drop_id[0] != '\0');
    REQUIRE(indexed_locator.event_type[0] != '\0');
    REQUIRE_OK(scribe_store_find_event_ids_by_key(
                &read_store,
                "payer_claim_control_number",
                "edf29f09740ab104da309e2b036e14d1",
                indexed_event_ids,
                16u,
                &indexed_event_count
            ));
    REQUIRE(indexed_event_count > 0u);
    REQUIRE_OK(scribe_store_get_event_locator(
                &read_store,
                indexed_event_ids[0],
                &indexed_locator
            ));
    REQUIRE_STR(indexed_locator.source_drop_id, "835:000000102:102:0001");
    REQUIRE_OK(scribe_store_get_source_drop(
                &read_store,
                indexed_locator.source_drop_id,
                &indexed_source_drop
            ));
    REQUIRE_STR(indexed_source_drop.source_file, "stroke_encounter/facility_835.edi");
    REQUIRE_OK(scribe_store_close(&read_store));

    projection_input.read_store_path = nonphi_read_store_path;
    projection_input.include_phi = 0;
    REQUIRE_OK(balance_projector_project(&projection_input, nonphi_projection_path));
    REQUIRE(read_file_text(nonphi_projection_path, projection, PROJECTION_LEN) == 0);

    REQUIRE(strstr(projection, "\"claim_id\":\"8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(projection, "\"payer_claim_control_number\":\"edf29f09740ab104da309e2b036e14d1\"") != NULL);
    REQUIRE(strstr(projection, "CLM-STROKE") == NULL);
    REQUIRE(strstr(projection, "PAYER-STROKE") == NULL);
    REQUIRE(strstr(projection, "PAT-STROKE") == NULL);
    REQUIRE(strstr(projection, "SUB-STROKE") == NULL);
    REQUIRE(strstr(projection, "REID") == NULL);
    REQUIRE(strstr(projection, "ALEX") == NULL);
    (void)remove(nonphi_read_store_path);
    (void)remove(nonphi_read_store_wal_path);
    (void)remove(nonphi_read_store_shm_path);
    (void)remove(phi_vault_path);
    (void)remove(phi_vault_wal_path);
    (void)remove(phi_vault_shm_path);

    TEST_FREE_ALLOCATIONS();
    return 0;
}

static int test_member_coverage_aggregate_from_journal(void)
{
    char coverage_834_path[512];
    char eligibility_270_path[512];
    char eligibility_271_path[512];
    char journal_path[512];
    char aggregate_path[512];
    char phi_aggregate_path[512];
    char read_store_path[512];
    char read_store_wal_path[560];
    char read_store_shm_path[560];
    char phi_read_store_path[512];
    char phi_read_store_wal_path[560];
    char phi_read_store_shm_path[560];
    char phi_vault_path[512];
    char phi_vault_wal_path[560];
    char phi_vault_shm_path[560];
    enum {
        COVERAGE_AGGREGATES_LEN = 128000,
        COVERAGE_PHI_AGGREGATES_LEN = 128000,
        LATEST_COVERAGE_LEN = 65536
    };
    char *aggregates = NULL;
    char *phi_aggregates = NULL;
    char *latest_coverage = NULL;
    char aggregate_id[SCRIBE_STORE_ID_MAX];
    char member_token[TOKENISE_MAX_TOKEN_LEN];
    char payer_token[TOKENISE_MAX_TOKEN_LEN];
    char dob_token[TOKENISE_MAX_TOKEN_LEN];
    char aggregate_ids[4][SCRIBE_STORE_ID_MAX];
    x12_str_t raw;
    journal_builder_input_t journal_input;
    coverage_stitcher_input_t coverage_input;
    scribe_store_t store;
    size_t latest_version = 0u;
    size_t aggregate_count = 0u;

    REQUIRE_ALLOC(aggregates, COVERAGE_AGGREGATES_LEN);
    REQUIRE_ALLOC(phi_aggregates, COVERAGE_PHI_AGGREGATES_LEN);
    REQUIRE_ALLOC(latest_coverage, LATEST_COVERAGE_LEN);

    REQUIRE(make_path(coverage_834_path, sizeof(coverage_834_path), TEST_FIXTURE_DIR, "stroke_encounter/coverage_834.edi") == 0);
    REQUIRE(make_path(eligibility_270_path, sizeof(eligibility_270_path), TEST_FIXTURE_DIR, "stroke_encounter/eligibility_270.edi") == 0);
    REQUIRE(make_path(eligibility_271_path, sizeof(eligibility_271_path), TEST_FIXTURE_DIR, "stroke_encounter/eligibility_271.edi") == 0);
    REQUIRE(make_path(journal_path, sizeof(journal_path), TEST_OUTPUT_DIR, "member_coverage.journal") == 0);
    REQUIRE(make_path(aggregate_path, sizeof(aggregate_path), TEST_OUTPUT_DIR, "member_coverage.ndjson") == 0);
    REQUIRE(make_path(phi_aggregate_path, sizeof(phi_aggregate_path), TEST_OUTPUT_DIR, "member_coverage_phi.ndjson") == 0);
    REQUIRE(make_path(read_store_path, sizeof(read_store_path), TEST_OUTPUT_DIR, "member_coverage.sqlite") == 0);
    REQUIRE(make_path(phi_read_store_path, sizeof(phi_read_store_path), TEST_OUTPUT_DIR, "member_coverage_phi.sqlite") == 0);
    REQUIRE(make_path(phi_vault_path, sizeof(phi_vault_path), TEST_OUTPUT_DIR, "member_coverage_phi_vault.sqlite") == 0);
    REQUIRE(snprintf(read_store_wal_path, sizeof(read_store_wal_path), "%s-wal", read_store_path) > 0);
    REQUIRE(snprintf(read_store_shm_path, sizeof(read_store_shm_path), "%s-shm", read_store_path) > 0);
    REQUIRE(snprintf(phi_read_store_wal_path, sizeof(phi_read_store_wal_path), "%s-wal", phi_read_store_path) > 0);
    REQUIRE(snprintf(phi_read_store_shm_path, sizeof(phi_read_store_shm_path), "%s-shm", phi_read_store_path) > 0);
    REQUIRE(snprintf(phi_vault_wal_path, sizeof(phi_vault_wal_path), "%s-wal", phi_vault_path) > 0);
    REQUIRE(snprintf(phi_vault_shm_path, sizeof(phi_vault_shm_path), "%s-shm", phi_vault_path) > 0);

    raw.ptr = "SUB-STROKE-001";
    raw.len = strlen(raw.ptr);
    REQUIRE_OK(tokenise_value(TOK_MEMBER_ID, raw, member_token, sizeof(member_token)));
    raw.ptr = "PAYOR123";
    raw.len = strlen(raw.ptr);
    REQUIRE_OK(tokenise_value(TOK_PAYER_ID, raw, payer_token, sizeof(payer_token)));
    raw.ptr = "19790314";
    raw.len = strlen(raw.ptr);
    REQUIRE_OK(tokenise_value(TOK_MEMBER_DOB, raw, dob_token, sizeof(dob_token)));
    REQUIRE(snprintf(aggregate_id, sizeof(aggregate_id), "member_coverage:%s", member_token) > 0);

    (void)remove(journal_path);
    (void)remove(read_store_path);
    (void)remove(read_store_wal_path);
    (void)remove(read_store_shm_path);
    (void)remove(phi_read_store_path);
    (void)remove(phi_read_store_wal_path);
    (void)remove(phi_read_store_shm_path);
    (void)remove(phi_vault_path);
    (void)remove(phi_vault_wal_path);
    (void)remove(phi_vault_shm_path);

    journal_builder_input_init(&journal_input);
    journal_input.run_id = "coverage-ingest-test";
    journal_input.source_root = TEST_FIXTURE_DIR;
    journal_input.phi_vault_path = phi_vault_path;
    REQUIRE_OK(journal_builder_input_add_834(&journal_input, coverage_834_path));
    REQUIRE_OK(journal_builder_input_add_270(&journal_input, eligibility_270_path));
    REQUIRE_OK(journal_builder_input_add_271(&journal_input, eligibility_271_path));
    REQUIRE_OK(journal_builder_build(&journal_input, journal_path));

    coverage_stitcher_input_init(&coverage_input);
    coverage_input.journal_path = journal_path;
    coverage_input.out_path = aggregate_path;
    coverage_input.read_store_path = read_store_path;
    coverage_input.run_id = "coverage-stitch-test";
    REQUIRE_OK(coverage_stitcher_stitch(&coverage_input));
    REQUIRE(read_file_text(aggregate_path, aggregates, COVERAGE_AGGREGATES_LEN) == 0);

    REQUIRE(count_substring(aggregates, "\"event_type\":\"MemberCoverageUpdated\"") == 3u);
    REQUIRE(strstr(aggregates, "\"run_id\":\"coverage-stitch-test\"") != NULL);
    REQUIRE(strstr(aggregates, "\"source_run_id\":\"coverage-ingest-test\"") != NULL);
    REQUIRE(strstr(aggregates, "\"aggregate_type\":\"member_coverage\"") != NULL);
    REQUIRE(strstr(aggregates, aggregate_id) != NULL);
    REQUIRE(strstr(aggregates, "\"contains_phi\":false") != NULL);
    REQUIRE(strstr(aggregates, "\"member_id\":\"") != NULL);
    REQUIRE(strstr(aggregates, member_token) != NULL);
    REQUIRE(strstr(aggregates, payer_token) != NULL);
    REQUIRE(strstr(aggregates, dob_token) != NULL);
    REQUIRE(strstr(aggregates, "\"coverage_effective_date\":\"20260601\"") != NULL);
    REQUIRE(strstr(aggregates, "\"effective_date\":\"20260601\"") != NULL);
    REQUIRE(strstr(aggregates, "\"termination_date\":\"20261231\"") != NULL);
    REQUIRE(strstr(aggregates, "\"service_type_code\":\"30\"") != NULL);
    REQUIRE(strstr(aggregates, "\"service_type_code\":\"47\"") != NULL);
    REQUIRE(strstr(aggregates, "\"service_type_code\":\"98\"") != NULL);
    REQUIRE(strstr(aggregates, "SUB-STROKE-001") == NULL);
    REQUIRE(strstr(aggregates, "PAYOR123") == NULL);
    REQUIRE(strstr(aggregates, "19790314") == NULL);
    REQUIRE(strstr(aggregates, "REID") == NULL);
    REQUIRE(strstr(aggregates, "ALEX") == NULL);

    scribe_store_init(&store);
    REQUIRE_OK(scribe_store_open(&store, read_store_path));
    REQUIRE_OK(scribe_store_init_schema(&store));
    REQUIRE_OK(scribe_store_get_latest_member_coverage(
                &store,
                aggregate_id,
                &latest_version,
                latest_coverage,
                LATEST_COVERAGE_LEN
            ));
    REQUIRE(latest_version == 3u);
    REQUIRE(strstr(latest_coverage, "\"contains_phi\":false") != NULL);
    REQUIRE(strstr(latest_coverage, "\"benefit_count\":3") != NULL);
    REQUIRE_OK(scribe_store_find_member_coverage_ids_by_key(
                &store,
                "member_id",
                member_token,
                aggregate_ids,
                4u,
                &aggregate_count
            ));
    REQUIRE(aggregate_count == 1u);
    REQUIRE_STR(aggregate_ids[0], aggregate_id);
    REQUIRE_OK(scribe_store_find_member_coverage_ids_by_key(
                &store,
                "payer_id",
                payer_token,
                aggregate_ids,
                4u,
                &aggregate_count
            ));
    REQUIRE(aggregate_count == 1u);
    REQUIRE_STR(aggregate_ids[0], aggregate_id);
    REQUIRE_OK(scribe_store_find_member_coverage_ids_by_key(
                &store,
                "service_type_code",
                "47",
                aggregate_ids,
                4u,
                &aggregate_count
            ));
    REQUIRE(aggregate_count == 1u);
    REQUIRE_STR(aggregate_ids[0], aggregate_id);
    REQUIRE_OK(scribe_store_close(&store));

    coverage_stitcher_input_init(&coverage_input);
    coverage_input.journal_path = journal_path;
    coverage_input.out_path = phi_aggregate_path;
    coverage_input.read_store_path = phi_read_store_path;
    coverage_input.phi_vault_path = phi_vault_path;
    coverage_input.include_phi = 1;
    coverage_input.run_id = "coverage-phi-stitch-test";
    REQUIRE_OK(coverage_stitcher_stitch(&coverage_input));
    REQUIRE(read_file_text(phi_aggregate_path, phi_aggregates, COVERAGE_PHI_AGGREGATES_LEN) == 0);

    REQUIRE(count_substring(phi_aggregates, "\"event_type\":\"MemberCoverageUpdated\"") == 3u);
    REQUIRE(strstr(phi_aggregates, "\"contains_phi\":true") != NULL);
    REQUIRE(strstr(phi_aggregates, "\"member_id\":\"SUB-STROKE-001\"") != NULL);
    REQUIRE(strstr(phi_aggregates, "\"member_id_token\":\"") != NULL);
    REQUIRE(strstr(phi_aggregates, member_token) != NULL);
    REQUIRE(strstr(phi_aggregates, "\"payer_id\":\"PAYOR123\"") != NULL);
    REQUIRE(strstr(phi_aggregates, "\"payer_id_token\":\"") != NULL);
    REQUIRE(strstr(phi_aggregates, payer_token) != NULL);
    REQUIRE(strstr(phi_aggregates, "\"member_last_name_or_org\":\"REID\"") != NULL);
    REQUIRE(strstr(phi_aggregates, "\"member_first_name\":\"ALEX\"") != NULL);
    REQUIRE(strstr(phi_aggregates, "\"date_of_birth\":\"19790314\"") != NULL);
    REQUIRE(strstr(phi_aggregates, "\"date_of_birth_token\":\"") != NULL);
    REQUIRE(strstr(phi_aggregates, dob_token) != NULL);

    scribe_store_init(&store);
    REQUIRE_OK(scribe_store_open(&store, phi_read_store_path));
    REQUIRE_OK(scribe_store_init_schema(&store));
    REQUIRE_OK(scribe_store_get_latest_member_coverage(
                &store,
                aggregate_id,
                &latest_version,
                latest_coverage,
                LATEST_COVERAGE_LEN
            ));
    REQUIRE(latest_version == 3u);
    REQUIRE(strstr(latest_coverage, "\"contains_phi\":true") != NULL);
    REQUIRE(strstr(latest_coverage, "\"member_id\":\"SUB-STROKE-001\"") != NULL);
    REQUIRE_OK(scribe_store_find_member_coverage_ids_by_key(
                &store,
                "member_id_raw",
                "SUB-STROKE-001",
                aggregate_ids,
                4u,
                &aggregate_count
            ));
    REQUIRE(aggregate_count == 1u);
    REQUIRE_STR(aggregate_ids[0], aggregate_id);
    REQUIRE_OK(scribe_store_close(&store));

    (void)remove(journal_path);
    (void)remove(read_store_path);
    (void)remove(read_store_wal_path);
    (void)remove(read_store_shm_path);
    (void)remove(phi_read_store_path);
    (void)remove(phi_read_store_wal_path);
    (void)remove(phi_read_store_shm_path);
    (void)remove(phi_vault_path);
    (void)remove(phi_vault_wal_path);
    (void)remove(phi_vault_shm_path);

    TEST_FREE_ALLOCATIONS();
    return 0;
}

int main(void)
{
    REQUIRE(test_837_claim_event() == 0);
    REQUIRE(test_x12_005010x222_example_01_shape() == 0);
    REQUIRE(test_journal_builder_file_list() == 0);
    REQUIRE(test_journal_builder_compressed_zstd() == 0);
    REQUIRE(test_834_ins_event() == 0);
    REQUIRE(test_270_271_eligibility_events() == 0);
    REQUIRE(test_835_remittance_events() == 0);
    REQUIRE(test_stroke_encounter_fixture_set() == 0);
    REQUIRE(test_incremental_claim_stitch_from_source_drops() == 0);
    REQUIRE(test_incremental_coverage_stitch_from_source_drops() == 0);
    REQUIRE(test_837_claim_context_aggregate_state() == 0);
    REQUIRE(test_837_institutional_context_aggregate_state() == 0);
    REQUIRE(test_stroke_balance_projection_from_journal() == 0);
    REQUIRE(test_member_coverage_aggregate_from_journal() == 0);

    return 0;
}
