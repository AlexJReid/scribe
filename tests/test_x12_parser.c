#include "test_support.h"
#include "x12_reader.h"

#include <string.h>

typedef struct {
    size_t count;
    int saw_isa;
    int saw_clm;
} parser_seen_t;

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

static int test_delimiter_detection(void)
{
    char path[512];
    x12_document_t doc;
    int rc;

    REQUIRE(make_path(path, sizeof(path), TEST_FIXTURE_DIR, "sample_837.edi") == 0);

    rc = x12_document_load(path, &doc);
    REQUIRE_OK(rc);

    rc = x12_document_detect_delimiters(&doc);
    REQUIRE_OK(rc);
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
    REQUIRE_OK(rc);

    rc = x12_document_each_segment(&doc, parser_seen_cb, &seen);
    REQUIRE_OK(rc);
    REQUIRE(seen.count == 16u);
    REQUIRE(seen.saw_isa);
    REQUIRE(seen.saw_clm);

    x12_document_free(&doc);
    return 0;
}

int main(void)
{
    REQUIRE(test_delimiter_detection() == 0);
    REQUIRE(test_segment_and_element_splitting() == 0);
    return 0;
}
