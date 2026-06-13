#include "sqlite_util.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

int scribe_sqlite_to_x12(int rc)
{
    if (rc == SQLITE_NOMEM)
    {
        return X12_ERR_NO_MEMORY;
    }
    if (rc == SQLITE_CONSTRAINT)
    {
        return X12_ERR_CONFLICT;
    }
    return X12_ERR_IO;
}

int scribe_sqlite_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc;

    if (db == NULL || sql == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err != NULL)
    {
        sqlite3_free(err);
    }
    return rc == SQLITE_OK ? X12_OK : scribe_sqlite_to_x12(rc);
}

int scribe_sqlite_prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt)
{
    int rc;

    if (db == NULL || sql == NULL || stmt == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
    return rc == SQLITE_OK ? X12_OK : scribe_sqlite_to_x12(rc);
}

int scribe_sqlite_bind_text(sqlite3_stmt *stmt, int index, const char *value)
{
    int rc;

    if (stmt == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (value == NULL)
    {
        value = "";
    }

    rc = sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
    return rc == SQLITE_OK ? X12_OK : scribe_sqlite_to_x12(rc);
}

int scribe_sqlite_bind_x12_str(sqlite3_stmt *stmt, int index, x12_str_t value)
{
    int rc;

    if (stmt == NULL || value.ptr == NULL || value.len > (size_t)INT_MAX)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = sqlite3_bind_text(stmt, index, value.ptr, (int)value.len, SQLITE_TRANSIENT);
    return rc == SQLITE_OK ? X12_OK : scribe_sqlite_to_x12(rc);
}

int scribe_sqlite_step_done(sqlite3_stmt *stmt)
{
    int rc;

    if (stmt == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = sqlite3_step(stmt);
    return rc == SQLITE_DONE ? X12_OK : scribe_sqlite_to_x12(rc);
}

int scribe_sqlite_reset_stmt(sqlite3_stmt *stmt)
{
    int rc;

    if (stmt == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    rc = sqlite3_reset(stmt);
    if (rc != SQLITE_OK)
    {
        return scribe_sqlite_to_x12(rc);
    }
    rc = sqlite3_clear_bindings(stmt);
    return rc == SQLITE_OK ? X12_OK : scribe_sqlite_to_x12(rc);
}

int scribe_sqlite_finalize(sqlite3_stmt **stmt)
{
    int rc;

    if (stmt == NULL)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }
    if (*stmt == NULL)
    {
        return X12_OK;
    }

    rc = sqlite3_finalize(*stmt);
    *stmt = NULL;
    return rc == SQLITE_OK ? X12_OK : scribe_sqlite_to_x12(rc);
}

int scribe_sqlite_copy_column_text(
    sqlite3_stmt *stmt,
    int column,
    char *out,
    size_t out_len)
{
    const unsigned char *value;
    int len;

    if (stmt == NULL || out == NULL || out_len == 0u)
    {
        return X12_ERR_INVALID_ARGUMENT;
    }

    value = sqlite3_column_text(stmt, column);
    if (value == NULL)
    {
        value = (const unsigned char *)"";
    }
    len = sqlite3_column_bytes(stmt, column);
    if (len < 0 || (size_t)len >= out_len)
    {
        return X12_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(out, value, (size_t)len);
    out[len] = '\0';
    return X12_OK;
}

char *scribe_sqlite_sidecar_path(sqlite3 *db, const char *suffix)
{
    const char *path;
    char *out;
    size_t path_len;
    size_t suffix_len;

    if (db == NULL || suffix == NULL)
    {
        return NULL;
    }

    path = sqlite3_db_filename(db, "main");
    if (path == NULL || path[0] == '\0')
    {
        return NULL;
    }

    path_len = strlen(path);
    suffix_len = strlen(suffix);
    out = (char *)malloc(path_len + suffix_len + 1u);
    if (out == NULL)
    {
        return NULL;
    }
    memcpy(out, path, path_len);
    memcpy(out + path_len, suffix, suffix_len + 1u);
    return out;
}
