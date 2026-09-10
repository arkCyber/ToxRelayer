/*  msg_database.h
 *
 *  Store-and-forward message persistence (SQLite3 backend) for the metaRelayer.
 *
 *  Design constraints:
 *   - Single threaded. Every entry point must be called from the toxrelayer event
 *     loop; no locking is performed here.
 *   - No function in this module calls exit()/abort(). Every failure is reported
 *     through an MSG_DB_ERR_* return code so the caller stays in control.
 *   - Payloads reach SQLite through bound parameters, never through string
 *     interpolation, therefore untrusted message content cannot alter the
 *     statement that is executed.
 *   - Every string copied into a fixed-size field is length-checked and clamped,
 *     so no input can overflow a destination buffer.
 *
 *  On-disk schema (the table name keeps its historical spelling so that
 *  databases created by earlier builds remain readable):
 *
 *    CHAT_MESSEAGE(
 *        UTC_DateTime INTEGER PRIMARY KEY,  -- unix epoch seconds
 *        Msg_Sender   TEXT,                 -- sender public key / identity
 *        Msg_SN       INTEGER,              -- 1..999 telegram serial number
 *        Friend_Num   INTEGER,              -- local friend / channel number
 *        Send_Receive TEXT,                 -- "incoming" | "outgoing"
 *        Chat_Message TEXT )
 */
#ifndef MSG_DATABASE_H
#define MSG_DATABASE_H

#include <stdbool.h>
#include <time.h>

/* Return codes. All are negative except MSG_DB_OK. */
#define MSG_DB_OK              0
#define MSG_DB_ERR_NOT_READY  (-1)  /* database has not been opened successfully */
#define MSG_DB_ERR_OPEN       (-2)  /* sqlite3_open_v2 / sqlite3_open failed     */
#define MSG_DB_ERR_SCHEMA     (-3)  /* schema creation / verification failed     */
#define MSG_DB_ERR_PREPARE    (-4)  /* sqlite3_prepare_v2 failed                 */
#define MSG_DB_ERR_BIND       (-5)  /* sqlite3_bind_* failed                     */
#define MSG_DB_ERR_STEP       (-6)  /* sqlite3_step did not return DONE/ROW      */
#define MSG_DB_ERR_RANGE      (-7)  /* caller-supplied argument out of range     */

/* Field capacities, used for clamping before any copy. */
#define MSG_DB_SENDER_MAX      128
#define MSG_DB_MESSAGE_MAX     1024
#define MSG_DB_DIRECTION_MAX   16

/* Number of most recent records kept for the `/top` style display. */
#define MAX_MSG_TOP            10

struct CHAT_MSG {
    char sender[MSG_DB_SENDER_MAX];
    char message[MSG_DB_MESSAGE_MAX];
};

/* A stored telegram retrieved for replay, including its serial number. */
struct CHAT_MSG_RECORD {
    int    msg_sn;
    time_t datetime;
    char   sender[MSG_DB_SENDER_MAX];
    char   message[MSG_DB_MESSAGE_MAX];
};

/* Open the message database, creating the file and schema when missing, and
 * enable WAL journalling for crash resilience.
 *
 * Preconditions : none.
 * Postconditions: msg_database_is_ready() returns true iff the result is MSG_DB_OK.
 * Idempotent    : calling it while already open is a no-op that returns MSG_DB_OK.
 *
 * @return MSG_DB_OK on success, otherwise an MSG_DB_ERR_* code.
 */
int msg_database_open(void);

/* Release the database handle. Safe to call when not open. */
void msg_database_close(void);

/* @return true when a usable handle with a verified schema is present. */
bool msg_database_is_ready(void);

/* Append one store-and-forward record.
 *
 * Preconditions : `direction` is non-NULL. `sender` and `chat_msg` may be NULL
 *                 (treated as the empty string).
 * Postconditions: on MSG_DB_OK exactly one row has been committed.
 * Notes         : `msg_sn` is clamped into [0, 999]; `friend_num` must be >= 0;
 *                 `datetime` is stored verbatim.
 *
 * @return MSG_DB_OK, MSG_DB_ERR_NOT_READY, MSG_DB_ERR_RANGE, MSG_DB_ERR_PREPARE,
 *         MSG_DB_ERR_BIND or MSG_DB_ERR_STEP.
 */
int msg_database_add(int friend_num, int msg_sn, time_t datetime,
                     const char *sender, const char *direction, const char *chat_msg);

/* Fill `out[0 .. max-1]` with the most recent records.
 *
 * Preconditions : `out` is non-NULL and `max` is > 0.
 * Postconditions: entries are written newest first and every field is
 *                 NUL-terminated. Unused entries are left untouched.
 *
 * @return the number of records written (0..max), or an MSG_DB_ERR_* code.
 */
int msg_database_top(struct CHAT_MSG *out, int max);

/* Read the highest serial number previously exchanged with `friend_num` for the
 * given `direction`, so serial numbers can be resumed after a restart.
 *
 * Preconditions : `direction` and `sn_out` are non-NULL.
 * Postconditions: on MSG_DB_OK, *sn_out holds the stored value (0 when no row exists).
 *
 * @return MSG_DB_OK, MSG_DB_ERR_NOT_READY, MSG_DB_ERR_RANGE, MSG_DB_ERR_PREPARE,
 *         MSG_DB_ERR_BIND or MSG_DB_ERR_STEP.
 */
int msg_database_last_sn(int friend_num, const char *direction, int *sn_out);

/* Retrieve stored telegrams for one (friend, direction) pair, for replay to a
 * peer that reported a gap.
 *
 * Serial numbers wrap, so the requested range is passed in explicit order:
 *   - when from <= to, serials from..to are fetched;
 *   - when from >  to, the range crosses the wrap point and the serials
 *     from..maximum together with minimum..to are fetched.
 * This keeps the module free of any dependency on the telegram module.
 *
 * Preconditions : direction non-NULL; out != NULL; max > 0.
 * Postconditions: records are written oldest first (by timestamp) and every
 *                 field is NUL terminated. Unused entries are left untouched.
 *
 * @return the number of records written (0..max), or an MSG_DB_ERR_* code.
 */
int msg_database_range(int friend_num, const char *direction,
                       int from, int to, struct CHAT_MSG_RECORD *out, int max);

#endif /* MSG_DATABASE_H */
