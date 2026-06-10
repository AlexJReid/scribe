#include "phi_vault.h"

#include <sqlite3.h>

#include <string.h>

static sqlite3 *vault_db(phi_vault_t *vault)
{
    return vault == NULL ? NULL : (sqlite3 *)vault->db;
}

static int sqlite_to_x12(int rc)
{
    if (rc == SQLITE_NOMEM) {
        return X12_ERR_NO_MEMORY;
    }
    if (rc == SQLITE_CONSTRAINT) {
        return X12_ERR_CONFLICT;
    }
    return X12_ERR_IO;
}

static int exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc;

    if (db == NULL || sql == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err != NULL) {
        sqlite3_free(err);
    }
    return rc == SQLITE_OK ? X12_OK : sqlite_to_x12(rc);
}

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt)
{
    int rc;

    if (db == NULL || sql == NULL || stmt == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
    return rc == SQLITE_OK ? X12_OK : sqlite_to_x12(rc);
}

static int bind_text(sqlite3_stmt *stmt, int index, const char *value)
{
    int rc;

    if (stmt == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (value == NULL) {
        value = "";
    }

    rc = sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
    return rc == SQLITE_OK ? X12_OK : sqlite_to_x12(rc);
}

static int bind_x12_str(sqlite3_stmt *stmt, int index, x12_str_t value)
{
    int rc;

    if (stmt == NULL || value.ptr == NULL || value.len > (size_t)2147483647) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = sqlite3_bind_text(stmt, index, value.ptr, (int)value.len, SQLITE_TRANSIENT);
    return rc == SQLITE_OK ? X12_OK : sqlite_to_x12(rc);
}

static int step_done(sqlite3_stmt *stmt)
{
    int rc;

    if (stmt == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = sqlite3_step(stmt);
    return rc == SQLITE_DONE ? X12_OK : sqlite_to_x12(rc);
}

static int copy_column_text(sqlite3_stmt *stmt, int column, char *out, size_t out_len)
{
    const unsigned char *value;
    int len;

    if (stmt == NULL || out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    value = sqlite3_column_text(stmt, column);
    if (value == NULL) {
        value = (const unsigned char *)"";
    }
    len = sqlite3_column_bytes(stmt, column);
    if (len < 0 || (size_t)len >= out_len) {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, value, (size_t)len);
    out[len] = '\0';
    return X12_OK;
}

static int audit_resolution(
    sqlite3 *db,
    const char *namespace_name,
    const char *token,
    const char *actor,
    const char *purpose,
    const char *result
)
{
    sqlite3_stmt *stmt;
    int rc;

    rc = prepare(
        db,
        "INSERT INTO phi_audit "
        "(action, namespace, token, actor, purpose, result) "
        "VALUES ('resolve', ?, ?, ?, ?, ?);",
        &stmt
    );
    if (rc != X12_OK) {
        return rc;
    }

    rc = bind_text(stmt, 1, namespace_name);
    if (rc == X12_OK) {
        rc = bind_text(stmt, 2, token);
    }
    if (rc == X12_OK) {
        rc = bind_text(stmt, 3, actor);
    }
    if (rc == X12_OK) {
        rc = bind_text(stmt, 4, purpose);
    }
    if (rc == X12_OK) {
        rc = bind_text(stmt, 5, result);
    }
    if (rc == X12_OK) {
        rc = step_done(stmt);
    }
    if (rc == X12_OK && sqlite3_changes(db) == 0) {
        rc = X12_ERR_CONFLICT;
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK) {
        rc = X12_ERR_IO;
    }

    return rc;
}

void phi_vault_init(phi_vault_t *vault)
{
    if (vault != NULL) {
        vault->db = NULL;
    }
}

int phi_vault_open(phi_vault_t *vault, const char *path)
{
    sqlite3 *db = NULL;
    int rc;

    if (vault == NULL || path == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    phi_vault_init(vault);
    rc = sqlite3_open_v2(
        path,
        &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        NULL
    );
    if (rc != SQLITE_OK) {
        if (db != NULL) {
            (void)sqlite3_close(db);
        }
        return sqlite_to_x12(rc);
    }

    vault->db = db;
    (void)sqlite3_busy_timeout(db, 5000);
    return X12_OK;
}

int phi_vault_close(phi_vault_t *vault)
{
    sqlite3 *db = vault_db(vault);
    int rc;

    if (db == NULL) {
        return X12_OK;
    }

    (void)sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    rc = sqlite3_close(db);
    vault->db = NULL;
    return rc == SQLITE_OK ? X12_OK : sqlite_to_x12(rc);
}

int phi_vault_init_schema(phi_vault_t *vault)
{
    sqlite3 *db = vault_db(vault);

    if (db == NULL) {
        return X12_ERR_INVALID_ARGUMENT;
    }

    return exec_sql(
        db,
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "CREATE TABLE IF NOT EXISTS phi_mappings ("
        "  namespace TEXT NOT NULL,"
        "  token TEXT NOT NULL,"
        "  raw_value TEXT NOT NULL,"
        "  first_source_drop_id TEXT NOT NULL,"
        "  last_source_drop_id TEXT NOT NULL,"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY (namespace, token)"
        ");"
        "CREATE TABLE IF NOT EXISTS phi_audit ("
        "  audit_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  action TEXT NOT NULL,"
        "  namespace TEXT NOT NULL,"
        "  token TEXT NOT NULL,"
        "  actor TEXT NOT NULL,"
        "  purpose TEXT NOT NULL,"
        "  result TEXT NOT NULL,"
        "  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE INDEX IF NOT EXISTS phi_audit_lookup "
        "ON phi_audit(namespace, token, created_at);"
    );
}

int phi_vault_put_mapping(
    phi_vault_t *vault,
    const char *namespace_name,
    const char *token,
    x12_str_t raw_value,
    const char *source_drop_id
)
{
    sqlite3_stmt *stmt;
    sqlite3_stmt *check_stmt;
    sqlite3 *db = vault_db(vault);
    const unsigned char *existing;
    int existing_len;
    int step_rc;
    int rc;

    if (db == NULL || namespace_name == NULL || token == NULL ||
        raw_value.ptr == NULL || raw_value.len == 0u ||
        source_drop_id == NULL || source_drop_id[0] == '\0') {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = prepare(
        db,
        "SELECT raw_value FROM phi_mappings WHERE namespace = ? AND token = ?;",
        &check_stmt
    );
    if (rc != X12_OK) {
        return rc;
    }
    rc = bind_text(check_stmt, 1, namespace_name);
    if (rc == X12_OK) {
        rc = bind_text(check_stmt, 2, token);
    }
    step_rc = rc == X12_OK ? sqlite3_step(check_stmt) : SQLITE_DONE;
    if (rc == X12_OK && step_rc == SQLITE_ROW) {
        existing = sqlite3_column_text(check_stmt, 0);
        existing_len = sqlite3_column_bytes(check_stmt, 0);
        if (existing == NULL ||
            existing_len < 0 ||
            (size_t)existing_len != raw_value.len ||
            memcmp(existing, raw_value.ptr, raw_value.len) != 0) {
            rc = X12_ERR_CONFLICT;
        }
    } else if (rc == X12_OK && step_rc != SQLITE_DONE) {
        rc = sqlite_to_x12(step_rc);
    }
    if (sqlite3_finalize(check_stmt) != SQLITE_OK && rc == X12_OK) {
        rc = X12_ERR_IO;
    }
    if (rc != X12_OK) {
        return rc;
    }

    rc = prepare(
        db,
        "INSERT INTO phi_mappings "
        "(namespace, token, raw_value, first_source_drop_id, last_source_drop_id) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(namespace, token) DO UPDATE SET "
        "last_source_drop_id = excluded.last_source_drop_id, "
        "updated_at = CURRENT_TIMESTAMP "
        "WHERE phi_mappings.raw_value = excluded.raw_value;",
        &stmt
    );
    if (rc != X12_OK) {
        return rc;
    }

    rc = bind_text(stmt, 1, namespace_name);
    if (rc == X12_OK) {
        rc = bind_text(stmt, 2, token);
    }
    if (rc == X12_OK) {
        rc = bind_x12_str(stmt, 3, raw_value);
    }
    if (rc == X12_OK) {
        rc = bind_text(stmt, 4, source_drop_id);
    }
    if (rc == X12_OK) {
        rc = bind_text(stmt, 5, source_drop_id);
    }
    if (rc == X12_OK) {
        rc = step_done(stmt);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK) {
        rc = X12_ERR_IO;
    }

    return rc;
}

int phi_vault_resolve(
    phi_vault_t *vault,
    const char *namespace_name,
    const char *token,
    const char *actor,
    const char *purpose,
    char *out,
    size_t out_len
)
{
    sqlite3_stmt *stmt;
    sqlite3 *db = vault_db(vault);
    int step_rc;
    int rc;

    if (db == NULL || namespace_name == NULL || token == NULL ||
        out == NULL || out_len == 0u) {
        return X12_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';

    rc = prepare(
        db,
        "SELECT raw_value FROM phi_mappings "
        "WHERE namespace = ? AND token = ?;",
        &stmt
    );
    if (rc != X12_OK) {
        return rc;
    }

    rc = bind_text(stmt, 1, namespace_name);
    if (rc == X12_OK) {
        rc = bind_text(stmt, 2, token);
    }
    step_rc = rc == X12_OK ? sqlite3_step(stmt) : SQLITE_DONE;
    if (rc == X12_OK && step_rc == SQLITE_ROW) {
        rc = copy_column_text(stmt, 0, out, out_len);
    } else if (rc == X12_OK && step_rc == SQLITE_DONE) {
        rc = X12_ERR_NOT_FOUND;
    } else if (rc == X12_OK) {
        rc = sqlite_to_x12(step_rc);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK && rc == X12_OK) {
        rc = X12_ERR_IO;
    }

    if (rc == X12_OK) {
        return audit_resolution(db, namespace_name, token, actor, purpose, "found");
    }
    if (rc == X12_ERR_NOT_FOUND) {
        int audit_rc = audit_resolution(db, namespace_name, token, actor, purpose, "not_found");
        if (audit_rc != X12_OK) {
            return audit_rc;
        }
    }

    return rc;
}
