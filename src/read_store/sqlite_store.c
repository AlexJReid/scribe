#include "store.h"

#include "sqlite_util.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define sqlite_to_x12 scribe_sqlite_to_x12
#define exec_sql scribe_sqlite_exec
#define prepare scribe_sqlite_prepare
#define bind_text scribe_sqlite_bind_text
#define step_done scribe_sqlite_step_done
#define copy_column_text scribe_sqlite_copy_column_text
#define sqlite_sidecar_path scribe_sqlite_sidecar_path

static sqlite3 *store_db(scribe_store_t *store)
{
    return store == NULL ? NULL : (sqlite3 *)store->db;
}

void scribe_store_init(scribe_store_t *store)
{
    if (store != NULL)
    {
        store->db = NULL;
    }
}

int scribe_store_open(scribe_store_t *store, const char *path)
{
    sqlite3 *db = NULL;
    int rc;

    if (store == NULL || path == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    scribe_store_init(store);
    rc = sqlite3_open_v2(
        path,
        &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL);
    if (rc != SQLITE_OK)
    {
        if (db != NULL)
        {
            (void)sqlite3_close(db);
        }
        return sqlite_to_x12(rc);
    }

    store->db = db;
    (void)sqlite3_busy_timeout(db, 5000);
    rc = exec_sql(db, "PRAGMA foreign_keys = ON;");
    if (rc != X12_OK)
    {
        (void)scribe_store_close(store);
        return rc;
    }

    return X12_OK;
}

int scribe_store_close(scribe_store_t *store)
{
    sqlite3 *db;
    char *wal_path;
    char *shm_path;
    int rc;

    db = store_db(store);
    if (db == NULL)
    {
        return X12_OK;
    }

    wal_path = sqlite_sidecar_path(db, "-wal");
    shm_path = sqlite_sidecar_path(db, "-shm");
    (void)sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    (void)sqlite3_exec(db, "PRAGMA journal_mode = DELETE;", NULL, NULL, NULL);
    rc = sqlite3_close(db);
    store->db = NULL;
    if (rc == SQLITE_OK)
    {
        if (wal_path != NULL)
        {
            (void)remove(wal_path);
        }
        if (shm_path != NULL)
        {
            (void)remove(shm_path);
        }
    }
    free(wal_path);
    free(shm_path);
    return rc == SQLITE_OK ? X12_OK : sqlite_to_x12(rc);
}

int scribe_store_init_schema(scribe_store_t *store)
{
    sqlite3 *db = store_db(store);

    if (db == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return exec_sql(
        db,
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "CREATE TABLE IF NOT EXISTS source_drops ("
        "  source_drop_id TEXT PRIMARY KEY,"
        "  source_type TEXT NOT NULL,"
        "  source_file TEXT NOT NULL,"
        "  received_at TEXT NOT NULL,"
        "  file_hash TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS events ("
        "  event_id TEXT PRIMARY KEY,"
        "  source_drop_id TEXT NOT NULL REFERENCES source_drops(source_drop_id),"
        "  event_type TEXT NOT NULL,"
        "  segment_id TEXT NOT NULL,"
        "  event_offset INTEGER NOT NULL,"
        "  event_length INTEGER NOT NULL,"
        "  checksum TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS event_keys ("
        "  key_type TEXT NOT NULL,"
        "  key_value TEXT NOT NULL,"
        "  event_id TEXT NOT NULL REFERENCES events(event_id),"
        "  PRIMARY KEY (key_type, key_value, event_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS event_keys_lookup "
        "ON event_keys(key_type, key_value);"
        "CREATE INDEX IF NOT EXISTS events_source_drop "
        "ON events(source_drop_id, segment_id, event_offset);"
        "CREATE TABLE IF NOT EXISTS claim_aggregate_versions ("
        "  aggregate_id TEXT NOT NULL,"
        "  version INTEGER NOT NULL,"
        "  state_json TEXT NOT NULL,"
        "  updated_by_event_id TEXT NOT NULL,"
        "  source_drop_id TEXT NOT NULL,"
        "  PRIMARY KEY (aggregate_id, version)"
        ");"
        "CREATE INDEX IF NOT EXISTS claim_aggregate_versions_source_drop "
        "ON claim_aggregate_versions(source_drop_id, aggregate_id, version);"
        "CREATE TABLE IF NOT EXISTS claim_aggregate_latest ("
        "  aggregate_id TEXT PRIMARY KEY,"
        "  version INTEGER NOT NULL,"
        "  state_json TEXT NOT NULL,"
        "  updated_by_event_id TEXT NOT NULL,"
        "  source_drop_id TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS claim_aggregate_keys ("
        "  key_type TEXT NOT NULL,"
        "  key_value TEXT NOT NULL,"
        "  aggregate_id TEXT NOT NULL,"
        "  PRIMARY KEY (key_type, key_value, aggregate_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS claim_aggregate_keys_lookup "
        "ON claim_aggregate_keys(key_type, key_value);"
        "CREATE TABLE IF NOT EXISTS member_coverage_versions ("
        "  aggregate_id TEXT NOT NULL,"
        "  version INTEGER NOT NULL,"
        "  state_json TEXT NOT NULL,"
        "  updated_by_event_id TEXT NOT NULL,"
        "  source_drop_id TEXT NOT NULL,"
        "  PRIMARY KEY (aggregate_id, version)"
        ");"
        "CREATE INDEX IF NOT EXISTS member_coverage_versions_source_drop "
        "ON member_coverage_versions(source_drop_id, aggregate_id, version);"
        "CREATE TABLE IF NOT EXISTS member_coverage_latest ("
        "  aggregate_id TEXT PRIMARY KEY,"
        "  version INTEGER NOT NULL,"
        "  state_json TEXT NOT NULL,"
        "  updated_by_event_id TEXT NOT NULL,"
        "  source_drop_id TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS member_coverage_keys ("
        "  key_type TEXT NOT NULL,"
        "  key_value TEXT NOT NULL,"
        "  aggregate_id TEXT NOT NULL,"
        "  PRIMARY KEY (key_type, key_value, aggregate_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS member_coverage_keys_lookup "
        "ON member_coverage_keys(key_type, key_value);"
        "CREATE TABLE IF NOT EXISTS aggregate_event_routes ("
        "  aggregate_type TEXT NOT NULL,"
        "  aggregate_id TEXT NOT NULL,"
        "  event_id TEXT NOT NULL REFERENCES events(event_id),"
        "  PRIMARY KEY (aggregate_type, aggregate_id, event_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS aggregate_event_routes_lookup "
        "ON aggregate_event_routes(aggregate_type, aggregate_id);"
        "CREATE TABLE IF NOT EXISTS dirty_aggregates ("
        "  aggregate_type TEXT NOT NULL,"
        "  aggregate_id TEXT NOT NULL,"
        "  source_drop_id TEXT NOT NULL,"
        "  first_event_id TEXT NOT NULL,"
        "  PRIMARY KEY (aggregate_type, aggregate_id, source_drop_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS outbox_notifications ("
        "  sequence INTEGER PRIMARY KEY,"
        "  notification_id TEXT NOT NULL UNIQUE,"
        "  event_type TEXT NOT NULL,"
        "  aggregate_type TEXT NOT NULL,"
        "  source_drop_id TEXT NOT NULL,"
        "  run_id TEXT NOT NULL,"
        "  source_run_id TEXT NOT NULL,"
        "  aggregate_version_count INTEGER NOT NULL,"
        "  payload_json TEXT NOT NULL,"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE INDEX IF NOT EXISTS outbox_notifications_source_drop "
        "ON outbox_notifications(source_drop_id, aggregate_type);");
}

int scribe_store_begin_immediate(scribe_store_t *store)
{
    sqlite3 *db = store_db(store);

    if (db == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    return exec_sql(db, "BEGIN IMMEDIATE;");
}

int scribe_store_commit(scribe_store_t *store)
{
    sqlite3 *db = store_db(store);

    if (db == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    return exec_sql(db, "COMMIT;");
}

int scribe_store_rollback(scribe_store_t *store)
{
    sqlite3 *db = store_db(store);

    if (db == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    return exec_sql(db, "ROLLBACK;");
}

int scribe_store_put_source_drop(
    scribe_store_t *store,
    const char *source_drop_id,
    const char *source_type,
    const char *source_file,
    const char *received_at,
    const char *file_hash)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int rc;

    rc = prepare(
        db,
        "INSERT INTO source_drops "
        "(source_drop_id, source_type, source_file, received_at, file_hash) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(source_drop_id) DO UPDATE SET "
        "source_type = excluded.source_type, "
        "source_file = excluded.source_file, "
        "received_at = excluded.received_at, "
        "file_hash = excluded.file_hash;",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, source_drop_id);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, source_type);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, source_file);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 4, received_at);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 5, file_hash);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

int scribe_store_get_source_drop(
    scribe_store_t *store,
    const char *source_drop_id,
    scribe_source_drop_t *out)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int step_rc;
    int rc;

    if (source_drop_id == NULL || out == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    rc = prepare(
        db,
        "SELECT source_drop_id, source_type, source_file, received_at, file_hash "
        "FROM source_drops WHERE source_drop_id = ?;",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, source_drop_id);
    step_rc = rc == X12_OK ? sqlite3_step(stmt) : SQLITE_DONE;
    if (rc == X12_OK && step_rc == SQLITE_ROW)
    {
        rc = copy_column_text(stmt, 0, out->source_drop_id, sizeof(out->source_drop_id));
        if (rc == X12_OK)
        {
            rc = copy_column_text(stmt, 1, out->source_type, sizeof(out->source_type));
        }
        if (rc == X12_OK)
        {
            rc = copy_column_text(stmt, 2, out->source_file, sizeof(out->source_file));
        }
        if (rc == X12_OK)
        {
            rc = copy_column_text(stmt, 3, out->received_at, sizeof(out->received_at));
        }
        if (rc == X12_OK)
        {
            rc = copy_column_text(stmt, 4, out->file_hash, sizeof(out->file_hash));
        }
    }
    else if (rc == X12_OK && step_rc == SQLITE_DONE)
    {
        rc = X12_ERR_NOT_FOUND;
    }
    else if (rc == X12_OK)
    {
        rc = sqlite_to_x12(step_rc);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

int scribe_store_put_event(
    scribe_store_t *store,
    const char *event_id,
    const char *source_drop_id,
    const char *event_type,
    const char *segment_id,
    long long offset,
    long long length,
    const char *checksum)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int rc;

    rc = prepare(
        db,
        "INSERT INTO events "
        "(event_id, source_drop_id, event_type, segment_id, event_offset, event_length, checksum) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(event_id) DO UPDATE SET "
        "source_drop_id = excluded.source_drop_id, "
        "event_type = excluded.event_type, "
        "segment_id = excluded.segment_id, "
        "event_offset = excluded.event_offset, "
        "event_length = excluded.event_length, "
        "checksum = excluded.checksum;",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, event_id);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, source_drop_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, event_type);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 4, segment_id);
    }
    if (rc == X12_OK &&
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64)offset) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }
    if (rc == X12_OK &&
        sqlite3_bind_int64(stmt, 6, (sqlite3_int64)length) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 7, checksum);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

int scribe_store_put_event_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    const char *event_id)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int rc;

    rc = prepare(
        db,
        "INSERT OR IGNORE INTO event_keys (key_type, key_value, event_id) "
        "VALUES (?, ?, ?);",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, key_type);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, key_value);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, event_id);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

int scribe_store_find_event_ids_by_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    char event_ids[][SCRIBE_STORE_ID_MAX],
    size_t max_event_ids,
    size_t *out_count)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int step_rc;
    int rc;
    size_t count = 0u;

    if (out_count == NULL || (event_ids == NULL && max_event_ids > 0u))
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0u;

    rc = prepare(
        db,
        "SELECT event_keys.event_id "
        "FROM event_keys "
        "JOIN events ON events.event_id = event_keys.event_id "
        "WHERE event_keys.key_type = ? AND event_keys.key_value = ? "
        "ORDER BY events.segment_id, events.event_offset, events.event_id "
        "LIMIT ?;",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, key_type);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, key_value);
    }
    if (rc == X12_OK &&
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)max_event_ids) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }

    while (rc == X12_OK && (step_rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        rc = copy_column_text(stmt, 0, event_ids[count], SCRIBE_STORE_ID_MAX);
        if (rc != X12_OK)
        {
            break;
        }
        count++;
    }
    if (rc == X12_OK && step_rc != SQLITE_DONE)
    {
        rc = sqlite_to_x12(step_rc);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    *out_count = count;
    return rc;
}

int scribe_store_get_event_locator(
    scribe_store_t *store,
    const char *event_id,
    scribe_event_locator_t *out)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int step_rc;
    int rc;

    if (out == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));

    rc = prepare(
        db,
        "SELECT event_id, source_drop_id, event_type, segment_id, "
        "event_offset, event_length, checksum "
        "FROM events WHERE event_id = ?;",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, event_id);
    step_rc = rc == X12_OK ? sqlite3_step(stmt) : SQLITE_DONE;
    if (rc == X12_OK && step_rc == SQLITE_ROW)
    {
        rc = copy_column_text(stmt, 0, out->event_id, sizeof(out->event_id));
        if (rc == X12_OK)
        {
            rc = copy_column_text(stmt, 1, out->source_drop_id, sizeof(out->source_drop_id));
        }
        if (rc == X12_OK)
        {
            rc = copy_column_text(stmt, 2, out->event_type, sizeof(out->event_type));
        }
        if (rc == X12_OK)
        {
            rc = copy_column_text(stmt, 3, out->segment_id, sizeof(out->segment_id));
        }
        if (rc == X12_OK)
        {
            out->offset = (long long)sqlite3_column_int64(stmt, 4);
            out->length = (long long)sqlite3_column_int64(stmt, 5);
            rc = copy_column_text(stmt, 6, out->checksum, sizeof(out->checksum));
        }
    }
    else if (rc == X12_OK && step_rc == SQLITE_DONE)
    {
        rc = X12_ERR_NOT_FOUND;
    }
    else if (rc == X12_OK)
    {
        rc = sqlite_to_x12(step_rc);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

int scribe_store_put_aggregate_event_route(
    scribe_store_t *store,
    const char *aggregate_type,
    const char *aggregate_id,
    const char *event_id)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int rc;

    rc = prepare(
        db,
        "INSERT OR IGNORE INTO aggregate_event_routes "
        "(aggregate_type, aggregate_id, event_id) VALUES (?, ?, ?);",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, aggregate_type);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, aggregate_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, event_id);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

int scribe_store_mark_dirty_aggregate(
    scribe_store_t *store,
    const char *aggregate_type,
    const char *aggregate_id,
    const char *source_drop_id,
    const char *first_event_id)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int rc;

    rc = prepare(
        db,
        "INSERT INTO dirty_aggregates "
        "(aggregate_type, aggregate_id, source_drop_id, first_event_id) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(aggregate_type, aggregate_id, source_drop_id) DO NOTHING;",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, aggregate_type);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, aggregate_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, source_drop_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 4, first_event_id);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

int scribe_store_clear_dirty_aggregate(
    scribe_store_t *store,
    const char *aggregate_type,
    const char *aggregate_id,
    const char *source_drop_id)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int rc;

    rc = prepare(
        db,
        "DELETE FROM dirty_aggregates "
        "WHERE aggregate_type = ? AND aggregate_id = ? AND source_drop_id = ?;",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, aggregate_type);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, aggregate_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, source_drop_id);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

int scribe_store_put_outbox_notification(
    scribe_store_t *store,
    const char *notification_id,
    const char *event_type,
    const char *aggregate_type,
    const char *source_drop_id,
    const char *run_id,
    const char *source_run_id,
    size_t aggregate_version_count,
    const char *payload_json)
{
    sqlite3_stmt *stmt = NULL;
    sqlite3 *db = store_db(store);
    int owns_txn;
    int rc;

    if (db == NULL || notification_id == NULL || event_type == NULL ||
        aggregate_type == NULL || source_drop_id == NULL || run_id == NULL ||
        source_run_id == NULL || payload_json == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    owns_txn = sqlite3_get_autocommit(db) != 0;
    if (owns_txn)
    {
        rc = exec_sql(db, "BEGIN IMMEDIATE;");
        if (rc != X12_OK)
        {
            return rc;
        }
    }

    rc = prepare(
        db,
        "INSERT INTO outbox_notifications "
        "(notification_id, event_type, aggregate_type, source_drop_id, run_id, "
        "source_run_id, aggregate_version_count, payload_json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(notification_id) DO UPDATE SET "
        "event_type = excluded.event_type, "
        "aggregate_type = excluded.aggregate_type, "
        "source_drop_id = excluded.source_drop_id, "
        "run_id = excluded.run_id, "
        "source_run_id = excluded.source_run_id, "
        "aggregate_version_count = excluded.aggregate_version_count, "
        "payload_json = excluded.payload_json;",
        &stmt);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 1, notification_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, event_type);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, aggregate_type);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 4, source_drop_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 5, run_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 6, source_run_id);
    }
    if (rc == X12_OK &&
        sqlite3_bind_int64(stmt, 7, (sqlite3_int64)aggregate_version_count) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 8, payload_json);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (stmt != NULL && sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    if (owns_txn)
    {
        if (rc == X12_OK)
        {
            rc = exec_sql(db, "COMMIT;");
        }
        else
        {
            (void)exec_sql(db, "ROLLBACK;");
        }
    }

    return rc;
}

int scribe_store_each_outbox_notification_after(
    scribe_store_t *store,
    long long after_sequence,
    size_t limit,
    scribe_outbox_notification_cb callback,
    void *user)
{
    scribe_outbox_notification_t notification;
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int step_rc;
    int rc;

    if (db == NULL || callback == NULL || limit == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = prepare(
        db,
        "SELECT sequence, notification_id, event_type, aggregate_type, source_drop_id, "
        "run_id, source_run_id, aggregate_version_count, payload_json "
        "FROM outbox_notifications "
        "WHERE sequence > ? "
        "ORDER BY sequence "
        "LIMIT ?;",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)after_sequence) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }
    if (rc == X12_OK &&
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }

    while (rc == X12_OK)
    {
        step_rc = sqlite3_step(stmt);
        if (step_rc == SQLITE_DONE)
        {
            break;
        }
        if (step_rc != SQLITE_ROW)
        {
            rc = sqlite_to_x12(step_rc);
            break;
        }

        memset(&notification, 0, sizeof(notification));
        notification.sequence = (long long)sqlite3_column_int64(stmt, 0);
        rc = copy_column_text(
            stmt,
            1,
            notification.notification_id,
            sizeof(notification.notification_id));
        if (rc == X12_OK)
        {
            rc = copy_column_text(
                stmt,
                2,
                notification.event_type,
                sizeof(notification.event_type));
        }
        if (rc == X12_OK)
        {
            rc = copy_column_text(
                stmt,
                3,
                notification.aggregate_type,
                sizeof(notification.aggregate_type));
        }
        if (rc == X12_OK)
        {
            rc = copy_column_text(
                stmt,
                4,
                notification.source_drop_id,
                sizeof(notification.source_drop_id));
        }
        if (rc == X12_OK)
        {
            rc = copy_column_text(stmt, 5, notification.run_id, sizeof(notification.run_id));
        }
        if (rc == X12_OK)
        {
            rc = copy_column_text(
                stmt,
                6,
                notification.source_run_id,
                sizeof(notification.source_run_id));
        }
        if (rc == X12_OK)
        {
            notification.aggregate_version_count = (size_t)sqlite3_column_int64(stmt, 7);
            rc = copy_column_text(
                stmt,
                8,
                notification.payload_json,
                sizeof(notification.payload_json));
        }
        if (rc == X12_OK)
        {
            rc = callback(&notification, user);
        }
    }

    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

/*
 * The claim and member-coverage aggregates use parallel sets of tables
 * (versions / latest / keys). The helpers below take an aggregate_kind_t
 * and do the work; the public per-kind functions are thin shims.
 */

typedef struct
{
    const char *versions_table;
    const char *latest_table;
    const char *keys_table;
} aggregate_kind_t;

static const aggregate_kind_t aggregate_kind_claim = {
    "claim_aggregate_versions",
    "claim_aggregate_latest",
    "claim_aggregate_keys"};

static const aggregate_kind_t aggregate_kind_member_coverage = {
    "member_coverage_versions",
    "member_coverage_latest",
    "member_coverage_keys"};

static int put_aggregate(
    scribe_store_t *store,
    const aggregate_kind_t *kind,
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    const char *updated_by_event_id,
    const char *source_drop_id)
{
    sqlite3_stmt *stmt = NULL;
    sqlite3 *db = store_db(store);
    char sql[512];
    int owns_txn;
    int rc = X12_OK;
    int i;

    if (db == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    owns_txn = sqlite3_get_autocommit(db) != 0;
    if (owns_txn)
    {
        rc = exec_sql(db, "BEGIN IMMEDIATE;");
        if (rc != X12_OK)
        {
            return rc;
        }
    }

    for (i = 0; i < 2 && rc == X12_OK; i++)
    {
        const char *table = i == 0 ? kind->versions_table : kind->latest_table;
        const char *conflict = i == 0 ? "(aggregate_id, version)" : "(aggregate_id)";

        if (i == 0)
        {
            (void)snprintf(
                sql, sizeof(sql),
                "INSERT INTO %s "
                "(aggregate_id, version, state_json, updated_by_event_id, source_drop_id) "
                "VALUES (?, ?, ?, ?, ?) "
                "ON CONFLICT%s DO UPDATE SET "
                "state_json = excluded.state_json, "
                "updated_by_event_id = excluded.updated_by_event_id, "
                "source_drop_id = excluded.source_drop_id;",
                table, conflict);
        }
        else
        {
            (void)snprintf(
                sql, sizeof(sql),
                "INSERT INTO %s "
                "(aggregate_id, version, state_json, updated_by_event_id, source_drop_id) "
                "VALUES (?, ?, ?, ?, ?) "
                "ON CONFLICT%s DO UPDATE SET "
                "version = excluded.version, "
                "state_json = excluded.state_json, "
                "updated_by_event_id = excluded.updated_by_event_id, "
                "source_drop_id = excluded.source_drop_id "
                "WHERE excluded.version >= %s.version;",
                table, conflict, table);
        }

        rc = prepare(db, sql, &stmt);
        if (rc == X12_OK)
        {
            rc = bind_text(stmt, 1, aggregate_id);
        }
        if (rc == X12_OK &&
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)version) != SQLITE_OK)
        {
            rc = X12_ERR_IO;
        }
        if (rc == X12_OK)
        {
            rc = bind_text(stmt, 3, state_json);
        }
        if (rc == X12_OK)
        {
            rc = bind_text(stmt, 4, updated_by_event_id);
        }
        if (rc == X12_OK)
        {
            rc = bind_text(stmt, 5, source_drop_id);
        }
        if (rc == X12_OK)
        {
            rc = step_done(stmt);
        }
        if (stmt != NULL && sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
        {
            rc = X12_ERR_IO;
        }
        stmt = NULL;
    }

    if (owns_txn && rc == X12_OK)
    {
        rc = exec_sql(db, "COMMIT;");
    }
    else if (owns_txn)
    {
        (void)exec_sql(db, "ROLLBACK;");
    }

    return rc;
}

static int get_latest_aggregate(
    scribe_store_t *store,
    const aggregate_kind_t *kind,
    const char *aggregate_id,
    size_t *out_version,
    char *state_json,
    size_t state_json_len)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    char sql[256];
    int step_rc;
    int rc;

    if (out_version == NULL || state_json == NULL || state_json_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out_version = 0u;
    state_json[0] = '\0';

    (void)snprintf(
        sql, sizeof(sql),
        "SELECT version, state_json FROM %s WHERE aggregate_id = ?;",
        kind->latest_table);

    rc = prepare(db, sql, &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, aggregate_id);
    step_rc = rc == X12_OK ? sqlite3_step(stmt) : SQLITE_DONE;
    if (rc == X12_OK && step_rc == SQLITE_ROW)
    {
        *out_version = (size_t)sqlite3_column_int64(stmt, 0);
        rc = copy_column_text(stmt, 1, state_json, state_json_len);
    }
    else if (rc == X12_OK && step_rc == SQLITE_DONE)
    {
        rc = X12_ERR_NOT_FOUND;
    }
    else if (rc == X12_OK)
    {
        rc = sqlite_to_x12(step_rc);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

static int each_version_for_source_drop(
    scribe_store_t *store,
    const aggregate_kind_t *kind,
    const char *source_drop_id,
    scribe_aggregate_version_cb callback,
    void *user)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    char sql[512];
    int step_rc;
    int rc;

    if (db == NULL || source_drop_id == NULL || callback == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    (void)snprintf(
        sql, sizeof(sql),
        "SELECT aggregate_id, version, state_json, updated_by_event_id, source_drop_id "
        "FROM %s WHERE source_drop_id = ? ORDER BY aggregate_id, version;",
        kind->versions_table);

    rc = prepare(db, sql, &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, source_drop_id);
    while (rc == X12_OK)
    {
        step_rc = sqlite3_step(stmt);
        if (step_rc == SQLITE_DONE)
        {
            break;
        }
        if (step_rc != SQLITE_ROW)
        {
            rc = sqlite_to_x12(step_rc);
            break;
        }

        rc = callback(
            (const char *)sqlite3_column_text(stmt, 0),
            (size_t)sqlite3_column_int64(stmt, 1),
            (const char *)sqlite3_column_text(stmt, 2),
            (const char *)sqlite3_column_text(stmt, 3),
            (const char *)sqlite3_column_text(stmt, 4),
            user);
    }

    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

static int put_aggregate_key(
    scribe_store_t *store,
    const aggregate_kind_t *kind,
    const char *key_type,
    const char *key_value,
    const char *aggregate_id)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    char sql[256];
    int rc;

    (void)snprintf(
        sql, sizeof(sql),
        "INSERT OR IGNORE INTO %s (key_type, key_value, aggregate_id) VALUES (?, ?, ?);",
        kind->keys_table);

    rc = prepare(db, sql, &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, key_type);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, key_value);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, aggregate_id);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

static int find_aggregate_ids_by_key(
    scribe_store_t *store,
    const aggregate_kind_t *kind,
    const char *key_type,
    const char *key_value,
    char aggregate_ids[][SCRIBE_STORE_ID_MAX],
    size_t max_aggregate_ids,
    size_t *out_count)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    char sql[256];
    int step_rc;
    int rc;
    size_t count = 0u;

    if (out_count == NULL || (aggregate_ids == NULL && max_aggregate_ids > 0u))
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0u;

    (void)snprintf(
        sql, sizeof(sql),
        "SELECT aggregate_id FROM %s "
        "WHERE key_type = ? AND key_value = ? "
        "ORDER BY aggregate_id LIMIT ?;",
        kind->keys_table);

    rc = prepare(db, sql, &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = bind_text(stmt, 1, key_type);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, key_value);
    }
    if (rc == X12_OK &&
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)max_aggregate_ids) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }

    while (rc == X12_OK && (step_rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        rc = copy_column_text(stmt, 0, aggregate_ids[count], SCRIBE_STORE_ID_MAX);
        if (rc != X12_OK)
        {
            break;
        }
        count++;
    }
    if (rc == X12_OK && step_rc != SQLITE_DONE)
    {
        rc = sqlite_to_x12(step_rc);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    *out_count = count;
    return rc;
}

int scribe_store_put_claim_aggregate(
    scribe_store_t *store,
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    const char *updated_by_event_id,
    const char *source_drop_id)
{
    return put_aggregate(
        store, &aggregate_kind_claim,
        aggregate_id, version, state_json, updated_by_event_id, source_drop_id);
}

int scribe_store_get_latest_claim_aggregate(
    scribe_store_t *store,
    const char *aggregate_id,
    size_t *out_version,
    char *state_json,
    size_t state_json_len)
{
    return get_latest_aggregate(
        store, &aggregate_kind_claim,
        aggregate_id, out_version, state_json, state_json_len);
}

int scribe_store_each_latest_claim_aggregate(
    scribe_store_t *store,
    scribe_latest_aggregate_cb callback,
    void *user)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = store_db(store);
    int step_rc;
    int rc;

    if (callback == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = prepare(
        db,
        "SELECT aggregate_id, version, state_json "
        "FROM claim_aggregate_latest "
        "ORDER BY aggregate_id;",
        &stmt);
    if (rc != X12_OK)
    {
        return rc;
    }

    while (rc == X12_OK)
    {
        step_rc = sqlite3_step(stmt);
        if (step_rc == SQLITE_DONE)
        {
            break;
        }
        if (step_rc != SQLITE_ROW)
        {
            rc = sqlite_to_x12(step_rc);
            break;
        }

        rc = callback(
            (const char *)sqlite3_column_text(stmt, 0),
            (size_t)sqlite3_column_int64(stmt, 1),
            (const char *)sqlite3_column_text(stmt, 2),
            user);
    }

    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

int scribe_store_each_claim_aggregate_version_for_source_drop(
    scribe_store_t *store,
    const char *source_drop_id,
    scribe_aggregate_version_cb callback,
    void *user)
{
    return each_version_for_source_drop(
        store, &aggregate_kind_claim, source_drop_id, callback, user);
}

int scribe_store_put_claim_aggregate_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    const char *aggregate_id)
{
    return put_aggregate_key(
        store, &aggregate_kind_claim, key_type, key_value, aggregate_id);
}

int scribe_store_find_claim_aggregate_ids_by_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    char aggregate_ids[][SCRIBE_STORE_ID_MAX],
    size_t max_aggregate_ids,
    size_t *out_count)
{
    return find_aggregate_ids_by_key(
        store, &aggregate_kind_claim,
        key_type, key_value, aggregate_ids, max_aggregate_ids, out_count);
}

int scribe_store_put_member_coverage(
    scribe_store_t *store,
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    const char *updated_by_event_id,
    const char *source_drop_id)
{
    return put_aggregate(
        store, &aggregate_kind_member_coverage,
        aggregate_id, version, state_json, updated_by_event_id, source_drop_id);
}

int scribe_store_get_latest_member_coverage(
    scribe_store_t *store,
    const char *aggregate_id,
    size_t *out_version,
    char *state_json,
    size_t state_json_len)
{
    return get_latest_aggregate(
        store, &aggregate_kind_member_coverage,
        aggregate_id, out_version, state_json, state_json_len);
}

int scribe_store_each_member_coverage_version_for_source_drop(
    scribe_store_t *store,
    const char *source_drop_id,
    scribe_aggregate_version_cb callback,
    void *user)
{
    return each_version_for_source_drop(
        store, &aggregate_kind_member_coverage, source_drop_id, callback, user);
}

int scribe_store_put_member_coverage_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    const char *aggregate_id)
{
    return put_aggregate_key(
        store, &aggregate_kind_member_coverage, key_type, key_value, aggregate_id);
}

int scribe_store_find_member_coverage_ids_by_key(
    scribe_store_t *store,
    const char *key_type,
    const char *key_value,
    char aggregate_ids[][SCRIBE_STORE_ID_MAX],
    size_t max_aggregate_ids,
    size_t *out_count)
{
    return find_aggregate_ids_by_key(
        store, &aggregate_kind_member_coverage,
        key_type, key_value, aggregate_ids, max_aggregate_ids, out_count);
}
