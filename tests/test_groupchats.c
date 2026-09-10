/*  test_groupchats.c
 *
 *  Verification suite for the group-chat table (groupchats.c).
 *
 *  The table management functions (group_add / group_leave / group_index /
 *  realloc_groupchats) operate purely on the global Tox_Bot state and do not
 *  need a live Tox instance, so they are unit tested here. The two functions
 *  that walk toxcore conference state are covered by the integration tier.
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_groupchats \
 *         tests/test_groupchats.c src/groupchats.c src/misc.c src/log.c \
 *         $(pkg-config --cflags --libs toxcore)
 *      ./test_groupchats
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/toxrelayer.h"
#include "../src/groupchats.h"

/* Defined in toxrelayer.c; the module under test references it. */
struct Tox_Bot Tox_Bot;

static int tests_run = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        tests_run++;                                                            \
        if (!(expr)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);      \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

/* Return the table to the empty state between tests. */
static void reset_table(void)
{
    free(Tox_Bot.g_chats);
    Tox_Bot.g_chats = NULL;
    Tox_Bot.chats_idx = 0;
}

/* ------------------------------------------------------------------------- */
/* group_index                                                               */
/* ------------------------------------------------------------------------- */

/* T1: an empty table contains nothing and is never dereferenced. */
static void test_index_empty(void)
{
    reset_table();

    CHECK(group_index(0) == -1);
    CHECK(group_index(1) == -1);
    CHECK(group_index(0xFFFFFFFFu) == -1);
}

/* T2: index lookup finds the right entry and ignores inactive ones. */
static void test_index_lookup(void)
{
    reset_table();

    CHECK(group_add(10, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_add(20, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_add(30, TOX_CONFERENCE_TYPE_AV, NULL) == 0);
    CHECK(Tox_Bot.chats_idx == 3);

    CHECK(group_index(10) == 0);
    CHECK(group_index(20) == 1);
    CHECK(group_index(30) == 2);
    CHECK(group_index(40) == -1);

    /* Removing the middle entry makes it unfindable while the others keep their
     * positions: group_leave() clears the slot but does not shift the array, it
     * only truncates inactive entries at the tail. */
    group_leave(20);
    CHECK(group_index(10) == 0);
    CHECK(group_index(20) == -1);
    CHECK(group_index(30) == 2);
    CHECK(Tox_Bot.chats_idx == 3);

    /* The hole is reused by the next group added. */
    CHECK(group_add(40, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_index(40) == 1);
    CHECK(Tox_Bot.chats_idx == 3);

    reset_table();
}

/* ------------------------------------------------------------------------- */
/* group_add                                                                 */
/* ------------------------------------------------------------------------- */

/* T3: a new group records its number, type and password state. */
static void test_group_add_attributes(void)
{
    reset_table();

    CHECK(group_add(7, TOX_CONFERENCE_TYPE_AV, NULL) == 0);
    CHECK(Tox_Bot.g_chats[0].active == true);
    CHECK(Tox_Bot.g_chats[0].groupnum == 7);
    CHECK(Tox_Bot.g_chats[0].type == TOX_CONFERENCE_TYPE_AV);
    CHECK(Tox_Bot.g_chats[0].has_pass == false);
    CHECK(Tox_Bot.g_chats[0].password[0] == '\0');

    /* A password enables the flag and is copied in. */
    CHECK(group_add(8, TOX_CONFERENCE_TYPE_TEXT, "s3cret") == 0);
    CHECK(Tox_Bot.g_chats[1].has_pass == true);
    CHECK(strcmp(Tox_Bot.g_chats[1].password, "s3cret") == 0);

    /* An over-long password is truncated by snprintf, never overflowing. */
    char long_pass[MAX_PASSWORD_SIZE + 32];
    memset(long_pass, 'p', sizeof(long_pass) - 1);
    long_pass[sizeof(long_pass) - 1] = '\0';

    CHECK(group_add(9, TOX_CONFERENCE_TYPE_TEXT, long_pass) == 0);
    CHECK(Tox_Bot.g_chats[2].has_pass == true);
    CHECK(strlen(Tox_Bot.g_chats[2].password) < MAX_PASSWORD_SIZE);

    reset_table();
}

/* T4: a slot freed by group_leave is reused by the next group_add. */
static void test_group_add_reuses_slot(void)
{
    reset_table();

    CHECK(group_add(1, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_add(2, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_add(3, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(Tox_Bot.chats_idx == 3);

    group_leave(2);
    CHECK(Tox_Bot.chats_idx == 3);      /* the slot is a hole; the count is unchanged */
    CHECK(group_index(2) == -1);

    /* The freed slot is taken by the next group. */
    CHECK(group_add(4, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(Tox_Bot.chats_idx == 3);
    CHECK(group_index(4) == 1);
    CHECK(group_index(2) == -1);

    reset_table();
}

/* T5: the table records duplicate numbers as distinct rows rather than
 *     corrupting itself; the caller is responsible for avoiding them. */
static void test_group_add_duplicate(void)
{
    reset_table();

    CHECK(group_add(5, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_add(5, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);

    CHECK(Tox_Bot.chats_idx == 2);
    CHECK(group_index(5) == 0);         /* the first row wins the lookup */

    reset_table();
}

/* T6: the table refuses to grow past MAX_NUM_GROUPS. */
static void test_group_add_capacity(void)
{
    reset_table();

    for (uint32_t i = 0; i < MAX_NUM_GROUPS; ++i) {
        CHECK(group_add(i + 1, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    }

    CHECK(Tox_Bot.chats_idx == MAX_NUM_GROUPS);
    CHECK(group_add(9999, TOX_CONFERENCE_TYPE_TEXT, NULL) == -1);

    reset_table();
}

/* ------------------------------------------------------------------------- */
/* group_leave                                                               */
/* ------------------------------------------------------------------------- */

/* T7: leaving an unknown group is harmless. */
static void test_group_leave_unknown(void)
{
    reset_table();

    group_leave(0);
    CHECK(Tox_Bot.chats_idx == 0);

    CHECK(group_add(1, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    group_leave(123456);
    CHECK(Tox_Bot.chats_idx == 1);      /* untouched */
    CHECK(group_index(1) == 0);

    reset_table();
}

/* T8: emptying the table releases the storage. */
static void test_group_leave_all(void)
{
    reset_table();

    CHECK(group_add(1, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_add(2, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);

    group_leave(1);
    group_leave(2);

    CHECK(Tox_Bot.chats_idx == 0);
    CHECK(Tox_Bot.g_chats == NULL);

    /* Leaving again remains harmless. */
    group_leave(1);
    CHECK(Tox_Bot.chats_idx == 0);

    reset_table();
}

/* T9: leaving a tail entry compacts the used count. */
static void test_group_leave_compaction(void)
{
    reset_table();

    CHECK(group_add(1, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_add(2, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_add(3, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);

    group_leave(3);                     /* the last entry */
    CHECK(Tox_Bot.chats_idx == 2);

    /* A group added afterwards is still found. */
    CHECK(group_add(4, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(group_index(4) != -1);

    reset_table();
}

/* ------------------------------------------------------------------------- */
/* realloc_groupchats                                                        */
/* ------------------------------------------------------------------------- */

/* T10: a non-positive size frees the table and clears the pointer, and the
 *      table can then be grown again from scratch. */
static void test_realloc_non_positive(void)
{
    reset_table();

    CHECK(group_add(1, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(Tox_Bot.g_chats != NULL);

    realloc_groupchats(0);
    CHECK(Tox_Bot.g_chats == NULL);

    /* Adding re-allocates rather than failing: chats_idx still describes how
     * many slots are in use, so the storage is simply rebuilt. */
    CHECK(group_add(2, TOX_CONFERENCE_TYPE_TEXT, NULL) == 0);
    CHECK(Tox_Bot.g_chats != NULL);
    CHECK(group_index(2) != -1);

    reset_table();

    /* With the table already released, a negative size stays harmless. */
    realloc_groupchats(-5);
    CHECK(Tox_Bot.g_chats == NULL);

    reset_table();
}

/* T11: growing the table preserves the existing entries. */
static void test_realloc_preserves(void)
{
    reset_table();

    CHECK(group_add(11, TOX_CONFERENCE_TYPE_TEXT, "pw") == 0);
    CHECK(group_add(22, TOX_CONFERENCE_TYPE_AV, NULL) == 0);

    realloc_groupchats(64);
    CHECK(Tox_Bot.g_chats != NULL);

    CHECK(Tox_Bot.g_chats[0].groupnum == 11);
    CHECK(strcmp(Tox_Bot.g_chats[0].password, "pw") == 0);
    CHECK(Tox_Bot.g_chats[1].groupnum == 22);
    CHECK(Tox_Bot.g_chats[1].type == TOX_CONFERENCE_TYPE_AV);

    reset_table();
}

int main(void)
{
    Tox_Bot.chats_idx = 0;
    Tox_Bot.g_chats = NULL;

    test_index_empty();
    test_index_lookup();
    test_group_add_attributes();
    test_group_add_reuses_slot();
    test_group_add_duplicate();
    test_group_add_capacity();
    test_group_leave_unknown();
    test_group_leave_all();
    test_group_leave_compaction();
    test_realloc_non_positive();
    test_realloc_preserves();

    reset_table();

    printf("test_groupchats: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}
