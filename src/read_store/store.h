#ifndef SCRIBE_STORE_H
#define SCRIBE_STORE_H

#include "x12_parser.h"

#include <stddef.h>

#define SCRIBE_STORE_ID_MAX 128u
#define SCRIBE_STORE_TYPE_MAX 96u
#define SCRIBE_STORE_CHECKSUM_MAX 128u
#define SCRIBE_STORE_PATH_MAX 512u

typedef struct {
    void *db;
} scribe_store_t;

typedef struct {
    char source_drop_id[SCRIBE_STORE_ID_MAX];
    char source_type[SCRIBE_STORE_TYPE_MAX];
    char source_file[SCRIBE_STORE_PATH_MAX];
    char received_at[SCRIBE_STORE_ID_MAX];
    char file_hash[SCRIBE_STORE_CHECKSUM_MAX];
} scribe_source_drop_t;

typedef struct {
    char event_id[SCRIBE_STORE_ID_MAX];
    char source_drop_id[SCRIBE_STORE_ID_MAX];
    char event_type[SCRIBE_STORE_TYPE_MAX];
    char segment_id[SCRIBE_STORE_ID_MAX];
    long long offset;
    long long length;
    char checksum[SCRIBE_STORE_CHECKSUM_MAX];
} scribe_event_locator_t;

typedef int (*scribe_latest_aggregate_cb)(
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    void *user
);

void scribe_store_init(scribe_store_t *store);
int scribe_store_open(scribe_store_t *store, const char *path);
int scribe_store_close(scribe_store_t *store);
int scribe_store_init_schema(scribe_store_t *store);
int scribe_store_begin_immediate(scribe_store_t *store);
int scribe_store_commit(scribe_store_t *store);
int scribe_store_rollback(scribe_store_t *store);

int scribe_store_put_source_drop(
    scribe_store_t *store,
    const char *source_drop_id,
    const char *source_type,
    const char *source_file,
    const char *received_at,
    const char *file_hash
);

int scribe_store_get_source_drop(
    scribe_store_t *store,
    const char *source_drop_id,
    scribe_source_drop_t *out
);

int scribe_store_put_event(
    scribe_store_t *store,
    const char *event_id,
    const char *source_drop_id,
    const char *event_type,
    const char *segment_id,
    long long offset,
    long long length,
    const char *checksum
);

int scribe_store_put_event_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    const char *event_id
);

int scribe_store_find_event_ids_by_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    char event_ids[][SCRIBE_STORE_ID_MAX],
    size_t max_event_ids,
    size_t *out_count
);

int scribe_store_get_event_locator(
    scribe_store_t *store,
    const char *event_id,
    scribe_event_locator_t *out
);

int scribe_store_put_aggregate_event_route(
    scribe_store_t *store,
    const char *aggregate_type,
    const char *aggregate_id,
    const char *event_id
);

int scribe_store_mark_dirty_aggregate(
    scribe_store_t *store,
    const char *aggregate_type,
    const char *aggregate_id,
    const char *source_drop_id,
    const char *first_event_id
);

int scribe_store_clear_dirty_aggregate(
    scribe_store_t *store,
    const char *aggregate_type,
    const char *aggregate_id,
    const char *source_drop_id
);

int scribe_store_put_claim_aggregate(
    scribe_store_t *store,
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    const char *updated_by_event_id,
    const char *source_drop_id
);

int scribe_store_get_latest_claim_aggregate(
    scribe_store_t *store,
    const char *aggregate_id,
    size_t *out_version,
    char *state_json,
    size_t state_json_len
);

int scribe_store_each_latest_claim_aggregate(
    scribe_store_t *store,
    scribe_latest_aggregate_cb callback,
    void *user
);

int scribe_store_put_claim_aggregate_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    const char *aggregate_id
);

int scribe_store_find_claim_aggregate_ids_by_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    char aggregate_ids[][SCRIBE_STORE_ID_MAX],
    size_t max_aggregate_ids,
    size_t *out_count
);

int scribe_store_put_member_coverage(
    scribe_store_t *store,
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    const char *updated_by_event_id,
    const char *source_drop_id
);

int scribe_store_put_member_coverage_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    const char *aggregate_id
);

int scribe_store_find_member_coverage_ids_by_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    char aggregate_ids[][SCRIBE_STORE_ID_MAX],
    size_t max_aggregate_ids,
    size_t *out_count
);

int scribe_store_get_latest_member_coverage(
    scribe_store_t *store,
    const char *aggregate_id,
    size_t *out_version,
    char *state_json,
    size_t state_json_len
);

#endif
