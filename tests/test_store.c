#include "delta_exporter.h"
#include "store.h"
#include "test_support.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t count;
    char aggregate_id[SCRIBE_STORE_ID_MAX];
    size_t version;
    char updated_by_event_id[SCRIBE_STORE_ID_MAX];
    char source_drop_id[SCRIBE_STORE_ID_MAX];
} test_aggregate_version_seen_t;

typedef struct {
    size_t count;
    scribe_outbox_notification_t first;
} test_outbox_seen_t;

static int collect_aggregate_version(
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    const char *updated_by_event_id,
    const char *source_drop_id,
    void *user
)
{
    test_aggregate_version_seen_t *seen = (test_aggregate_version_seen_t *)user;

    (void)state_json;
    if (seen == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (seen->count == 0u) {
        snprintf(seen->aggregate_id, sizeof(seen->aggregate_id), "%s", aggregate_id);
        seen->version = version;
        snprintf(
            seen->updated_by_event_id,
            sizeof(seen->updated_by_event_id),
            "%s",
            updated_by_event_id
        );
        snprintf(seen->source_drop_id, sizeof(seen->source_drop_id), "%s", source_drop_id);
    }
    seen->count++;
    return X12_OK;
}

static int collect_outbox_notification(
    const scribe_outbox_notification_t *notification,
    void *user
)
{
    test_outbox_seen_t *seen = (test_outbox_seen_t *)user;

    if (seen == NULL || notification == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (seen->count == 0u) {
        seen->first = *notification;
    }
    seen->count++;
    return X12_OK;
}

static int query_count(const char *db_path, const char *sql, long long *out)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int step_rc;
    int ok = 0;

    if (db_path == NULL || sql == NULL || out == NULL) {
        return 1;
    }
    *out = 0;

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db != NULL) {
            (void)sqlite3_close(db);
        }
        return 1;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        (void)sqlite3_close(db);
        return 1;
    }

    step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW) {
        *out = (long long)sqlite3_column_int64(stmt, 0);
        ok = 1;
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK) {
        ok = 0;
    }
    if (sqlite3_close(db) != SQLITE_OK) {
        ok = 0;
    }

    return ok ? 0 : 1;
}

static int file_exists(const char *path)
{
    FILE *fp;

    if (path == NULL) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    (void)fclose(fp);
    return 1;
}

static int test_store_indexes_and_aggregates(void)
{
    char db_path[512];
    char db_wal_path[560];
    char db_shm_path[560];
    char delta_path[512];
    char delta_wal_path[560];
    char delta_shm_path[560];
    char delta_journal_path[560];
    char event_ids[4][SCRIBE_STORE_ID_MAX];
    char aggregate_ids[4][SCRIBE_STORE_ID_MAX];
    char state_json[512];
    scribe_store_t store;
    scribe_source_drop_t source_drop;
    scribe_event_locator_t locator;
    scribe_delta_exporter_input_t delta_input;
    test_aggregate_version_seen_t seen_versions;
    test_outbox_seen_t seen_outbox;
    long long outbox_sequence = 0;
    long long delta_count = 0;
    size_t count = 0u;
    size_t version = 0u;

    REQUIRE(make_path(db_path, sizeof(db_path), TEST_OUTPUT_DIR, "scribe_store.sqlite") == 0);
    REQUIRE(make_path(delta_path, sizeof(delta_path), TEST_OUTPUT_DIR, "scribe_delta.sqlite") == 0);
    REQUIRE(snprintf(db_wal_path, sizeof(db_wal_path), "%s-wal", db_path) > 0);
    REQUIRE(snprintf(db_shm_path, sizeof(db_shm_path), "%s-shm", db_path) > 0);
    REQUIRE(snprintf(delta_wal_path, sizeof(delta_wal_path), "%s-wal", delta_path) > 0);
    REQUIRE(snprintf(delta_shm_path, sizeof(delta_shm_path), "%s-shm", delta_path) > 0);
    REQUIRE(snprintf(delta_journal_path, sizeof(delta_journal_path), "%s-journal", delta_path) > 0);
    (void)remove(db_path);
    (void)remove(db_wal_path);
    (void)remove(db_shm_path);
    (void)remove(delta_path);
    (void)remove(delta_wal_path);
    (void)remove(delta_shm_path);
    (void)remove(delta_journal_path);

    scribe_store_init(&store);
    REQUIRE_OK(scribe_store_open(&store, db_path));
    REQUIRE_OK(scribe_store_init_schema(&store));
    REQUIRE_OK(scribe_store_begin_immediate(&store));
    REQUIRE_OK(scribe_store_put_source_drop(
                &store,
                "837:rollback",
                "837",
                "/inbound/claims/rollback.edi",
                "2026-09-13T00:00:00Z",
                "sha256:rollback"
            ));
    REQUIRE_OK(scribe_store_rollback(&store));
    REQUIRE(scribe_store_get_source_drop(&store, "837:rollback", &source_drop) == X12_ERR_NOT_FOUND);

    REQUIRE_OK(scribe_store_begin_immediate(&store));
    REQUIRE_OK(scribe_store_put_source_drop(
                &store,
                "835:000000102:102:0001",
                "835",
                "/inbound/remits/facility_835.edi",
                "2026-09-14T00:00:00Z",
                "sha256:file"
            ));
    REQUIRE_OK(scribe_store_commit(&store));
    REQUIRE_OK(scribe_store_get_source_drop(
                &store,
                "835:000000102:102:0001",
                &source_drop
            ));
    REQUIRE_STR(source_drop.source_drop_id, "835:000000102:102:0001");
    REQUIRE_STR(source_drop.source_type, "835");
    REQUIRE_STR(source_drop.source_file, "/inbound/remits/facility_835.edi");
    REQUIRE_STR(source_drop.received_at, "2026-09-14T00:00:00Z");
    REQUIRE_STR(source_drop.file_hash, "sha256:file");
    REQUIRE_OK(scribe_store_put_event(
                &store,
                "evt_000128",
                "835:000000102:102:0001",
                "RemittanceAdjustmentObserved",
                "20260603-000001",
                8172,
                612,
                "sha256:event"
            ));
    REQUIRE_OK(scribe_store_put_event_key(
                &store,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                "evt_000128"
            ));
    REQUIRE_OK(scribe_store_put_event_key(
                &store,
                "payer_claim_control_number",
                "edf29f09740ab104da309e2b036e14d1",
                "evt_000128"
            ));

    REQUIRE_OK(scribe_store_find_event_ids_by_key(
                &store,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                event_ids,
                4u,
                &count
            ));
    REQUIRE(count == 1u);
    REQUIRE_STR(event_ids[0], "evt_000128");

    REQUIRE_OK(scribe_store_get_event_locator(&store, "evt_000128", &locator));
    REQUIRE_STR(locator.source_drop_id, "835:000000102:102:0001");
    REQUIRE_STR(locator.event_type, "RemittanceAdjustmentObserved");
    REQUIRE_STR(locator.segment_id, "20260603-000001");
    REQUIRE(locator.offset == 8172);
    REQUIRE(locator.length == 612);
    REQUIRE_STR(locator.checksum, "sha256:event");

    REQUIRE_OK(scribe_store_put_claim_aggregate(
                &store,
                "claim:8259c238232f9585e95fc8f45b0bb410",
                3u,
                "{\"version\":3,\"has_835\":true}",
                "evt_000128",
                "835:000000102:102:0001"
            ));
    REQUIRE_OK(scribe_store_put_claim_aggregate(
                &store,
                "claim:8259c238232f9585e95fc8f45b0bb410",
                2u,
                "{\"version\":2}",
                "evt_000024",
                "837:000000101:101:0001"
            ));
    REQUIRE_OK(scribe_store_get_latest_claim_aggregate(
                &store,
                "claim:8259c238232f9585e95fc8f45b0bb410",
                &version,
                state_json,
                sizeof(state_json)
            ));
    REQUIRE(version == 3u);
    REQUIRE(strstr(state_json, "\"version\":3") != NULL);
    memset(&seen_versions, 0, sizeof(seen_versions));
    REQUIRE_OK(scribe_store_each_claim_aggregate_version_for_source_drop(
                &store,
                "835:000000102:102:0001",
                collect_aggregate_version,
                &seen_versions
            ));
    REQUIRE(seen_versions.count == 1u);
    REQUIRE_STR(seen_versions.aggregate_id, "claim:8259c238232f9585e95fc8f45b0bb410");
    REQUIRE(seen_versions.version == 3u);
    REQUIRE_STR(seen_versions.updated_by_event_id, "evt_000128");
    REQUIRE_STR(seen_versions.source_drop_id, "835:000000102:102:0001");
    REQUIRE_OK(scribe_store_put_outbox_notification(
                &store,
                "claim:835:000000102:102:0001:store-test-run",
                "SourceDropAggregatesRecorded",
                "claim",
                "835:000000102:102:0001",
                "store-test-run",
                "ingest-test-run",
                1u,
                "{\"event_type\":\"SourceDropAggregatesRecorded\"}"
            ));
    memset(&seen_outbox, 0, sizeof(seen_outbox));
    REQUIRE_OK(scribe_store_each_outbox_notification_after(
                &store,
                0,
                8u,
                collect_outbox_notification,
                &seen_outbox
    ));
    REQUIRE(seen_outbox.count == 1u);
    REQUIRE(seen_outbox.first.sequence > 0);
    outbox_sequence = seen_outbox.first.sequence;
    REQUIRE_STR(
        seen_outbox.first.notification_id,
        "claim:835:000000102:102:0001:store-test-run"
    );
    REQUIRE_STR(seen_outbox.first.event_type, "SourceDropAggregatesRecorded");
    REQUIRE_STR(seen_outbox.first.aggregate_type, "claim");
    REQUIRE_STR(seen_outbox.first.source_drop_id, "835:000000102:102:0001");
    REQUIRE_STR(seen_outbox.first.run_id, "store-test-run");
    REQUIRE_STR(seen_outbox.first.source_run_id, "ingest-test-run");
    REQUIRE(seen_outbox.first.aggregate_version_count == 1u);
    REQUIRE(strstr(seen_outbox.first.payload_json, "SourceDropAggregatesRecorded") != NULL);
    memset(&seen_outbox, 0, sizeof(seen_outbox));
    REQUIRE_OK(scribe_store_each_outbox_notification_after(
                &store,
                outbox_sequence,
                8u,
                collect_outbox_notification,
                &seen_outbox
            ));
    REQUIRE(seen_outbox.count == 0u);
    REQUIRE_OK(scribe_store_put_source_drop(
                &store,
                "271:000000111:111:0001",
                "271",
                "/inbound/eligibility/271.edi",
                "2026-09-15T00:00:00Z",
                "sha256:coverage-file"
            ));
    REQUIRE_OK(scribe_store_put_member_coverage(
                &store,
                "member_coverage:38b52e8dd8a4e5deef2342d9af458516",
                2u,
                "{\"version\":2,\"benefit_count\":3}",
                "evt_coverage_001",
                "271:000000111:111:0001"
            ));
    REQUIRE_OK(scribe_store_put_outbox_notification(
                &store,
                "member_coverage:271:000000111:111:0001:store-test-run",
                "SourceDropAggregatesRecorded",
                "member_coverage",
                "271:000000111:111:0001",
                "store-test-run",
                "coverage-ingest-test-run",
                1u,
                "{\"event_type\":\"SourceDropAggregatesRecorded\"}"
            ));
    scribe_delta_exporter_input_init(&delta_input);
    delta_input.read_store_path = db_path;
    delta_input.out_path = delta_path;
    delta_input.after_sequence = 0;
    delta_input.limit = 8u;
    REQUIRE_OK(scribe_delta_exporter_export(&delta_input));
    REQUIRE(!file_exists(delta_wal_path));
    REQUIRE(!file_exists(delta_shm_path));
    REQUIRE(!file_exists(delta_journal_path));
    REQUIRE(query_count(delta_path, "SELECT COUNT(*) FROM outbox_notifications;", &delta_count) == 0);
    REQUIRE(delta_count == 2);
    REQUIRE(query_count(delta_path, "SELECT COUNT(*) FROM aggregate_versions;", &delta_count) == 0);
    REQUIRE(delta_count == 2);
    REQUIRE(query_count(delta_path, "SELECT COUNT(*) FROM source_drops;", &delta_count) == 0);
    REQUIRE(delta_count == 2);
    REQUIRE(query_count(
                delta_path,
                "SELECT COUNT(*) FROM metadata "
                "WHERE key = 'schema_version' AND value = '1';",
                &delta_count
            ) == 0);
    REQUIRE(delta_count == 1);
    REQUIRE(query_count(
                delta_path,
                "SELECT COUNT(*) FROM aggregate_versions "
                "WHERE aggregate_type = 'claim' "
                "AND aggregate_id = 'claim:8259c238232f9585e95fc8f45b0bb410' "
                "AND version = 3 "
                "AND source_drop_id = '835:000000102:102:0001';",
                &delta_count
            ) == 0);
    REQUIRE(delta_count == 1);
    REQUIRE(query_count(
                delta_path,
                "SELECT COUNT(*) FROM aggregate_versions "
                "WHERE aggregate_type = 'member_coverage' "
                "AND aggregate_id = 'member_coverage:38b52e8dd8a4e5deef2342d9af458516' "
                "AND version = 2 "
                "AND source_drop_id = '271:000000111:111:0001';",
                &delta_count
            ) == 0);
    REQUIRE(delta_count == 1);

    REQUIRE_OK(scribe_store_put_claim_aggregate_key(
                &store,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                "claim:8259c238232f9585e95fc8f45b0bb410"
            ));
    REQUIRE_OK(scribe_store_find_claim_aggregate_ids_by_key(
                &store,
                "claim_id",
                "8259c238232f9585e95fc8f45b0bb410",
                aggregate_ids,
                4u,
                &count
            ));
    REQUIRE(count == 1u);
    REQUIRE_STR(aggregate_ids[0], "claim:8259c238232f9585e95fc8f45b0bb410");
    REQUIRE_OK(scribe_store_put_aggregate_event_route(
                &store,
                "claim",
                "claim:8259c238232f9585e95fc8f45b0bb410",
                "evt_000128"
            ));
    REQUIRE_OK(scribe_store_mark_dirty_aggregate(
                &store,
                "claim",
                "claim:8259c238232f9585e95fc8f45b0bb410",
                "835:000000102:102:0001",
                "evt_000128"
            ));
    REQUIRE_OK(scribe_store_clear_dirty_aggregate(
                &store,
                "claim",
                "claim:8259c238232f9585e95fc8f45b0bb410",
                "835:000000102:102:0001"
            ));

    REQUIRE(scribe_store_get_event_locator(&store, "evt_missing", &locator) == X12_ERR_NOT_FOUND);
    REQUIRE_OK(scribe_store_close(&store));
    (void)remove(db_path);
    (void)remove(db_wal_path);
    (void)remove(db_shm_path);
    (void)remove(delta_path);
    (void)remove(delta_wal_path);
    (void)remove(delta_shm_path);
    (void)remove(delta_journal_path);

    return 0;
}

int main(void)
{
    REQUIRE(test_store_indexes_and_aggregates() == 0);
    return 0;
}
