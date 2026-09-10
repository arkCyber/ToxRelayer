/*  test_msg_database.c
 *
 *  Verification suite for the store-and-forward persistence module.
 *
 *  Every test states its precondition, exercises one entry point and asserts the
 *  postcondition. The suite runs against a throw-away database in a private
 *  temporary directory so it never touches the production metaChatMsg.db.
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_msg_database \
 *         tests/test_msg_database.c src/msg_database.c -lsqlite3
 *      ./test_msg_database
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/msg_database.h"

static int tests_run = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        tests_run++;                                                            \
        if (!(expr)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);      \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

/* T1: opening creates the schema, so the module reports itself ready. */
static void test_open_creates_schema(void)
{
    CHECK(!msg_database_is_ready());
    CHECK(msg_database_open() == MSG_DB_OK);
    CHECK(msg_database_is_ready());

    /* Idempotent: a second open must not corrupt anything. */
    CHECK(msg_database_open() == MSG_DB_OK);
    CHECK(msg_database_is_ready());
}

/* T2: argument validation happens before any SQL is attempted. */
static void test_argument_validation(void)
{
    CHECK(msg_database_add(-1, 1, time(NULL), "pk", "incoming", "x") == MSG_DB_ERR_RANGE);
    CHECK(msg_database_add(0, 1, time(NULL), "pk", NULL, "x") == MSG_DB_ERR_RANGE);

    struct CHAT_MSG out[MAX_MSG_TOP];
    CHECK(msg_database_top(NULL, MAX_MSG_TOP) == MSG_DB_ERR_RANGE);
    CHECK(msg_database_top(out, 0) == MSG_DB_ERR_RANGE);

    int sn = -1;
    CHECK(msg_database_last_sn(0, NULL, &sn) == MSG_DB_ERR_RANGE);
    CHECK(msg_database_last_sn(0, "incoming", NULL) == MSG_DB_ERR_RANGE);
}

/* T3: round trip of a plain record, including serial number and ordering. */
static void test_round_trip(void)
{
    const time_t t0 = 1700000000;

    CHECK(msg_database_add(3, 41, t0,       "AAAA", "incoming", "first") == MSG_DB_OK);
    CHECK(msg_database_add(3, 42, t0 + 10,  "BBBB", "incoming", "second") == MSG_DB_OK);
    CHECK(msg_database_add(3, 43, t0 + 20,  "CCCC", "outgoing", "third") == MSG_DB_OK);

    /* Newest first. */
    struct CHAT_MSG out[MAX_MSG_TOP];
    int n = msg_database_top(out, MAX_MSG_TOP);
    CHECK(n == 3);
    CHECK(strcmp(out[0].message, "third") == 0);
    CHECK(strcmp(out[0].sender, "CCCC") == 0);
    CHECK(strcmp(out[1].message, "second") == 0);
    CHECK(strcmp(out[2].message, "first") == 0);

    /* Serial numbers are tracked per (friend, direction). */
    int sn = -1;
    CHECK(msg_database_last_sn(3, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 42);
    CHECK(msg_database_last_sn(3, "outgoing", &sn) == MSG_DB_OK);
    CHECK(sn == 43);

    /* Unknown peer / direction yields zero, never an error. */
    CHECK(msg_database_last_sn(999, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 0);
}

/* T4: hostile payloads are stored verbatim and cannot alter the schema.
 *     This is the regression test for the former sprintf-based INSERT. */
static void test_injection_is_neutralised(void)
{
    const char *payload = "'); DROP TABLE CHAT_MESSEAGE; --";
    const char *long_msg =
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789";

    CHECK(msg_database_add(7, 1, 1700001000, "EVIL", "incoming", payload) == MSG_DB_OK);
    CHECK(msg_database_add(7, 2, 1700001001, "EVIL", "incoming", long_msg) == MSG_DB_OK);

    /* The table must still exist and still hold the records. */
    struct CHAT_MSG out[MAX_MSG_TOP];
    int n = msg_database_top(out, MAX_MSG_TOP);
    CHECK(n > 0);
    CHECK(strcmp(out[0].message, long_msg) == 0);   /* stored verbatim, not truncated */

    int sn = 0;
    CHECK(msg_database_last_sn(7, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 2);
}

/* T5: out-of-range serial numbers are clamped, not rejected or truncated. */
static void test_serial_number_clamping(void)
{
    CHECK(msg_database_add(9, -5,     1700002000, "X", "incoming", "neg") == MSG_DB_OK);
    CHECK(msg_database_add(9, 100000, 1700002001, "X", "incoming", "big") == MSG_DB_OK);

    int sn = 0;
    CHECK(msg_database_last_sn(9, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 999);   /* 100000 clamped into the 0..999 telegram range */
}

/* T7: replay queries return exactly the requested serial range, in order, and
 *     handle a range that crosses the wrap point. */
static void test_range_replay(void)
{
    struct CHAT_MSG_RECORD recs[16];

    /* Plain interval: serials 2..4 of three stored telegrams. */
    CHECK(msg_database_add(21, 2, 1700003000, "PK", "incoming", "two") == MSG_DB_OK);
    CHECK(msg_database_add(21, 3, 1700003001, "PK", "incoming", "three") == MSG_DB_OK);
    CHECK(msg_database_add(21, 4, 1700003002, "PK", "incoming", "four") == MSG_DB_OK);
    CHECK(msg_database_add(21, 5, 1700003003, "PK", "incoming", "five") == MSG_DB_OK);

    int n = msg_database_range(21, "incoming", 2, 4, recs, 16);
    CHECK(n == 3);
    CHECK(recs[0].msg_sn == 2);
    CHECK(recs[1].msg_sn == 3);
    CHECK(recs[2].msg_sn == 4);
    CHECK(strcmp(recs[0].message, "two") == 0);
    CHECK(strcmp(recs[2].message, "four") == 0);
    CHECK(recs[0].datetime == 1700003000);

    /* The LIMIT is honoured. */
    n = msg_database_range(21, "incoming", 2, 5, recs, 2);
    CHECK(n == 2);
    CHECK(recs[0].msg_sn == 2);
    CHECK(recs[1].msg_sn == 3);

    /* No match yields zero rows, not an error. */
    CHECK(msg_database_range(21, "incoming", 900, 950, recs, 16) == 0);

    /* Direction is part of the key. */
    CHECK(msg_database_range(21, "outgoing", 2, 5, recs, 16) == 0);

    /* A range that crosses the wrap point (from > to) spans 999 and 1. */
    CHECK(msg_database_add(22, 998, 1700004000, "PK", "incoming", "late") == MSG_DB_OK);
    CHECK(msg_database_add(22, 999, 1700004001, "PK", "incoming", "last") == MSG_DB_OK);
    CHECK(msg_database_add(22, 1,   1700004002, "PK", "incoming", "first") == MSG_DB_OK);
    CHECK(msg_database_add(22, 2,   1700004003, "PK", "incoming", "second") == MSG_DB_OK);

    n = msg_database_range(22, "incoming", 998, 1, recs, 16);
    CHECK(n == 3);
    CHECK(recs[0].msg_sn == 998);
    CHECK(recs[1].msg_sn == 999);
    CHECK(recs[2].msg_sn == 1);

    /* Argument validation. */
    CHECK(msg_database_range(22, NULL, 1, 2, recs, 16) == MSG_DB_ERR_RANGE);
    CHECK(msg_database_range(22, "incoming", 1, 2, NULL, 16) == MSG_DB_ERR_RANGE);
    CHECK(msg_database_range(22, "incoming", 1, 2, recs, 0) == MSG_DB_ERR_RANGE);
    CHECK(msg_database_range(22, "incoming", -1, 2, recs, 16) == MSG_DB_ERR_RANGE);
    CHECK(msg_database_range(22, "incoming", 1, 1000, recs, 16) == MSG_DB_ERR_RANGE);
}

/* T6: after close the module refuses to pretend it can still persist. */
static void test_close(void)
{
    msg_database_close();
    CHECK(!msg_database_is_ready());
    CHECK(msg_database_add(1, 1, time(NULL), "pk", "incoming", "after close") == MSG_DB_ERR_NOT_READY);

    struct CHAT_MSG out[MAX_MSG_TOP];
    CHECK(msg_database_top(out, MAX_MSG_TOP) == MSG_DB_ERR_NOT_READY);

    int sn = 0;
    CHECK(msg_database_last_sn(1, "incoming", &sn) == MSG_DB_ERR_NOT_READY);

    /* Closing twice must be harmless. */
    msg_database_close();
    CHECK(!msg_database_is_ready());
}

int main(void)
{
    char template[] = "/tmp/toxrelayer_msgdb_XXXXXX";
    char *dir = mkdtemp(template);

    if (dir == NULL) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }

    if (chdir(dir) != 0) {
        perror("chdir");
        return EXIT_FAILURE;
    }

    test_open_creates_schema();
    test_argument_validation();
    test_round_trip();
    test_injection_is_neutralised();
    test_serial_number_clamping();
    test_range_replay();
    test_close();

    printf("test_msg_database: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}
