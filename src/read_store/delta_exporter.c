#include "delta_exporter.h"

#include "sqlite_util.h"
#include "store.h"

#include <sqlite3.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define sqlite_to_x12 scribe_sqlite_to_x12
#define exec_sql scribe_sqlite_exec
#define prepare scribe_sqlite_prepare
#define bind_text scribe_sqlite_bind_text
#define step_done scribe_sqlite_step_done
#define reset_stmt scribe_sqlite_reset_stmt
#define finalize_stmt scribe_sqlite_finalize

#define SCRIBE_DELTA_SCHEMA_VERSION "1"
#define SCRIBE_DELTA_DEFAULT_LIMIT 1000u
#define SCRIBE_AGGREGATE_CLAIM "claim"
#define SCRIBE_AGGREGATE_MEMBER_COVERAGE "member_coverage"

static const char SCRIBE_INSERT_AGGREGATE_VERSION_SQL[] =
    "INSERT INTO aggregate_versions "
    "(aggregate_type, aggregate_id, version, state_json, "
    "updated_by_event_id, source_drop_id) "
    "VALUES (?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(aggregate_type, aggregate_id, version) DO UPDATE SET "
    "state_json = excluded.state_json, "
    "updated_by_event_id = excluded.updated_by_event_id, "
    "source_drop_id = excluded.source_drop_id;";

typedef struct
{
    scribe_store_t *source;
    sqlite3 *dest;
    sqlite3_stmt *insert_aggregate_version_stmt;
    long long after_sequence;
    long long max_sequence;
    size_t notification_count;
    size_t aggregate_version_count;
} delta_export_state_t;

typedef struct
{
    delta_export_state_t *state;
    const char *aggregate_type;
} aggregate_version_export_t;

static int finalize_delta_statements(delta_export_state_t *state)
{
    if (state == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return finalize_stmt(&state->insert_aggregate_version_stmt);
}

static int put_metadata(sqlite3 *db, const char *key, const char *value)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    rc = prepare(
        db,
        "INSERT INTO metadata(key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
        &stmt);
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 1, key);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, value);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (stmt != NULL && sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

static int put_metadata_i64(sqlite3 *db, const char *key, long long value)
{
    char text[64];
    int written;

    written = snprintf(text, sizeof(text), "%lld", value);
    if (written < 0 || (size_t)written >= sizeof(text))
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    return put_metadata(db, key, text);
}

static int put_metadata_size(sqlite3 *db, const char *key, size_t value)
{
    char text[64];
    int written;

    written = snprintf(text, sizeof(text), "%zu", value);
    if (written < 0 || (size_t)written >= sizeof(text))
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }
    return put_metadata(db, key, text);
}

static int init_delta_pragmas(sqlite3 *db)
{
    int rc;

    rc = exec_sql(db, "PRAGMA journal_mode = OFF;");
    if (rc == X12_OK)
    {
        rc = exec_sql(db, "PRAGMA synchronous = OFF;");
    }
    if (rc == X12_OK)
    {
        rc = exec_sql(db, "PRAGMA temp_store = MEMORY;");
    }

    return rc;
}

static int init_delta_schema(sqlite3 *db)
{
    int rc;

    rc = exec_sql(db, "DROP TABLE IF EXISTS metadata;");
    if (rc == X12_OK)
    {
        rc = exec_sql(db, "DROP TABLE IF EXISTS outbox_notifications;");
    }
    if (rc == X12_OK)
    {
        rc = exec_sql(db, "DROP TABLE IF EXISTS aggregate_versions;");
    }
    if (rc == X12_OK)
    {
        rc = exec_sql(db, "DROP TABLE IF EXISTS source_drops;");
    }
    if (rc == X12_OK)
    {
        rc = exec_sql(
            db,
            "CREATE TABLE metadata ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL"
            ");");
    }
    if (rc == X12_OK)
    {
        rc = exec_sql(
            db,
            "CREATE TABLE outbox_notifications ("
            "  sequence INTEGER PRIMARY KEY,"
            "  notification_id TEXT NOT NULL UNIQUE,"
            "  event_type TEXT NOT NULL,"
            "  aggregate_type TEXT NOT NULL,"
            "  source_drop_id TEXT NOT NULL,"
            "  run_id TEXT NOT NULL,"
            "  source_run_id TEXT NOT NULL,"
            "  aggregate_version_count INTEGER NOT NULL,"
            "  payload_json TEXT NOT NULL"
            ");");
    }
    if (rc == X12_OK)
    {
        rc = exec_sql(
            db,
            "CREATE TABLE aggregate_versions ("
            "  aggregate_type TEXT NOT NULL,"
            "  aggregate_id TEXT NOT NULL,"
            "  version INTEGER NOT NULL,"
            "  state_json TEXT NOT NULL,"
            "  updated_by_event_id TEXT NOT NULL,"
            "  source_drop_id TEXT NOT NULL,"
            "  PRIMARY KEY (aggregate_type, aggregate_id, version)"
            ");");
    }
    if (rc == X12_OK)
    {
        rc = exec_sql(
            db,
            "CREATE INDEX aggregate_versions_source_drop "
            "ON aggregate_versions(source_drop_id, aggregate_type);");
    }
    if (rc == X12_OK)
    {
        rc = exec_sql(
            db,
            "CREATE TABLE source_drops ("
            "  source_drop_id TEXT PRIMARY KEY,"
            "  source_type TEXT NOT NULL,"
            "  source_file TEXT NOT NULL,"
            "  received_at TEXT NOT NULL,"
            "  file_hash TEXT NOT NULL"
            ");");
    }

    return rc;
}

static int insert_source_drop(sqlite3 *db, const scribe_source_drop_t *source_drop)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (db == NULL || source_drop == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

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
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 1, source_drop->source_drop_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, source_drop->source_type);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, source_drop->source_file);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 4, source_drop->received_at);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 5, source_drop->file_hash);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (stmt != NULL && sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

static int insert_notification(
    sqlite3 *db,
    const scribe_outbox_notification_t *notification)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (db == NULL || notification == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = prepare(
        db,
        "INSERT INTO outbox_notifications "
        "(sequence, notification_id, event_type, aggregate_type, source_drop_id, "
        "run_id, source_run_id, aggregate_version_count, payload_json) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(sequence) DO UPDATE SET "
        "notification_id = excluded.notification_id, "
        "event_type = excluded.event_type, "
        "aggregate_type = excluded.aggregate_type, "
        "source_drop_id = excluded.source_drop_id, "
        "run_id = excluded.run_id, "
        "source_run_id = excluded.source_run_id, "
        "aggregate_version_count = excluded.aggregate_version_count, "
        "payload_json = excluded.payload_json;",
        &stmt);
    if (rc == X12_OK &&
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)notification->sequence) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, notification->notification_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 3, notification->event_type);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 4, notification->aggregate_type);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 5, notification->source_drop_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 6, notification->run_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 7, notification->source_run_id);
    }
    if (rc == X12_OK &&
        sqlite3_bind_int64(
            stmt,
            8,
            (sqlite3_int64)notification->aggregate_version_count) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 9, notification->payload_json);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (stmt != NULL && sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

static int insert_aggregate_version(
    delta_export_state_t *state,
    const char *aggregate_type,
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    const char *updated_by_event_id,
    const char *source_drop_id)
{
    sqlite3_stmt *stmt;
    int rc = X12_OK;

    if (state == NULL || state->dest == NULL || aggregate_type == NULL || aggregate_id == NULL ||
        state_json == NULL || updated_by_event_id == NULL || source_drop_id == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    if (state->insert_aggregate_version_stmt == NULL)
    {
        rc = prepare(
            state->dest,
            SCRIBE_INSERT_AGGREGATE_VERSION_SQL,
            &state->insert_aggregate_version_stmt);
        if (rc != X12_OK)
        {
            return rc;
        }
    }

    stmt = state->insert_aggregate_version_stmt;
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 1, aggregate_type);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 2, aggregate_id);
    }
    if (rc == X12_OK &&
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)version) != SQLITE_OK)
    {
        rc = X12_ERR_IO;
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 4, state_json);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 5, updated_by_event_id);
    }
    if (rc == X12_OK)
    {
        rc = bind_text(stmt, 6, source_drop_id);
    }
    if (rc == X12_OK)
    {
        rc = step_done(stmt);
    }
    if (rc == X12_OK)
    {
        rc = reset_stmt(stmt);
    }

    return rc;
}

static int export_aggregate_version_cb(
    const char *aggregate_id,
    size_t version,
    const char *state_json,
    const char *updated_by_event_id,
    const char *source_drop_id,
    void *user)
{
    aggregate_version_export_t *export = (aggregate_version_export_t *)user;
    int rc;

    if (export == NULL || export->state == NULL || export->aggregate_type == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = insert_aggregate_version(
        export->state,
        export->aggregate_type,
        aggregate_id,
        version,
        state_json,
        updated_by_event_id,
        source_drop_id);
    if (rc == X12_OK)
    {
        export->state->aggregate_version_count++;
    }

    return rc;
}

static int export_notification_aggregate_versions(
    delta_export_state_t *state,
    const scribe_outbox_notification_t *notification)
{
    aggregate_version_export_t export;

    if (state == NULL || notification == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    export.state = state;
    export.aggregate_type = notification->aggregate_type;
    if (strcmp(notification->aggregate_type, SCRIBE_AGGREGATE_CLAIM) == 0)
    {
        return scribe_store_each_claim_aggregate_version_for_source_drop(
            state->source,
            notification->source_drop_id,
            export_aggregate_version_cb,
            &export);
    }
    if (strcmp(notification->aggregate_type, SCRIBE_AGGREGATE_MEMBER_COVERAGE) == 0)
    {
        return scribe_store_each_member_coverage_version_for_source_drop(
            state->source,
            notification->source_drop_id,
            export_aggregate_version_cb,
            &export);
    }

    fprintf(
        stderr,
        "export delta: unsupported aggregate type '%s' in notification '%s'\n",
        notification->aggregate_type,
        notification->notification_id);
    return X12_ERR_UNSUPPORTED;
}

static int export_notification_cb(
    const scribe_outbox_notification_t *notification,
    void *user)
{
    delta_export_state_t *state = (delta_export_state_t *)user;
    scribe_source_drop_t source_drop;
    int rc;

    if (state == NULL || notification == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = insert_notification(state->dest, notification);
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = scribe_store_get_source_drop(state->source, notification->source_drop_id, &source_drop);
    if (rc == X12_OK)
    {
        rc = insert_source_drop(state->dest, &source_drop);
    }
    else if (rc == X12_ERR_NOT_FOUND)
    {
        rc = X12_OK;
    }
    if (rc != X12_OK)
    {
        return rc;
    }

    rc = export_notification_aggregate_versions(state, notification);
    if (rc != X12_OK)
    {
        return rc;
    }

    state->notification_count++;
    if (notification->sequence > state->max_sequence)
    {
        state->max_sequence = notification->sequence;
    }

    return X12_OK;
}

static int write_metadata(delta_export_state_t *state)
{
    int rc;

    if (state == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = put_metadata(state->dest, "schema_version", SCRIBE_DELTA_SCHEMA_VERSION);
    if (rc == X12_OK)
    {
        rc = put_metadata_i64(state->dest, "after_sequence", state->after_sequence);
    }
    if (rc == X12_OK)
    {
        rc = put_metadata_i64(state->dest, "to_sequence", state->max_sequence);
    }
    if (rc == X12_OK)
    {
        rc = put_metadata_size(state->dest, "notification_count", state->notification_count);
    }
    if (rc == X12_OK)
    {
        rc = put_metadata_size(
            state->dest,
            "aggregate_version_count",
            state->aggregate_version_count);
    }

    return rc;
}

void scribe_delta_exporter_input_init(scribe_delta_exporter_input_t *input)
{
    if (input != NULL)
    {
        memset(input, 0, sizeof(*input));
        input->limit = SCRIBE_DELTA_DEFAULT_LIMIT;
    }
}

int scribe_delta_exporter_export(const scribe_delta_exporter_input_t *input)
{
    delta_export_state_t state;
    scribe_store_t source;
    sqlite3 *dest = NULL;
    int owns_source = 0;
    int txn_open = 0;
    int rc;

    if (input == NULL || input->read_store_path == NULL || input->out_path == NULL ||
        input->out_path[0] == '\0' || strcmp(input->out_path, "-") == 0 ||
        input->after_sequence < 0 || input->limit == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    memset(&state, 0, sizeof(state));
    state.after_sequence = input->after_sequence;
    state.max_sequence = input->after_sequence;

    scribe_store_init(&source);
    rc = scribe_store_open(&source, input->read_store_path);
    if (rc == X12_OK)
    {
        owns_source = 1;
    }
    if (rc != X12_OK)
    {
        if (owns_source)
        {
            (void)scribe_store_close(&source);
        }
        return rc;
    }

    if (sqlite3_open(input->out_path, &dest) != SQLITE_OK)
    {
        (void)scribe_store_close(&source);
        if (dest != NULL)
        {
            (void)sqlite3_close(dest);
        }
        return X12_ERR_IO;
    }

    state.source = &source;
    state.dest = dest;

    rc = init_delta_pragmas(dest);
    if (rc == X12_OK)
    {
        rc = exec_sql(dest, "BEGIN IMMEDIATE;");
        if (rc == X12_OK)
        {
            txn_open = 1;
        }
    }
    if (rc == X12_OK)
    {
        rc = init_delta_schema(dest);
    }
    if (rc == X12_OK)
    {
        rc = scribe_store_each_outbox_notification_after(
            &source,
            input->after_sequence,
            input->limit,
            export_notification_cb,
            &state);
    }
    if (rc == X12_OK)
    {
        rc = write_metadata(&state);
    }
    if (state.insert_aggregate_version_stmt != NULL)
    {
        int stmt_rc = finalize_delta_statements(&state);
        if (stmt_rc != X12_OK && rc == X12_OK)
        {
            rc = stmt_rc;
        }
    }
    if (txn_open)
    {
        if (rc == X12_OK)
        {
            rc = exec_sql(dest, "COMMIT;");
        }
        else
        {
            (void)exec_sql(dest, "ROLLBACK;");
        }
    }
    if (sqlite3_close(dest) != SQLITE_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }
    if (scribe_store_close(&source) != X12_OK && rc == X12_OK)
    {
        rc = X12_ERR_IO;
    }

    return rc;
}

static int parse_i64_arg(const char *value, long long *out)
{
    char *end = NULL;
    long long parsed;

    if (value == NULL || out == NULL || value[0] == '\0')
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    errno = 0;
    parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 0)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    *out = parsed;
    return X12_OK;
}

static int parse_size_arg(const char *value, size_t *out)
{
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || out == NULL || value[0] == '\0')
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    *out = (size_t)parsed;
    if ((unsigned long long)*out != parsed)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    return X12_OK;
}

int scribe_delta_exporter_run_cli(int argc, char **argv)
{
    scribe_delta_exporter_input_t input;
    int i;
    int rc;

    scribe_delta_exporter_input_init(&input);

    for (i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--read-store") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            input.read_store_path = argv[++i];
        }
        else if (strcmp(argv[i], "--out") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            input.out_path = argv[++i];
        }
        else if (strcmp(argv[i], "--after-sequence") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            rc = parse_i64_arg(argv[++i], &input.after_sequence);
            if (rc != X12_OK)
            {
                return -1;
            }
        }
        else if (strcmp(argv[i], "--limit") == 0)
        {
            if (i + 1 >= argc)
            {
                return -1;
            }
            rc = parse_size_arg(argv[++i], &input.limit);
            if (rc != X12_OK)
            {
                return -1;
            }
        }
        else
        {
            return -1;
        }
    }

    if (input.read_store_path == NULL || input.out_path == NULL)
    {
        return -1;
    }

    return scribe_delta_exporter_export(&input);
}
