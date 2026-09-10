/*  test_reliability.c
 *
 *  End-to-end verification of the store-and-forward reliability loop.
 *
 *  The suite wires the real telegram codec, the real serial-number tracker and
 *  the real message database together and replays a complete loss-and-recovery
 *  cycle, with no network and no toxcore involved:
 *
 *      sender stores 6 telegrams
 *      -> telegram 3 is lost in transit
 *      -> receiver detects the gap and emits a control request
 *      -> sender decodes the request and looks the telegrams up in its database
 *      -> sender replays them
 *      -> receiver accepts the replay as "recovered" and the gap is closed
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_reliability \
 *         tests/test_reliability.c src/telegram.c src/msg_database.c -lsqlite3
 *      ./test_reliability
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/msg_database.h"
#include "../src/telegram.h"

static int tests_run = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        tests_run++;                                                            \
        if (!(expr)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);      \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

#define RX_KEY  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
#define TX_KEY  "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"

#define GAP_BUDGET  64
#define REPLAY_MAX  64
#define WIRE_MAX    4096

/* What the receiver managed to obtain, indexed by the serial number itself. */
#define RX_SLOTS 1000

static char   g_received[RX_SLOTS][128];
static int    g_received_count;

static void receiver_store(int sn, const char *body)
{
    if (sn >= 1 && sn < RX_SLOTS) {
        snprintf(g_received[sn], sizeof(g_received[0]), "%s", body);
        g_received_count++;
    }
}

/* Serve a replay request exactly as the relay does: look the stored bodies up
 * and rebuild each telegram with the verified encoder.
 *
 * @return the number of telegrams produced, or 0 when the request is refused. */
static int sender_serve_request(int friend_num, const char *request,
                                char out[][WIRE_MAX], int out_max)
{
    tg_message cmd;

    if (out == NULL || out_max <= 0) {
        return 0;
    }

    if (tg_decode(request, strlen(request), &cmd) != TG_CMD) {
        return 0;   /* not a control record */
    }

    if (strcmp(cmd.receiver, TX_KEY) != 0) {
        return 0;   /* not addressed to us: ignore, as the relay does */
    }

    struct CHAT_MSG_RECORD records[REPLAY_MAX];
    const int n = msg_database_range(friend_num, "outgoing", cmd.sn, cmd.sn_to, records, REPLAY_MAX);

    if (n <= 0) {
        return 0;
    }

    int produced = 0;

    for (int i = 0; i < n && produced < out_max; ++i) {
        const int len = tg_encode_text(out[produced], WIRE_MAX, RX_KEY, TX_KEY,
                                       records[i].msg_sn, (long) records[i].datetime,
                                       records[i].message);

        if (len <= 0) {
            continue;   /* unencodable record: skipped, never sent malformed */
        }

        produced++;
    }

    return produced;
}

/* ------------------------------------------------------------------------- */
/* Scenario 1: one telegram lost, detected, requested and recovered           */
/* ------------------------------------------------------------------------- */

static void test_gap_detection_and_replay(void)
{
    static const int sns[] = { 1, 2, 3, 4, 5, 6 };
    const int total = (int) (sizeof(sns) / sizeof(sns[0]));

    memset(g_received, 0, sizeof(g_received));
    g_received_count = 0;

    /* Sender originates six telegrams. What goes on the wire is produced by the
     * encoder; what is stored for a later replay is the body only. */
    char wire[8][WIRE_MAX];

    for (int i = 0; i < total; ++i) {
        char body[64];
        snprintf(body, sizeof(body), "telegram-%d", sns[i]);

        const int n = tg_encode_text(wire[i], sizeof(wire[i]), RX_KEY, TX_KEY,
                                     sns[i], 1700000000L + i, body);
        CHECK(n > 0);

        /* The wire record decodes back to exactly what was meant to be sent. */
        tg_message sent;
        CHECK(tg_decode(wire[i], (size_t) n, &sent) == TG_TEXT);
        CHECK(sent.sn == sns[i]);
        CHECK(strcmp(sent.body, body) == 0);

        CHECK(msg_database_add(1, sns[i], 1700000000L + i, TX_KEY, "outgoing", body) == MSG_DB_OK);
    }

    /* In transit, telegram 3 never arrives. */
    sn_tracker rx;
    sn_tracker_init(&rx, TG_SN_MIN);

    int gap_from = 0;
    int gap_to = 0;
    int gaps = 0;
    char request[512] = "";
    int request_len = 0;

    for (int i = 0; i < total; ++i) {
        if (sns[i] == 3) {
            continue;   /* lost */
        }

        int it_from = 0;
        int it_to = 0;
        const sn_result r = sn_tracker_accept(&rx, sns[i], GAP_BUDGET, &it_from, &it_to);
        CHECK(r == SN_IN_ORDER || r == SN_GAP);

        if (r == SN_GAP) {
            /* The out-parameters are only valid for the call that reported the
             * gap, so capture them here rather than reading them afterwards. */
            gaps++;
            gap_from = it_from;
            gap_to = it_to;
            request_len = tg_encode_cmd(request, sizeof(request), TX_KEY, RX_KEY, gap_from, gap_to);
            CHECK(request_len > 0);
        }

        char body[64];
        snprintf(body, sizeof(body), "telegram-%d", sns[i]);
        receiver_store(sns[i], body);
    }

    CHECK(gaps == 1);
    CHECK(gap_from == 3);
    CHECK(gap_to == 3);
    CHECK(g_received_count == 5);

    /* The sender serves the request from its database by re-encoding. */
    char replay[8][WIRE_MAX];
    CHECK(sender_serve_request(1, request, replay, 8) == 1);

    struct CHAT_MSG_RECORD rec[REPLAY_MAX];
    CHECK(msg_database_range(1, "outgoing", gap_from, gap_to, rec, REPLAY_MAX) == 1);
    CHECK(rec[0].msg_sn == 3);
    CHECK(strcmp(rec[0].message, "telegram-3") == 0);   /* bodies are stored */

    /* The replayed telegram is a complete, decodable record. */
    tg_message replayed;
    CHECK(tg_decode(replay[0], strlen(replay[0]), &replayed) == TG_TEXT);
    CHECK(replayed.sn == 3);
    CHECK(strcmp(replayed.receiver, RX_KEY) == 0);
    CHECK(strcmp(replayed.sender, TX_KEY) == 0);
    CHECK(strcmp(replayed.body, "telegram-3") == 0);

    /* The receiver accepts it as a recovery, not as a duplicate. */
    CHECK(sn_tracker_accept(&rx, replayed.sn, GAP_BUDGET, NULL, NULL) == SN_RECOVERED);
    receiver_store(replayed.sn, replayed.body);

    CHECK(g_received_count == 6);
    CHECK(rx.pending_count == 0);

    for (int i = 0; i < total; ++i) {
        char expect[64];
        snprintf(expect, sizeof(expect), "telegram-%d", sns[i]);
        CHECK(strcmp(g_received[sns[i]], expect) == 0);
    }

    /* A second copy of the replay is now a genuine duplicate. */
    CHECK(sn_tracker_accept(&rx, 3, GAP_BUDGET, NULL, NULL) == SN_DUPLICATE);
    CHECK(rx.expected == 7);
}

/* ------------------------------------------------------------------------- */
/* Scenario 2: the loss happens across the serial wrap point                  */
/* ------------------------------------------------------------------------- */

static void test_gap_across_wraparound(void)
{
    static const int sns[] = { 998, 999, 1, 2 };
    const int total = (int) (sizeof(sns) / sizeof(sns[0]));

    memset(g_received, 0, sizeof(g_received));
    g_received_count = 0;

    char wire[8][WIRE_MAX];

    for (int i = 0; i < total; ++i) {
        char body[64];
        snprintf(body, sizeof(body), "wrap-%d", sns[i]);

        const int n = tg_encode_text(wire[i], sizeof(wire[i]), RX_KEY, TX_KEY,
                                     sns[i], 1700005000L + i, body);
        CHECK(n > 0);
        CHECK(msg_database_add(2, sns[i], 1700005000L + i, TX_KEY, "outgoing", body) == MSG_DB_OK);
    }

    /* 999 is lost. */
    sn_tracker rx;
    sn_tracker_init(&rx, TG_SN_MIN);

    char request[512] = "";

    for (int i = 0; i < total; ++i) {
        if (sns[i] == 999) {
            continue;
        }

        int it_from = 0;
        int it_to = 0;
        const sn_result r = sn_tracker_accept(&rx, sns[i], GAP_BUDGET, &it_from, &it_to);
        CHECK(r == SN_IN_ORDER || r == SN_GAP);

        if (r == SN_GAP) {
            CHECK(it_from == 999);
            CHECK(it_to == 999);
            CHECK(tg_encode_cmd(request, sizeof(request), TX_KEY, RX_KEY, it_from, it_to) > 0);
        }

        char body[64];
        snprintf(body, sizeof(body), "wrap-%d", sns[i]);
        receiver_store(sns[i], body);
    }

    CHECK(g_received_count == 3);

    char replay[8][WIRE_MAX];
    CHECK(sender_serve_request(2, request, replay, 8) == 1);

    struct CHAT_MSG_RECORD rec[REPLAY_MAX];
    CHECK(msg_database_range(2, "outgoing", 999, 999, rec, REPLAY_MAX) == 1);

    tg_message replayed;
    CHECK(tg_decode(replay[0], strlen(replay[0]), &replayed) == TG_TEXT);
    CHECK(replayed.sn == 999);
    CHECK(strcmp(replayed.body, "wrap-999") == 0);

    CHECK(sn_tracker_accept(&rx, 999, GAP_BUDGET, NULL, NULL) == SN_RECOVERED);
    receiver_store(999, replayed.body);

    CHECK(g_received_count == 4);
    CHECK(strcmp(g_received[999], "wrap-999") == 0);
    CHECK(rx.pending_count == 0);
}

/* ------------------------------------------------------------------------- */
/* Scenario 3: requests that must be refused                                  */
/* ------------------------------------------------------------------------- */

static void test_request_rejection(void)
{
    char request[512];
    char out[4][WIRE_MAX];
    tg_message cmd;

    /* A request addressed to somebody else must not be served, even though the
     * serial range exists in our database. */
    CHECK(tg_encode_cmd(request, sizeof(request), "SOMEONE-ELSE", RX_KEY, 1, 6) > 0);
    CHECK(tg_decode(request, strlen(request), &cmd) == TG_CMD);
    CHECK(sender_serve_request(1, request, out, 4) == 0);

    /* A request addressed to us but for serials we never stored yields nothing. */
    CHECK(tg_encode_cmd(request, sizeof(request), TX_KEY, RX_KEY, 500, 510) > 0);
    CHECK(sender_serve_request(1, request, out, 4) == 0);

    /* A malformed control record is not served. */
    CHECK(sender_serve_request(1, "ZCZC CMD\ngarbage\n", out, 4) == 0);
    CHECK(sender_serve_request(1, "", out, 4) == 0);

    /* A data telegram is never mistaken for a control request. */
    char text[512];
    CHECK(tg_encode_text(text, sizeof(text), RX_KEY, TX_KEY, 1, 0L, "hello") > 0);
    CHECK(sender_serve_request(1, text, out, 4) == 0);

    /* A NULL output buffer and a zero capacity are refused. */
    CHECK(sender_serve_request(1, request, NULL, 4) == 0);
    CHECK(sender_serve_request(1, request, out, 0) == 0);
}

int main(void)
{
    char template[] = "/tmp/toxrelayer_reliability_XXXXXX";
    char *dir = mkdtemp(template);

    if (dir == NULL) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }

    if (chdir(dir) != 0) {
        perror("chdir");
        return EXIT_FAILURE;
    }

    if (msg_database_open() != MSG_DB_OK) {
        fprintf(stderr, "cannot open the message database\n");
        return EXIT_FAILURE;
    }

    test_gap_detection_and_replay();
    test_gap_across_wraparound();
    test_request_rejection();

    msg_database_close();

    printf("test_reliability: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}

