#include "store.h"
#include "test_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_store_indexes_and_aggregates(void)
{
    char db_path[512];
    char db_wal_path[560];
    char db_shm_path[560];
    char event_ids[4][SCRIBE_STORE_ID_MAX];
    char aggregate_ids[4][SCRIBE_STORE_ID_MAX];
    char state_json[512];
    scribe_store_t store;
    scribe_source_drop_t source_drop;
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

    return 0;
}

int main(void)
{
    REQUIRE(test_store_indexes_and_aggregates() == 0);
    return 0;
}
