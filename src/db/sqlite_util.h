#ifndef SCRIBE_DB_SQLITE_UTIL_H
#define SCRIBE_DB_SQLITE_UTIL_H

#include "x12_parser.h"

#include <sqlite3.h>

#include <stddef.h>

int scribe_sqlite_to_x12(int rc);

int scribe_sqlite_exec(sqlite3 *db, const char *sql);
int scribe_sqlite_prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt);

int scribe_sqlite_bind_text(sqlite3_stmt *stmt, int index, const char *value);
int scribe_sqlite_bind_x12_str(sqlite3_stmt *stmt, int index, x12_str_t value);

int scribe_sqlite_step_done(sqlite3_stmt *stmt);
int scribe_sqlite_reset_stmt(sqlite3_stmt *stmt);
int scribe_sqlite_finalize(sqlite3_stmt **stmt);

int scribe_sqlite_copy_column_text(
    sqlite3_stmt *stmt,
    int column,
    char *out,
    size_t out_len);

char *scribe_sqlite_sidecar_path(sqlite3 *db, const char *suffix);

#endif
