#include "aggregate_stitcher.h"
#include "balance_projector.h"
#include "event_writer.h"
#include "journal_builder.h"
#include "phi_vault.h"
#include "store.h"
#include "tokenise.h"
#include "x12_mapper_834.h"
#include "x12_mapper_835.h"
#include "x12_mapper_837.h"
#include "x12_reader.h"

#include <stdio.h>
#include <stdlib.h>
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

typedef struct {
    size_t count;
    int saw_isa;
    int saw_clm;
} parser_seen_t;

static int make_path(char *out, size_t out_len, const char *base, const char *name)
{
    int written = snprintf(out, out_len, "%s/%s", base, name);

    if (written < 0 || (size_t)written >= out_len) {
        return 1;
    }

    return 0;
}

static int parser_seen_cb(const x12_segment_t *seg, void *user)
{
    parser_seen_t *seen = (parser_seen_t *)user;

    seen->count++;

    if (seg->segment_index == 1u) {
        REQUIRE(x12_str_eq_cstr(seg->tag, "ISA"));
        REQUIRE(seg->element_count == 16u);
        REQUIRE(x12_str_eq_cstr(seg->elements[12], "000000001"));
        seen->saw_isa = 1;
    }

    if (x12_str_eq_cstr(seg->tag, "CLM")) {
        REQUIRE(seg->element_count >= 2u);
        REQUIRE(x12_str_eq_cstr(seg->elements[0], "CLM123"));
        REQUIRE(x12_str_eq_cstr(seg->elements[1], "125.50"));
        seen->saw_clm = 1;
    }

    return X12_OK;
}

static int read_file_text(const char *path, char *out, size_t out_len)
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

static size_t count_substring(const char *text, const char *needle)
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
    REQUIRE(rc == X12_OK);
    rc = event_writer_open(&writer, out_path, input_path, transaction_type);
    REQUIRE(rc == X12_OK);

    if (strcmp(transaction_type, "837") == 0) {
        rc = x12_map_837_document(&doc, &writer);
    } else if (strcmp(transaction_type, "835") == 0) {
        rc = x12_map_835_document(&doc, &writer);
    } else {
        rc = X12_ERR_INVALID_ARGUMENT;
    }

    REQUIRE(rc == X12_OK);
    rc = event_writer_close(&writer);
    REQUIRE(rc == X12_OK);
    x12_document_free(&doc);

    REQUIRE(read_file_text(out_path, output, output_len) == 0);
    return 0;
}

static int test_delimiter_detection(void)
{
    char path[512];
    x12_document_t doc;
    int rc;

    REQUIRE(make_path(path, sizeof(path), TEST_FIXTURE_DIR, "sample_837.edi") == 0);

    rc = x12_document_load(path, &doc);
    REQUIRE(rc == X12_OK);

    rc = x12_document_detect_delimiters(&doc);
    REQUIRE(rc == X12_OK);
    REQUIRE(doc.delimiters.element_sep == '*');
    REQUIRE(doc.delimiters.component_sep == ':');
    REQUIRE(doc.delimiters.segment_term == '~');

    x12_document_free(&doc);
    return 0;
}

static int test_segment_and_element_splitting(void)
{
    char path[512];
    x12_document_t doc;
    parser_seen_t seen;
    int rc;

    memset(&seen, 0, sizeof(seen));
    REQUIRE(make_path(path, sizeof(path), TEST_FIXTURE_DIR, "sample_837.edi") == 0);

    rc = x12_document_load(path, &doc);
    REQUIRE(rc == X12_OK);

    rc = x12_document_each_segment(&doc, parser_seen_cb, &seen);
    REQUIRE(rc == X12_OK);
    REQUIRE(seen.count == 16u);
    REQUIRE(seen.saw_isa);
    REQUIRE(seen.saw_clm);

    x12_document_free(&doc);
    return 0;
}

static int test_837_claim_event(void)
{
    char input_path[512];
    char out_path[512];
    char phi_out_path[512];
    char output[8192];
    char phi_output[8192];
    x12_document_t doc;
    event_writer_t writer;
    int rc;

    REQUIRE(make_path(input_path, sizeof(input_path), TEST_FIXTURE_DIR, "sample_837.edi") == 0);
    REQUIRE(make_path(out_path, sizeof(out_path), TEST_OUTPUT_DIR, "sample_837.ndjson") == 0);
    REQUIRE(make_path(phi_out_path, sizeof(phi_out_path), TEST_OUTPUT_DIR, "sample_837_phi.ndjson") == 0);

    rc = x12_document_load(input_path, &doc);
    REQUIRE(rc == X12_OK);
    rc = event_writer_open(&writer, out_path, input_path, "837");
    REQUIRE(rc == X12_OK);
    rc = x12_map_837_document(&doc, &writer);
    REQUIRE(rc == X12_OK);
    rc = event_writer_close(&writer);
    REQUIRE(rc == X12_OK);
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
    REQUIRE(rc == X12_OK);
    rc = event_writer_open(&writer, phi_out_path, input_path, "837");
    REQUIRE(rc == X12_OK);
    event_writer_set_include_phi(&writer, 1);
    rc = x12_map_837_document(&doc, &writer);
    REQUIRE(rc == X12_OK);
    rc = event_writer_close(&writer);
    REQUIRE(rc == X12_OK);
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
    REQUIRE(rc == X12_OK);
    rc = event_writer_open(&writer, out_path, input_path, "834");
    REQUIRE(rc == X12_OK);
    rc = x12_map_834_document(&doc, &writer);
    REQUIRE(rc == X12_OK);
    rc = event_writer_close(&writer);
    REQUIRE(rc == X12_OK);
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
    REQUIRE(rc == X12_OK);
    rc = event_writer_open(&writer, phi_out_path, input_path, "834");
    REQUIRE(rc == X12_OK);
    event_writer_set_include_phi(&writer, 1);
    rc = x12_map_834_document(&doc, &writer);
    REQUIRE(rc == X12_OK);
    rc = event_writer_close(&writer);
    REQUIRE(rc == X12_OK);
    x12_document_free(&doc);

    REQUIRE(read_file_text(phi_out_path, phi_output, sizeof(phi_output)) == 0);
    REQUIRE(strstr(phi_output, "\"last_name_or_org\":\"DOE\"") != NULL);
    REQUIRE(strstr(phi_output, "\"first_name\":\"JOHN\"") != NULL);
    REQUIRE(strstr(phi_output, "\"id_value\":\"MEM12345\"") != NULL);
    REQUIRE(strstr(phi_output, "\"id_value_token\":\"1a1dbacf37d1e1998645cf82e8fccc15\"") != NULL);
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
    REQUIRE(tokenise_value(
                TOK_PAYER_CLAIM_CONTROL_NUMBER,
                payer_claim_control_raw,
                payer_claim_control_token,
                sizeof(payer_claim_control_token)
            ) == X12_OK);
    REQUIRE(snprintf(
                payer_claim_control_snippet,
                sizeof(payer_claim_control_snippet),
                "\"payer_claim_control_number\":\"%s\"",
                payer_claim_control_token
            ) > 0);

    rc = x12_document_load(input_path, &doc);
    REQUIRE(rc == X12_OK);
    rc = event_writer_open(&writer, out_path, input_path, "835");
    REQUIRE(rc == X12_OK);
    rc = x12_map_835_document(&doc, &writer);
    REQUIRE(rc == X12_OK);
    rc = event_writer_close(&writer);
    REQUIRE(rc == X12_OK);
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
    REQUIRE(rc == X12_OK);
    rc = event_writer_open(&writer, phi_out_path, input_path, "835");
    REQUIRE(rc == X12_OK);
    event_writer_set_include_phi(&writer, 1);
    rc = x12_map_835_document(&doc, &writer);
    REQUIRE(rc == X12_OK);
    rc = event_writer_close(&writer);
    REQUIRE(rc == X12_OK);
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
    char facility_837_output[20000];
    char facility_835_output[24000];
    char professional_837_output[20000];
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

static int test_stroke_balance_projection_from_journal(void)
{
    char charges_path[512];
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
    char projection_path[512];
    char nonphi_projection_path[512];
    char phi_vault_path[512];
    char phi_vault_wal_path[560];
    char phi_vault_shm_path[560];
    char aggregates[160000];
    char projection[96000];
    char resolved[256];
    x12_str_t conflicting_raw;
    journal_builder_input_t journal_input;
    aggregate_stitcher_input_t stitch_input;
    balance_projector_input_t projection_input;
    phi_vault_t phi_vault;

    REQUIRE(make_path(charges_path, sizeof(charges_path), TEST_FIXTURE_DIR, "stroke_encounter/charge_transactions.ndjson") == 0);
    REQUIRE(make_path(facility_837_path, sizeof(facility_837_path), TEST_FIXTURE_DIR, "stroke_encounter/facility_837.edi") == 0);
    REQUIRE(make_path(facility_835_path, sizeof(facility_835_path), TEST_FIXTURE_DIR, "stroke_encounter/facility_835.edi") == 0);
    REQUIRE(make_path(professional_837_path, sizeof(professional_837_path), TEST_FIXTURE_DIR, "stroke_encounter/professional_837.edi") == 0);
    REQUIRE(make_path(professional_835_path, sizeof(professional_835_path), TEST_FIXTURE_DIR, "stroke_encounter/professional_835.edi") == 0);
    REQUIRE(make_path(rehab_837_path, sizeof(rehab_837_path), TEST_FIXTURE_DIR, "stroke_encounter/rehab_837.edi") == 0);
    REQUIRE(make_path(rehab_835_path, sizeof(rehab_835_path), TEST_FIXTURE_DIR, "stroke_encounter/rehab_835.edi") == 0);
    REQUIRE(make_path(neurology_837_path, sizeof(neurology_837_path), TEST_FIXTURE_DIR, "stroke_encounter/neurology_837.edi") == 0);
    REQUIRE(make_path(neurology_835_path, sizeof(neurology_835_path), TEST_FIXTURE_DIR, "stroke_encounter/neurology_835.edi") == 0);
    REQUIRE(make_path(journal_path, sizeof(journal_path), TEST_OUTPUT_DIR, "stroke_encounter.journal.ndjson") == 0);
    REQUIRE(make_path(nonphi_journal_path, sizeof(nonphi_journal_path), TEST_OUTPUT_DIR, "stroke_encounter_nonphi.journal.ndjson") == 0);
    REQUIRE(make_path(aggregate_path, sizeof(aggregate_path), TEST_OUTPUT_DIR, "stroke_claim_aggregates.ndjson") == 0);
    REQUIRE(make_path(nonphi_aggregate_path, sizeof(nonphi_aggregate_path), TEST_OUTPUT_DIR, "stroke_claim_aggregates_nonphi.ndjson") == 0);
    REQUIRE(make_path(projection_path, sizeof(projection_path), TEST_OUTPUT_DIR, "stroke_balance_projection.json") == 0);
    REQUIRE(make_path(nonphi_projection_path, sizeof(nonphi_projection_path), TEST_OUTPUT_DIR, "stroke_balance_projection_nonphi.json") == 0);
    REQUIRE(make_path(phi_vault_path, sizeof(phi_vault_path), TEST_OUTPUT_DIR, "stroke_phi_vault.sqlite") == 0);
    REQUIRE(snprintf(phi_vault_wal_path, sizeof(phi_vault_wal_path), "%s-wal", phi_vault_path) > 0);
    REQUIRE(snprintf(phi_vault_shm_path, sizeof(phi_vault_shm_path), "%s-shm", phi_vault_path) > 0);

    journal_builder_input_init(&journal_input);
    journal_input.include_phi = 1;
    REQUIRE(journal_builder_input_add_charges(&journal_input, charges_path) == X12_OK);
    REQUIRE(journal_builder_input_add_837(&journal_input, facility_837_path) == X12_OK);
    REQUIRE(journal_builder_input_add_837(&journal_input, professional_837_path) == X12_OK);
    REQUIRE(journal_builder_input_add_837(&journal_input, rehab_837_path) == X12_OK);
    REQUIRE(journal_builder_input_add_837(&journal_input, neurology_837_path) == X12_OK);
    REQUIRE(journal_builder_input_add_835(&journal_input, facility_835_path) == X12_OK);
    REQUIRE(journal_builder_input_add_835(&journal_input, professional_835_path) == X12_OK);
    REQUIRE(journal_builder_input_add_835(&journal_input, rehab_835_path) == X12_OK);
    REQUIRE(journal_builder_input_add_835(&journal_input, neurology_835_path) == X12_OK);
    REQUIRE(journal_builder_build(&journal_input, journal_path) == X12_OK);

    aggregate_stitcher_input_init(&stitch_input);
    stitch_input.journal_path = journal_path;
    stitch_input.out_path = aggregate_path;
    stitch_input.encounter_id = "ENC-SYN-STROKE-001";
    stitch_input.include_phi = 1;
    REQUIRE(aggregate_stitcher_stitch(&stitch_input) == X12_OK);
    REQUIRE(read_file_text(aggregate_path, aggregates, sizeof(aggregates)) == 0);

    REQUIRE(strstr(aggregates, "\"event_type\":\"ClaimAggregateUpdated\"") != NULL);
    REQUIRE(count_substring(aggregates, "\"event_type\":\"ClaimAggregateUpdated\"") == 12u);
    REQUIRE(strstr(aggregates, "\"aggregate_type\":\"claim\"") != NULL);
    REQUIRE(strstr(aggregates, "\"aggregate_id\":\"claim:8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"CLM-STROKE-RAD-FAC-001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"CLM-STROKE-RAD-PRO-001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"CLM-STROKE-REHAB-001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"CLM-STROKE-NEURO-001\"") != NULL);
    REQUIRE(strstr(aggregates, "\"version\":1") != NULL);
    REQUIRE(strstr(aggregates, "\"version\":3") != NULL);
    REQUIRE(strstr(aggregates, "\"version\":4") == NULL);
    REQUIRE(strstr(aggregates, "\"updated_by_event_id\"") != NULL);
    REQUIRE(strstr(aggregates, "\"update_scope\":\"source_drop\"") != NULL);
    REQUIRE(strstr(aggregates, "\"source_drop_id\":\"charge:1\"") != NULL);
    REQUIRE(strstr(aggregates, "\"source_drop_id\":\"837:2\"") != NULL);
    REQUIRE(strstr(aggregates, "\"source_drop_id\":\"835:6\"") != NULL);
    REQUIRE(strstr(aggregates, "\"compacted_source_event_count\":12") != NULL);
    REQUIRE(strstr(aggregates, "\"compacted_source_event_count\":14") != NULL);
    REQUIRE(strstr(aggregates, "\"applied_event_ids\":[") != NULL);
    REQUIRE(strstr(aggregates, "\"update_event_ids\":[") != NULL);
    REQUIRE(strstr(aggregates, "\"source_event_ids\":[") == NULL);
    REQUIRE(strstr(aggregates, "\"updated_source_event_ids\":[") == NULL);
    REQUIRE(strstr(aggregates, "\"has_charge_context\":true") != NULL);
    REQUIRE(strstr(aggregates, "\"has_837\":true") != NULL);
    REQUIRE(strstr(aggregates, "\"has_835\":true") != NULL);
    REQUIRE(strstr(aggregates, "\"submitted_service_line_count\":3") != NULL);
    REQUIRE(strstr(aggregates, "\"remittance_service_line_count\":3") != NULL);
    REQUIRE(strstr(aggregates, "\"adjustment_count\":6") != NULL);
    REQUIRE(strstr(aggregates, "\"payer_claim_control_number\":\"PAYER-STROKE-FAC-001\"") != NULL);

    balance_projector_input_init(&projection_input);
    projection_input.journal_path = journal_path;
    projection_input.encounter_id = "ENC-SYN-STROKE-001";
    projection_input.include_phi = 1;
    REQUIRE(balance_projector_project(&projection_input, projection_path) == X12_OK);
    REQUIRE(read_file_text(projection_path, projection, sizeof(projection)) == 0);

    REQUIRE(strstr(projection, "\"event_type\":\"EncounterBalanceProjected\"") != NULL);
    REQUIRE(strstr(projection, "\"encounter_id\":\"ENC-SYN-STROKE-001\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_id\":\"CLM-STROKE-RAD-FAC-001\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_type\":\"radiology_facility\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_id\":\"CLM-STROKE-RAD-PRO-001\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_type\":\"radiology_professional\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_id\":\"CLM-STROKE-REHAB-001\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_type\":\"outpatient_rehab\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_id\":\"CLM-STROKE-NEURO-001\"") != NULL);
    REQUIRE(strstr(projection, "\"claim_type\":\"neurology_followup\"") != NULL);
    REQUIRE(strstr(projection, "\"description\":\"CT head without contrast facility charge\"") != NULL);
    REQUIRE(strstr(projection, "\"description\":\"CT head with contrast facility charge\"") != NULL);
    REQUIRE(strstr(projection, "\"description\":\"MRI brain professional interpretation\"") != NULL);
    REQUIRE(strstr(projection, "\"description\":\"Therapeutic exercise for stroke recovery\"") != NULL);
    REQUIRE(strstr(projection, "\"description\":\"Neurology follow-up for stroke recovery\"") != NULL);
    REQUIRE(strstr(projection, "\"match_method\":\"procedure_charge_date\"") != NULL);
    REQUIRE(strstr(projection, "\"entry_type\":\"billed_charge\"") != NULL);
    REQUIRE(strstr(projection, "\"entry_type\":\"payer_payment\"") != NULL);
    REQUIRE(strstr(projection, "\"entry_type\":\"contractual_adjustment\"") != NULL);
    REQUIRE(strstr(projection, "\"entry_type\":\"patient_responsibility\"") != NULL);
    REQUIRE(strstr(projection, "\"total_billed\":\"3720.00\"") != NULL);
    REQUIRE(strstr(projection, "\"payer_paid\":\"2340.00\"") != NULL);
    REQUIRE(strstr(projection, "\"contractual_adjustments\":\"830.00\"") != NULL);
    REQUIRE(strstr(projection, "\"patient_responsibility\":\"550.00\"") != NULL);
    REQUIRE(strstr(projection, "\"patient_payments\":\"0.00\"") != NULL);
    REQUIRE(strstr(projection, "\"current_balance\":\"550.00\"") != NULL);

    (void)remove(phi_vault_path);
    (void)remove(phi_vault_wal_path);
    (void)remove(phi_vault_shm_path);

    journal_input.include_phi = 0;
    journal_input.phi_vault_path = phi_vault_path;
    REQUIRE(journal_builder_build(&journal_input, nonphi_journal_path) == X12_OK);

    phi_vault_init(&phi_vault);
    REQUIRE(phi_vault_open(&phi_vault, phi_vault_path) == X12_OK);
    REQUIRE(phi_vault_init_schema(&phi_vault) == X12_OK);
    REQUIRE(phi_vault_resolve(
                &phi_vault,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                "test",
                "unit",
                resolved,
                sizeof(resolved)
            ) == X12_OK);
    REQUIRE(strcmp(resolved, "CLM-STROKE-RAD-FAC-001") == 0);
    REQUIRE(phi_vault_resolve(
                &phi_vault,
                "payer_claim_control_number",
                "edf29f09740ab104da309e2b036e14d1",
                "test",
                "unit",
                resolved,
                sizeof(resolved)
            ) == X12_OK);
    REQUIRE(strcmp(resolved, "PAYER-STROKE-FAC-001") == 0);
    conflicting_raw.ptr = "DIFFERENT-RAW-CLAIM";
    conflicting_raw.len = strlen(conflicting_raw.ptr);
    REQUIRE(phi_vault_put_mapping(
                &phi_vault,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                conflicting_raw,
                "test-conflict"
            ) == X12_ERR_CONFLICT);
    REQUIRE(phi_vault_close(&phi_vault) == X12_OK);
    (void)remove(phi_vault_path);
    (void)remove(phi_vault_wal_path);
    (void)remove(phi_vault_shm_path);

    stitch_input.journal_path = nonphi_journal_path;
    stitch_input.out_path = nonphi_aggregate_path;
    stitch_input.include_phi = 0;
    REQUIRE(aggregate_stitcher_stitch(&stitch_input) == X12_OK);
    REQUIRE(read_file_text(nonphi_aggregate_path, aggregates, sizeof(aggregates)) == 0);

    REQUIRE(count_substring(aggregates, "\"event_type\":\"ClaimAggregateUpdated\"") == 12u);
    REQUIRE(strstr(aggregates, "\"claim_id\":\"8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(aggregates, "\"payer_claim_control_number\":\"edf29f09740ab104da309e2b036e14d1\"") != NULL);
    REQUIRE(strstr(aggregates, "CLM-STROKE") == NULL);
    REQUIRE(strstr(aggregates, "PAYER-STROKE") == NULL);
    REQUIRE(strstr(aggregates, "PAT-STROKE") == NULL);
    REQUIRE(strstr(aggregates, "SUB-STROKE") == NULL);
    REQUIRE(strstr(aggregates, "REID") == NULL);
    REQUIRE(strstr(aggregates, "ALEX") == NULL);

    projection_input.journal_path = nonphi_journal_path;
    projection_input.include_phi = 0;
    REQUIRE(balance_projector_project(&projection_input, nonphi_projection_path) == X12_OK);
    REQUIRE(read_file_text(nonphi_projection_path, projection, sizeof(projection)) == 0);

    REQUIRE(strstr(projection, "\"claim_id\":\"8259c238232f9585e95fc8f45b0bb410\"") != NULL);
    REQUIRE(strstr(projection, "\"payer_claim_control_number\":\"edf29f09740ab104da309e2b036e14d1\"") != NULL);
    REQUIRE(strstr(projection, "CLM-STROKE") == NULL);
    REQUIRE(strstr(projection, "PAYER-STROKE") == NULL);
    REQUIRE(strstr(projection, "PAT-STROKE") == NULL);
    REQUIRE(strstr(projection, "SUB-STROKE") == NULL);
    REQUIRE(strstr(projection, "REID") == NULL);
    REQUIRE(strstr(projection, "ALEX") == NULL);

    return 0;
}

static int test_store_indexes_and_aggregates(void)
{
    char db_path[512];
    char db_wal_path[560];
    char db_shm_path[560];
    char event_ids[4][SCRIBE_STORE_ID_MAX];
    char state_json[512];
    scribe_store_t store;
    scribe_event_locator_t locator;
    size_t count = 0u;
    size_t version = 0u;

    REQUIRE(make_path(db_path, sizeof(db_path), TEST_OUTPUT_DIR, "scribe_store.sqlite") == 0);
    REQUIRE(snprintf(db_wal_path, sizeof(db_wal_path), "%s-wal", db_path) > 0);
    REQUIRE(snprintf(db_shm_path, sizeof(db_shm_path), "%s-shm", db_path) > 0);
    (void)remove(db_path);
    (void)remove(db_wal_path);
    (void)remove(db_shm_path);

    scribe_store_init(&store);
    REQUIRE(scribe_store_open(&store, db_path) == X12_OK);
    REQUIRE(scribe_store_init_schema(&store) == X12_OK);
    REQUIRE(scribe_store_put_source_drop(
                &store,
                "835:6",
                "835",
                "2026-09-14T00:00:00Z",
                "sha256:file"
            ) == X12_OK);
    REQUIRE(scribe_store_put_event(
                &store,
                "evt_000128",
                "835:6",
                "RemittanceAdjustmentObserved",
                "20260603-000001",
                8172,
                612,
                "sha256:event"
            ) == X12_OK);
    REQUIRE(scribe_store_put_event_key(
                &store,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                "evt_000128"
            ) == X12_OK);
    REQUIRE(scribe_store_put_event_key(
                &store,
                "payer_claim_control_number",
                "edf29f09740ab104da309e2b036e14d1",
                "evt_000128"
            ) == X12_OK);

    REQUIRE(scribe_store_find_event_ids_by_key(
                &store,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                event_ids,
                4u,
                &count
            ) == X12_OK);
    REQUIRE(count == 1u);
    REQUIRE(strcmp(event_ids[0], "evt_000128") == 0);

    REQUIRE(scribe_store_get_event_locator(&store, "evt_000128", &locator) == X12_OK);
    REQUIRE(strcmp(locator.source_drop_id, "835:6") == 0);
    REQUIRE(strcmp(locator.event_type, "RemittanceAdjustmentObserved") == 0);
    REQUIRE(strcmp(locator.segment_id, "20260603-000001") == 0);
    REQUIRE(locator.offset == 8172);
    REQUIRE(locator.length == 612);
    REQUIRE(strcmp(locator.checksum, "sha256:event") == 0);

    REQUIRE(scribe_store_put_claim_aggregate(
                &store,
                "claim:8259c238232f9585e95fc8f45b0bb410",
                3u,
                "{\"version\":3,\"has_835\":true}",
                "evt_000128",
                "835:6"
            ) == X12_OK);
    REQUIRE(scribe_store_put_claim_aggregate(
                &store,
                "claim:8259c238232f9585e95fc8f45b0bb410",
                2u,
                "{\"version\":2}",
                "evt_000024",
                "837:2"
            ) == X12_OK);
    REQUIRE(scribe_store_get_latest_claim_aggregate(
                &store,
                "claim:8259c238232f9585e95fc8f45b0bb410",
                &version,
                state_json,
                sizeof(state_json)
            ) == X12_OK);
    REQUIRE(version == 3u);
    REQUIRE(strstr(state_json, "\"version\":3") != NULL);

    REQUIRE(scribe_store_get_event_locator(&store, "evt_missing", &locator) == X12_ERR_NOT_FOUND);
    REQUIRE(scribe_store_close(&store) == X12_OK);
    (void)remove(db_path);
    (void)remove(db_wal_path);
    (void)remove(db_shm_path);

    return 0;
}

static int test_json_escaping(void)
{
    FILE *fp;
    char output[128];
    const char input[] = "a\"b\\c\n";

    fp = tmpfile();
    REQUIRE(fp != NULL);
    REQUIRE(event_writer_write_json_string(fp, input, strlen(input)) == X12_OK);
    REQUIRE(fflush(fp) == 0);
    REQUIRE(fseek(fp, 0, SEEK_SET) == 0);
    REQUIRE(fgets(output, sizeof(output), fp) != NULL);
    REQUIRE(strcmp(output, "\"a\\\"b\\\\c\\n\"") == 0);
    REQUIRE(fclose(fp) == 0);

    return 0;
}

static int test_tokenise_format(void)
{
    char token[TOKENISE_MAX_TOKEN_LEN];
    x12_str_t raw;

    raw.ptr = "ABC123";
    raw.len = strlen(raw.ptr);

    REQUIRE(tokenise_value(TOK_CLAIM_ID, raw, token, sizeof(token)) == X12_OK);
    REQUIRE(strcmp(token, "f58260c3ffcdfaff81c42473f162e481") == 0);

    return 0;
}

int main(void)
{
    REQUIRE(test_delimiter_detection() == 0);
    REQUIRE(test_segment_and_element_splitting() == 0);
    REQUIRE(test_837_claim_event() == 0);
    REQUIRE(test_834_ins_event() == 0);
    REQUIRE(test_835_remittance_events() == 0);
    REQUIRE(test_stroke_encounter_fixture_set() == 0);
    REQUIRE(test_stroke_balance_projection_from_journal() == 0);
    REQUIRE(test_store_indexes_and_aggregates() == 0);
    REQUIRE(test_json_escaping() == 0);
    REQUIRE(test_tokenise_format() == 0);

    return 0;
}
