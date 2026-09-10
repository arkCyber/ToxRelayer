/*  msg_database.c
 *
 *  Store-and-forward message persistence (SQLite3 backend).
 *  The contract of every entry point is documented in msg_database.h.
 *
 *  The module owns exactly one connection and three prepared statements that are
 *  compiled once on open and reused for the lifetime of the process. The hot
 *  path (one INSERT per relayed telegram) therefore performs no SQL parsing and
 *  cannot be influenced by message content.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

#include "msg_database.h"

#define MSG_DB_FILENAME     "metaChatMsg.db"
#define MSG_DB_TIMEOUT_MS   5000

/* Historical table name, kept so databases written by earlier builds stay readable. */
#define MSG_DB_TABLE        "CHAT_MESSEAGE"

#define MSG_DB_SN_MIN       0
#define MSG_DB_SN_MAX       999

static const char *SQL_SCHEMA =
    "CREATE TABLE IF NOT EXISTS " MSG_DB_TABLE " ("
        "UTC_DateTime INTEGER PRIMARY KEY,"
        "Msg_Sender   TEXT,"
        "Msg_SN       INTEGER,"
        "Friend_Num   INTEGER,"
        "Send_Receive TEXT,"
        "Chat_Message TEXT);"
    "CREATE INDEX IF NOT EXISTS idx_" MSG_DB_TABLE "_friend_dir_sn "
        "ON " MSG_DB_TABLE " (Friend_Num, Send_Receive, Msg_SN);";

static const char *SQL_INSERT =
    "INSERT OR REPLACE INTO " MSG_DB_TABLE
    " (UTC_DateTime, Msg_Sender, Msg_SN, Friend_Num, Send_Receive, Chat_Message)"
    " VALUES (?1, ?2, ?3, ?4, ?5, ?6);";

static const char *SQL_TOP =
    "SELECT Msg_Sender, Chat_Message FROM " MSG_DB_TABLE
    " ORDER BY UTC_DateTime DESC LIMIT ?1;";

static const char *SQL_LAST_SN =
    "SELECT COALESCE(MAX(Msg_SN), 0) FROM " MSG_DB_TABLE
    " WHERE Friend_Num = ?1 AND Send_Receive = ?2;";

/* Replay queries. Two statements are needed because serial numbers wrap: either
 * the requested range is a plain interval, or it crosses the wrap point. */
static const char *SQL_RANGE_IN =
    "SELECT Msg_SN, UTC_DateTime, Msg_Sender, Chat_Message FROM " MSG_DB_TABLE
    " WHERE Friend_Num = ?1 AND Send_Receive = ?2 AND Msg_SN BETWEEN ?3 AND ?4"
    " ORDER BY UTC_DateTime ASC LIMIT ?5;";

static const char *SQL_RANGE_WRAP =
    "SELECT Msg_SN, UTC_DateTime, Msg_Sender, Chat_Message FROM " MSG_DB_TABLE
    " WHERE Friend_Num = ?1 AND Send_Receive = ?2 AND (Msg_SN >= ?3 OR Msg_SN <= ?4)"
    " ORDER BY UTC_DateTime ASC LIMIT ?5;";

static sqlite3      *g_db          = NULL;
static sqlite3_stmt *g_stmt_insert = NULL;
static sqlite3_stmt *g_stmt_top    = NULL;
static sqlite3_stmt *g_stmt_last   = NULL;
static bool          g_ready       = false;

/* Emit a diagnostic on stderr.
 *
 * CERT ERR33-C requires the result of every library call to be observed.
 * A failure to write to stderr cannot be acted upon, so the value is captured
 * and deliberately discarded here once, instead of being left implicit at every
 * call site. Logging never changes control flow. */
static void db_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vfprintf(stderr, format, args);
    va_end(args);

    (void) written;
}

/* Copy at most dst_size-1 bytes and always NUL terminate. Every string that
 * enters a fixed-size field goes through this helper so no input length can
 * overflow a destination buffer. */
static void copy_bounded(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t n = strlen(src);

    if (n >= dst_size) {
        n = dst_size - 1;
    }

    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Finalize a prepared statement and clear the pointer. Safe with NULL. */
static void finalize_stmt(sqlite3_stmt **stmt)
{
    if (stmt != NULL && *stmt != NULL) {
        sqlite3_finalize(*stmt);
        *stmt = NULL;
    }
}


/* Open the database, ensure the schema exists and compile the statements.
 * Idempotent: a second call while open is a no-op returning MSG_DB_OK. */
int msg_database_open(void)
{
    if (g_ready) {
        return MSG_DB_OK;
    }

    int rc = sqlite3_open_v2(MSG_DB_FILENAME, &g_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             NULL);

    if (rc != SQLITE_OK) {
        db_log("msg_database: cannot open '%s': %s\n",
                MSG_DB_FILENAME, g_db ? sqlite3_errmsg(g_db) : "out of memory");
        msg_database_close();
        return MSG_DB_ERR_OPEN;
    }

    (void) sqlite3_busy_timeout(g_db, MSG_DB_TIMEOUT_MS);

    /* WAL keeps a committed record durable across a crash without a full fsync
     * per telegram. Some filesystems reject WAL; that is non fatal. */
    char *err = NULL;

    if (sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err) != SQLITE_OK) {
        db_log("msg_database: WAL unavailable: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        err = NULL;
    }

    if (sqlite3_exec(g_db, SQL_SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        db_log("msg_database: schema error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        msg_database_close();
        return MSG_DB_ERR_SCHEMA;
    }

    if (sqlite3_prepare_v2(g_db, SQL_INSERT,  -1, &g_stmt_insert, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(g_db, SQL_TOP,     -1, &g_stmt_top,    NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(g_db, SQL_LAST_SN, -1, &g_stmt_last,   NULL) != SQLITE_OK) {
        db_log("msg_database: prepare error: %s\n", sqlite3_errmsg(g_db));
        msg_database_close();
        return MSG_DB_ERR_PREPARE;
    }

    g_ready = true;
    return MSG_DB_OK;
}

/* Release every resource owned by the module. Safe to call at any time. */
void msg_database_close(void)
{
    finalize_stmt(&g_stmt_insert);
    finalize_stmt(&g_stmt_top);
    finalize_stmt(&g_stmt_last);
    g_ready = false;

    if (g_db != NULL) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

bool msg_database_is_ready(void)
{
    return g_ready;
}

/* Append one store-and-forward record. See msg_database.h for the contract. */
int msg_database_add(int friend_num, int msg_sn, time_t datetime,
                     const char *sender, const char *direction, const char *chat_msg)
{
    if (!g_ready || g_stmt_insert == NULL) {
        return MSG_DB_ERR_NOT_READY;
    }

    if (friend_num < 0 || direction == NULL) {
        return MSG_DB_ERR_RANGE;
    }

    if (msg_sn < MSG_DB_SN_MIN) {
        msg_sn = MSG_DB_SN_MIN;
    } else if (msg_sn > MSG_DB_SN_MAX) {
        msg_sn = MSG_DB_SN_MAX;
    }

    const char *sender_s = (sender   != NULL) ? sender   : "";
    const char *msg_s    = (chat_msg != NULL) ? chat_msg : "";

    sqlite3_stmt *s = g_stmt_insert;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);

    int rc = sqlite3_bind_int64(s, 1, (sqlite3_int64) datetime);

    if (rc == SQLITE_OK) { rc = sqlite3_bind_text(s, 2, sender_s,  -1, SQLITE_TRANSIENT); }
    if (rc == SQLITE_OK) { rc = sqlite3_bind_int (s, 3, msg_sn); }
    if (rc == SQLITE_OK) { rc = sqlite3_bind_int (s, 4, friend_num); }
    if (rc == SQLITE_OK) { rc = sqlite3_bind_text(s, 5, direction, -1, SQLITE_TRANSIENT); }
    if (rc == SQLITE_OK) { rc = sqlite3_bind_text(s, 6, msg_s,     -1, SQLITE_TRANSIENT); }

    if (rc != SQLITE_OK) {
        db_log("msg_database: bind failed (%d): %s\n", rc, sqlite3_errmsg(g_db));
        sqlite3_reset(s);
        return MSG_DB_ERR_BIND;
    }

    rc = sqlite3_step(s);
    sqlite3_reset(s);

    if (rc != SQLITE_DONE) {
        db_log("msg_database: insert failed (%d): %s\n", rc, sqlite3_errmsg(g_db));
        return MSG_DB_ERR_STEP;
    }

    return MSG_DB_OK;
}

/* Copy the most recent records into out[0..max-1], newest first. */
int msg_database_top(struct CHAT_MSG *out, int max)
{
    if (!g_ready || g_stmt_top == NULL) {
        return MSG_DB_ERR_NOT_READY;
    }

    if (out == NULL || max <= 0) {
        return MSG_DB_ERR_RANGE;
    }

    sqlite3_stmt *s = g_stmt_top;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);

    if (sqlite3_bind_int(s, 1, max) != SQLITE_OK) {
        return MSG_DB_ERR_BIND;
    }

    int count = 0;
    int rc = SQLITE_DONE;

    while (count < max && (rc = sqlite3_step(s)) == SQLITE_ROW) {
        const char *sender  = (const char *) sqlite3_column_text(s, 0);
        const char *message = (const char *) sqlite3_column_text(s, 1);

        copy_bounded(out[count].sender,  sizeof(out[count].sender),  sender);
        copy_bounded(out[count].message, sizeof(out[count].message), message);
        ++count;
    }

    sqlite3_reset(s);

    if (count < max && rc != SQLITE_DONE) {
        return MSG_DB_ERR_STEP;
    }

    return count;
}

/* Highest serial number already stored for one (friend, direction) pair. */
int msg_database_last_sn(int friend_num, const char *direction, int *sn_out)
{
    if (!g_ready || g_stmt_last == NULL) {
        return MSG_DB_ERR_NOT_READY;
    }

    if (direction == NULL || sn_out == NULL || friend_num < 0) {
        return MSG_DB_ERR_RANGE;
    }

    *sn_out = 0;

    sqlite3_stmt *s = g_stmt_last;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);

    int rc = sqlite3_bind_int(s, 1, friend_num);

    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(s, 2, direction, -1, SQLITE_TRANSIENT);
    }

    if (rc != SQLITE_OK) {
        sqlite3_reset(s);
        return MSG_DB_ERR_BIND;
    }

    rc = sqlite3_step(s);

    if (rc == SQLITE_ROW) {
        *sn_out = sqlite3_column_int(s, 0);
    }

    sqlite3_reset(s);

    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        return MSG_DB_ERR_STEP;
    }

    return MSG_DB_OK;
}

/* Fetch stored telegrams for replay. See msg_database.h for the contract. */
int msg_database_range(int friend_num, const char *direction,
                       int from, int to, struct CHAT_MSG_RECORD *out, int max)
{
    if (!g_ready || g_db == NULL) {
        return MSG_DB_ERR_NOT_READY;
    }

    if (direction == NULL || out == NULL || max <= 0 || friend_num < 0) {
        return MSG_DB_ERR_RANGE;
    }

    if (from < MSG_DB_SN_MIN || from > MSG_DB_SN_MAX ||
        to   < MSG_DB_SN_MIN || to   > MSG_DB_SN_MAX) {
        return MSG_DB_ERR_RANGE;
    }

    /* Prepared on demand: gap recovery is rare, so a cached statement is not
     * worth the extra state. */
    const char *sql = (from <= to) ? SQL_RANGE_IN : SQL_RANGE_WRAP;
    sqlite3_stmt *s = NULL;

    if (sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) != SQLITE_OK) {
        db_log("msg_database: range prepare error: %s\n", sqlite3_errmsg(g_db));
        return MSG_DB_ERR_PREPARE;
    }

    int rc = sqlite3_bind_int(s, 1, friend_num);

    if (rc == SQLITE_OK) { rc = sqlite3_bind_text(s, 2, direction, -1, SQLITE_TRANSIENT); }
    if (rc == SQLITE_OK) { rc = sqlite3_bind_int(s, 3, from); }
    if (rc == SQLITE_OK) { rc = sqlite3_bind_int(s, 4, to); }
    if (rc == SQLITE_OK) { rc = sqlite3_bind_int(s, 5, max); }

    if (rc != SQLITE_OK) {
        db_log("msg_database: range bind failed (%d): %s\n", rc, sqlite3_errmsg(g_db));
        (void) sqlite3_finalize(s);
        return MSG_DB_ERR_BIND;
    }

    int count = 0;
    int step = SQLITE_DONE;

    while (count < max && (step = sqlite3_step(s)) == SQLITE_ROW) {
        out[count].msg_sn = sqlite3_column_int(s, 0);
        out[count].datetime = (time_t) sqlite3_column_int64(s, 1);

        copy_bounded(out[count].sender, sizeof(out[count].sender),
                     (const char *) sqlite3_column_text(s, 2));
        copy_bounded(out[count].message, sizeof(out[count].message),
                     (const char *) sqlite3_column_text(s, 3));
        ++count;
    }

    (void) sqlite3_finalize(s);

    if (count < max && step != SQLITE_DONE) {
        return MSG_DB_ERR_STEP;
    }

    return count;
}
