/*  test_telegram.c
 *
 *  Verification suite for the metaRelayer telegraph protocol module.
 *
 *  Covers the wire format (encode/decode round trips and malformed input), the
 *  wrap-around serial-number helpers, and the receive-sequence tracker including
 *  gap detection and resynchronisation.
 *
 *  A deterministic byte-level fuzz pass is included: buffers are allocated with
 *  the exact length so that building this suite with ASan turns any out-of-bounds
 *  read in the decoder into a hard failure.
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_telegram tests/test_telegram.c src/telegram.c
 *      ./test_telegram
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------------- */
/* Encoding and decoding round trips                                         */
/* ------------------------------------------------------------------------- */

/* T1: a TEXT telegram survives an encode/decode round trip unchanged. */
static void test_text_round_trip(void)
{
    char wire[4096];
    tg_message msg;

    const int n = tg_encode_text(wire, sizeof(wire), RX_KEY, TX_KEY, 42, 1700000000L, "hello world");
    CHECK(n > 0);

    /* The exact wire format is part of the protocol: the terminator follows the
     * body directly so that decode(encode(x)) == x. */
    CHECK(strncmp(wire, "ZCZC TEXT\n", 10) == 0);
    CHECK(strstr(wire, "0042 1700000000\n") != NULL);
    CHECK(strstr(wire, "hello worldNNNN\n") != NULL);

    CHECK(tg_decode(wire, (size_t) n, &msg) == TG_TEXT);
    CHECK(strcmp(msg.receiver, RX_KEY) == 0);
    CHECK(strcmp(msg.sender, TX_KEY) == 0);
    CHECK(msg.sn == 42);
    CHECK(msg.timestamp == 1700000000L);
    CHECK(strcmp(msg.body, "hello world") == 0);
}

/* T2: a multi-line body keeps its internal newlines. */
static void test_multiline_body(void)
{
    char wire[4096];
    tg_message msg;

    const char *body = "line one\nline two\nline three";
    const int n = tg_encode_text(wire, sizeof(wire), RX_KEY, TX_KEY, 7, 100L, body);
    CHECK(n > 0);

    CHECK(tg_decode(wire, (size_t) n, &msg) == TG_TEXT);
    CHECK(strcmp(msg.body, "line one\nline two\nline three") == 0);
}

/* T3: an empty body is legal. */
static void test_empty_body(void)
{
    char wire[4096];
    tg_message msg;

    const int n = tg_encode_text(wire, sizeof(wire), RX_KEY, TX_KEY, 1, 1L, "");
    CHECK(n > 0);
    CHECK(tg_decode(wire, (size_t) n, &msg) == TG_TEXT);
    CHECK(msg.body[0] == '\0');
}

/* T4: a CMD telegram carries the requested inclusive serial range. */
static void test_cmd_round_trip(void)
{
    char wire[4096];
    tg_message msg;

    const int n = tg_encode_cmd(wire, sizeof(wire), RX_KEY, TX_KEY, 5, 9);
    CHECK(n > 0);

    CHECK(strncmp(wire, "ZCZC CMD\n", 9) == 0);
    CHECK(strstr(wire, "R:5-9\n") != NULL);

    CHECK(tg_decode(wire, (size_t) n, &msg) == TG_CMD);
    CHECK(strcmp(msg.receiver, RX_KEY) == 0);
    CHECK(strcmp(msg.sender, TX_KEY) == 0);
    CHECK(msg.sn == 5);
    CHECK(msg.sn_to == 9);
}

/* T5: the encoder rejects input that would corrupt the framing. */
static void test_encoder_rejects_bad_input(void)
{
    char wire[4096];

    CHECK(tg_encode_text(NULL, sizeof(wire), RX_KEY, TX_KEY, 1, 0L, "x") == TG_ERR_ARG);
    CHECK(tg_encode_text(wire, 0, RX_KEY, TX_KEY, 1, 0L, "x") == TG_ERR_ARG);

    /* Invalid serial numbers. */
    CHECK(tg_encode_text(wire, sizeof(wire), RX_KEY, TX_KEY, 0, 0L, "x") == TG_ERR_RANGE);
    CHECK(tg_encode_text(wire, sizeof(wire), RX_KEY, TX_KEY, 1000, 0L, "x") == TG_ERR_RANGE);
    CHECK(tg_encode_cmd(wire, sizeof(wire), RX_KEY, TX_KEY, 0, 5) == TG_ERR_RANGE);
    CHECK(tg_encode_cmd(wire, sizeof(wire), RX_KEY, TX_KEY, 1, 1000) == TG_ERR_RANGE);

    /* A newline inside a key field would let one field forge the next. */
    CHECK(tg_encode_text(wire, sizeof(wire), "AA\nBB", TX_KEY, 1, 0L, "x") == TG_ERR_FORMAT);
    CHECK(tg_encode_text(wire, sizeof(wire), RX_KEY, "AA\rBB", 1, 0L, "x") == TG_ERR_FORMAT);

    /* Empty keys are not addressable. */
    CHECK(tg_encode_text(wire, sizeof(wire), "", TX_KEY, 1, 0L, "x") == TG_ERR_FORMAT);

    /* A body claiming the terminator would let the payload truncate itself. */
    CHECK(tg_encode_text(wire, sizeof(wire), RX_KEY, TX_KEY, 1, 0L, "hi\nNNNN\nbye") == TG_ERR_FORMAT);
    CHECK(tg_encode_text(wire, sizeof(wire), RX_KEY, TX_KEY, 1, 0L, "smuggledNNNN") == TG_ERR_FORMAT);

    /* Not enough room. */
    char tiny[16];
    CHECK(tg_encode_text(tiny, sizeof(tiny), RX_KEY, TX_KEY, 1, 0L, "body") == TG_ERR_TOOSMALL);
    CHECK(tiny[0] == '\0');   /* output is left empty on failure */
}

/* T6: an over-long key field is refused rather than silently truncated. */
static void test_encoder_field_limit(void)
{
    char long_key[TG_FIELD_MAX + 8];
    memset(long_key, 'A', sizeof(long_key));
    long_key[sizeof(long_key) - 1] = '\0';

    char wire[4096];
    CHECK(tg_encode_text(wire, sizeof(wire), long_key, TX_KEY, 1, 0L, "x") == TG_ERR_FORMAT);
}

/* ------------------------------------------------------------------------- */
/* Decoder robustness                                                        */
/* ------------------------------------------------------------------------- */

static tg_kind dec(const char *s, tg_message *m)
{
    return tg_decode(s, strlen(s), m);
}

/* T7: malformed input is rejected, never accepted or read past its length. */
static void test_decoder_rejects_malformed(void)
{
    tg_message msg;

    CHECK(tg_decode(NULL, 10, &msg) == TG_INVALID);
    CHECK(tg_decode("x", 1, NULL) == TG_INVALID);
    CHECK(tg_decode("x", 0, &msg) == TG_INVALID);
    CHECK(tg_decode("", 0, &msg) == TG_INVALID);

    /* Wrong or unknown record type. */
    CHECK(dec("ZCZC FOO\nA\nB\n0001 1\nhi\nNNNN\n", &msg) == TG_INVALID);
    CHECK(dec("GARBAGE\n", &msg) == TG_INVALID);

    /* Truncated: no terminator. */
    CHECK(dec("ZCZC TEXT\nA\nB\n0001 1\nhi\n", &msg) == TG_INVALID);

    /* Missing fields. */
    CHECK(dec("ZCZC TEXT\nA\n", &msg) == TG_INVALID);

    /* Bad metadata. */
    CHECK(dec("ZCZC TEXT\nA\nB\nnotanumber 1\nhi\nNNNN\n", &msg) == TG_INVALID);
    CHECK(dec("ZCZC TEXT\nA\nB\n0000 1\nhi\nNNNN\n", &msg) == TG_INVALID);
    CHECK(dec("ZCZC TEXT\nA\nB\n0001\nhi\nNNNN\n", &msg) == TG_INVALID);
    CHECK(dec("ZCZC CMD\nA\nB\n5-9\nNNNN\n", &msg) == TG_INVALID);
    CHECK(dec("ZCZC CMD\nA\nB\nR:0-9\nNNNN\n", &msg) == TG_INVALID);
    CHECK(dec("ZCZC CMD\nA\nB\nR:1-1000\nNNNN\n", &msg) == TG_INVALID);

    /* Empty key field. */
    CHECK(dec("ZCZC TEXT\n\nB\n0001 1\nhi\nNNNN\n", &msg) == TG_INVALID);
    CHECK(dec("ZCZC TEXT\nA\n\n0001 1\nhi\nNNNN\n", &msg) == TG_INVALID);
}

/* T8: decoding must not require a NUL terminator and must obey `len`. */
static void test_decoder_needs_no_terminator(void)
{
    tg_message msg;

    char wire[4096];
    const int n = tg_encode_text(wire, sizeof(wire), RX_KEY, TX_KEY, 3, 55L, "bounded");
    CHECK(n > 0);

    /* The final newline after the terminator is optional. */
    CHECK(tg_decode(wire, (size_t) n - 1, &msg) == TG_TEXT);
    CHECK(strcmp(msg.body, "bounded") == 0);

    /* Removing the terminator itself must be rejected: the record is incomplete. */
    CHECK(tg_decode(wire, (size_t) n - 5, &msg) == TG_INVALID);

    /* The trailing newline of the framing is omitted here, so the decoder must
     * not rely on the buffer being NUL terminated. */
    for (int i = 0; i < n; ++i) {
        CHECK(tg_decode(wire, (size_t) i, &msg) == TG_INVALID || msg.sn == 3);
    }
}

/* T9: CRLF line endings are accepted, and line separators are normalised to a
 *     single LF. When the sender puts the terminator on its own line the body
 *     therefore carries one trailing separator, which is the documented
 *     contract: the body is the bytes between the metadata line and the
 *     terminator. */
static void test_decoder_accepts_crlf(void)
{
    static const char wire[] =
        "ZCZC TEXT\r\n" RX_KEY "\r\n" TX_KEY "\r\n0009 77\r\ncrlf body\r\nNNNN\r\n";
    tg_message msg;

    CHECK(tg_decode(wire, sizeof(wire), &msg) == TG_TEXT);
    CHECK(msg.sn == 9);
    CHECK(msg.timestamp == 77L);
    CHECK(strcmp(msg.body, "crlf body\n") == 0);

    /* The same telegram with the terminator appended directly to the body keeps
     * the body byte for byte, which is what our own encoder produces. */
    static const char wire2[] =
        "ZCZC TEXT\r\n" RX_KEY "\r\n" TX_KEY "\r\n0009 77\r\ncrlf bodyNNNN\r\n";

    CHECK(tg_decode(wire2, sizeof(wire2), &msg) == TG_TEXT);
    CHECK(strcmp(msg.body, "crlf body") == 0);
}

/* T10: telegrams written by earlier builds, where the body is not followed by a
 *      newline before the terminator, are still accepted. */
static void test_decoder_accepts_legacy_framing(void)
{
    static const char wire[] =
        "ZCZC TEXT\n" RX_KEY "\n" TX_KEY "\n0011 99\nlegacy bodyNNNN\n";
    tg_message msg;

    CHECK(tg_decode(wire, sizeof(wire) - 1, &msg) == TG_TEXT);
    CHECK(msg.sn == 11);
    CHECK(strcmp(msg.body, "legacy body") == 0);
}

/* T11: an over-long body is rejected instead of being silently truncated. */
static void test_decoder_rejects_oversized_body(void)
{
    char wire[8192];
    int pos = 0;

    pos += snprintf(wire + pos, sizeof(wire) - (size_t) pos,
                    "ZCZC TEXT\n%s\n%s\n0001 1\n", RX_KEY, TX_KEY);

    while (pos < (int) sizeof(wire) - 8) {
        wire[pos++] = 'A';
        wire[pos++] = '\n';
    }

    pos += snprintf(wire + pos, sizeof(wire) - (size_t) pos, "NNNN\n");

    tg_message msg;
    CHECK(tg_decode(wire, (size_t) pos, &msg) == TG_INVALID);
}

/* ------------------------------------------------------------------------- */
/* Serial-number helpers                                                     */
/* ------------------------------------------------------------------------- */

/* T12: validation and wrap-around of the 1..999 serial space. */
static void test_sn_helpers(void)
{
    CHECK(!tg_sn_valid(0));
    CHECK(tg_sn_valid(1));
    CHECK(tg_sn_valid(999));
    CHECK(!tg_sn_valid(1000));
    CHECK(!tg_sn_valid(-1));

    CHECK(tg_sn_next(1) == 2);
    CHECK(tg_sn_next(998) == 999);
    CHECK(tg_sn_next(999) == 1);        /* wraps */
    CHECK(tg_sn_prev(2) == 1);
    CHECK(tg_sn_prev(999) == 998);
    CHECK(tg_sn_prev(1) == 999);        /* wraps */

    /* Distance is always in [0, 998] and is zero only for equal serials. */
    CHECK(tg_sn_distance(1, 1) == 0);
    CHECK(tg_sn_distance(1, 2) == 1);
    CHECK(tg_sn_distance(1, 999) == 998);   /* one step backwards */
    CHECK(tg_sn_distance(999, 1) == 1);
    CHECK(tg_sn_distance(998, 1) == 2);

    for (int a = TG_SN_MIN; a <= TG_SN_MAX; a += 37) {
        for (int b = TG_SN_MIN; b <= TG_SN_MAX; b += 41) {
            const int d = tg_sn_distance(a, b);

            CHECK(d >= 0 && d < TG_SN_RANGE);

            /* Stepping forward `d` times must land on b. */
            int x = a;

            for (int i = 0; i < d; ++i) {
                x = tg_sn_next(x);
            }

            CHECK(x == b);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Receive sequence tracker                                                  */
/* ------------------------------------------------------------------------- */

/* T13: the first serial synchronises the tracker. */
static void test_tracker_sync(void)
{
    sn_tracker t;

    sn_tracker_init(&t, 1);
    CHECK(t.expected == 1);
    CHECK(!t.synced);

    /* Whatever arrives first is accepted and sets the expectation. */
    CHECK(sn_tracker_accept(&t, 500, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(t.synced);
    CHECK(t.expected == 501);

    /* A second tracker can be primed with a recovered value. */
    sn_tracker_init(&t, 42);
    CHECK(t.expected == 42);

    /* An invalid starting point falls back to the minimum. */
    sn_tracker_init(&t, 0);
    CHECK(t.expected == TG_SN_MIN);
}

/* T14: in-order arrivals advance the expectation, repeats are detected. */
static void test_tracker_in_order_and_duplicate(void)
{
    sn_tracker t;
    sn_tracker_init(&t, 10);

    CHECK(sn_tracker_accept(&t, 10, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(sn_tracker_accept(&t, 11, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(sn_tracker_accept(&t, 12, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(t.expected == 13);

    /* Replays must not move the expectation backwards. */
    CHECK(sn_tracker_accept(&t, 12, 100, NULL, NULL) == SN_DUPLICATE);
    CHECK(sn_tracker_accept(&t, 10, 100, NULL, NULL) == SN_DUPLICATE);
    CHECK(t.expected == 13);

    /* The expected serial is accepted again once it is reached. */
    CHECK(sn_tracker_accept(&t, 13, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(t.expected == 14);
}

/* T15: a gap reports the exact inclusive range that must be requested. */
static void test_tracker_gap_range(void)
{
    sn_tracker t;
    int from = 0;
    int to = 0;

    sn_tracker_init(&t, 20);
    CHECK(sn_tracker_accept(&t, 20, 100, NULL, NULL) == SN_IN_ORDER);

    /* 21, 22 and 23 are missing when 24 arrives. */
    CHECK(sn_tracker_accept(&t, 24, 100, &from, &to) == SN_GAP);
    CHECK(from == 21);
    CHECK(to == 23);
    CHECK(t.expected == 25);

    /* A single missing telegram. */
    CHECK(sn_tracker_accept(&t, 26, 100, &from, &to) == SN_GAP);
    CHECK(from == 25);
    CHECK(to == 25);
    CHECK(t.expected == 27);
}

/* T16: an unrecoverably large gap resynchronises instead of requesting a huge
 *      replay. */
static void test_tracker_resync_on_large_gap(void)
{
    sn_tracker t;
    int from = -1;
    int to = -1;

    sn_tracker_init(&t, 1);
    CHECK(sn_tracker_accept(&t, 1, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(t.expected == 2);

    /* 400 missing telegrams exceeds the recovery budget of 100. */
    CHECK(sn_tracker_accept(&t, 402, 100, &from, &to) == SN_RESYNC);
    CHECK(from == 0);
    CHECK(to == 0);
    CHECK(t.expected == 403);

    /* Exactly at the budget the gap is still recoverable. */
    CHECK(sn_tracker_accept(&t, 503, 100, &from, &to) == SN_GAP);
    CHECK(from == 403);
    CHECK(to == 502);
}

/* T17: gaps and duplicates are classified correctly across the wrap point. */
static void test_tracker_wraparound(void)
{
    sn_tracker t;
    int from = 0;
    int to = 0;

    sn_tracker_init(&t, 997);
    CHECK(sn_tracker_accept(&t, 997, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(sn_tracker_accept(&t, 998, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(sn_tracker_accept(&t, 999, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(sn_tracker_accept(&t, 1, 100, NULL, NULL) == SN_IN_ORDER);   /* wraps */
    CHECK(sn_tracker_accept(&t, 2, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(t.expected == 3);

    /* A serial from before the wrap is a replay, not a gap. */
    CHECK(sn_tracker_accept(&t, 999, 100, NULL, NULL) == SN_DUPLICATE);

    /* A gap that itself crosses the wrap point. */
    sn_tracker_init(&t, 998);
    CHECK(sn_tracker_accept(&t, 998, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(sn_tracker_accept(&t, 2, 100, &from, &to) == SN_GAP);
    CHECK(from == 999);
    CHECK(to == 1);

    /* A tracker that has just wrapped must still reject old telegrams. */
    sn_tracker_init(&t, 1);
    CHECK(sn_tracker_accept(&t, 1, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(sn_tracker_accept(&t, 999, 100, NULL, NULL) == SN_DUPLICATE);
}

/* T18: invalid arguments never advance the tracker. */
static void test_tracker_invalid_arguments(void)
{
    sn_tracker t;
    sn_tracker_init(&t, 5);

    CHECK(sn_tracker_accept(&t, 5, 100, NULL, NULL) == SN_IN_ORDER);
    CHECK(t.expected == 6);

    CHECK(sn_tracker_accept(&t, 0, 100, NULL, NULL) == SN_RESYNC);
    CHECK(sn_tracker_accept(&t, 1000, 100, NULL, NULL) == SN_RESYNC);
    CHECK(t.expected == 6);   /* unchanged by invalid input */

    CHECK(sn_tracker_accept(NULL, 6, 100, NULL, NULL) == SN_RESYNC);

    /* NULL gap pointers are tolerated. */
    CHECK(sn_tracker_accept(&t, 6, 100, NULL, NULL) == SN_IN_ORDER);

    /* A zero recovery budget still recovers a single missing telegram. */
    sn_tracker_init(&t, 7);
    CHECK(sn_tracker_accept(&t, 7, 0, NULL, NULL) == SN_IN_ORDER);
    CHECK(sn_tracker_accept(&t, 9, 0, NULL, NULL) == SN_RESYNC);   /* 8 missing */
}

/* T20: a telegram that was reported missing and later replayed must be accepted
 *      as SN_RECOVERED, not mistaken for a duplicate. This is the property that
 *      makes gap recovery actually close the gap. */
static void test_tracker_replay_closes_gap(void)
{
    sn_tracker t;
    int from = 0;
    int to = 0;

    sn_tracker_init(&t, 1);
    CHECK(sn_tracker_accept(&t, 1, 64, NULL, NULL) == SN_IN_ORDER);
    CHECK(sn_tracker_accept(&t, 2, 64, NULL, NULL) == SN_IN_ORDER);

    /* 3 and 4 are lost; 5 arrives. */
    CHECK(sn_tracker_accept(&t, 5, 64, &from, &to) == SN_GAP);
    CHECK(from == 3);
    CHECK(to == 4);
    CHECK(t.expected == 6);
    CHECK(t.pending_count == 2);

    /* The peer replays them. Both must be accepted even though they sit behind
     * the expectation. */
    CHECK(sn_tracker_accept(&t, 3, 64, NULL, NULL) == SN_RECOVERED);
    CHECK(sn_tracker_accept(&t, 4, 64, NULL, NULL) == SN_RECOVERED);
    CHECK(t.pending_count == 0);
    CHECK(t.expected == 6);   /* recovery does not move the expectation */

    /* A second copy of a recovered telegram is a genuine duplicate again. */
    CHECK(sn_tracker_accept(&t, 3, 64, NULL, NULL) == SN_DUPLICATE);

    /* Traffic continues normally. */
    CHECK(sn_tracker_accept(&t, 6, 64, NULL, NULL) == SN_IN_ORDER);
    CHECK(t.expected == 7);
}

/* T21: the outstanding set is bounded; exceeding it resynchronises instead of
 *      overflowing the array. */
static void test_tracker_pending_bound(void)
{
    sn_tracker t;
    sn_tracker_init(&t, 1);
    CHECK(sn_tracker_accept(&t, 1, 1000, NULL, NULL) == SN_IN_ORDER);
    CHECK(t.expected == 2);

    /* 298 missing telegrams fit the caller's budget but not SN_MAX_PENDING, so
     * the tracker must resynchronise rather than overflow. */
    CHECK(sn_tracker_accept(&t, 300, 1000, NULL, NULL) == SN_RESYNC);
    CHECK(t.pending_count == 0);
    CHECK(t.expected == 301);

    /* A gap that does fit the set is still tracked, and every serial in it is
     * recognised when replayed. */
    sn_tracker u;
    sn_tracker_init(&u, 1);
    CHECK(sn_tracker_accept(&u, 1, SN_MAX_PENDING, NULL, NULL) == SN_IN_ORDER);

    const int target = 1 + SN_MAX_PENDING;   /* SN_MAX_PENDING missing serials */

    CHECK(sn_tracker_accept(&u, target, SN_MAX_PENDING, NULL, NULL) == SN_GAP);
    CHECK(u.pending_count == SN_MAX_PENDING - 1);   /* serials 2..target-1 */

    for (int sn = 2; sn < target; ++sn) {
        CHECK(sn_tracker_accept(&u, sn, SN_MAX_PENDING, NULL, NULL) == SN_RECOVERED);
    }

    CHECK(u.pending_count == 0);
}

/* ------------------------------------------------------------------------- */
/* Byte-level fuzzing                                                        */
/* ------------------------------------------------------------------------- */
static unsigned int g_rng = 0x12345u;

static unsigned int rng_next(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 16) & 0x7fffu;
}

/* T19: mutated and random bytes must never crash the decoder, never make it read
 *      past the supplied length, and never yield a partially filled message.
 *
 * Inputs are derived from valid telegrams with random mutations, which reaches
 * far deeper into the decoder than pure noise, plus pure-random buffers. The
 * buffer is heap allocated with exactly `len` bytes, so building this suite with
 * AddressSanitizer turns any over-read into a hard failure. */
static void test_decoder_fuzz(void)
{
    char base[4096];
    int accepted = 0;

    for (int iter = 0; iter < 20000; ++iter) {
        int blen;

        if (rng_next() & 1u) {
            const int sn = 1 + (int) (rng_next() % (unsigned) TG_SN_MAX);
            blen = tg_encode_text(base, sizeof(base), RX_KEY, TX_KEY, sn,
                                  1700000000L, "fuzz payload\nsecond line");
        } else {
            blen = tg_encode_cmd(base, sizeof(base), RX_KEY, TX_KEY, 1, 2);
        }

        CHECK(blen > 0);

        size_t len = (size_t) blen;
        char *buf = malloc(len);

        CHECK(buf != NULL);
        memcpy(buf, base, len);

        if (iter % 5 == 4) {
            /* Pure noise: must never be accepted as a well formed telegram. */
            for (size_t i = 0; i < len; ++i) {
                buf[i] = (char) (rng_next() & 0xff);
            }
        } else {
            /* A few random mutations of a valid record. */
            const int muts = (int) (rng_next() % 4);

            for (int m = 0; m < muts && len > 0; ++m) {
                switch (rng_next() % 4u) {
                case 0:
                    buf[rng_next() % len] = (char) (rng_next() & 0xff);
                    break;
                case 1:
                    buf[rng_next() % len] = '\n';
                    break;
                case 2:
                    buf[rng_next() % len] = '\0';
                    break;
                default:
                    len = 1 + (rng_next() % len);   /* truncate */
                    break;
                }
            }
        }

        tg_message msg;
        const tg_kind kind = tg_decode(buf, len, &msg);

        CHECK(kind == TG_INVALID || kind == TG_TEXT || kind == TG_CMD);

        if (kind != TG_INVALID) {
            /* Any accepted telegram must be fully formed. */
            accepted++;
            CHECK(msg.kind == kind);
            CHECK(msg.receiver[0] != '\0');
            CHECK(msg.sender[0] != '\0');
            CHECK(tg_sn_valid(msg.sn));
            CHECK(memchr(msg.receiver, '\0', sizeof(msg.receiver)) != NULL);
            CHECK(memchr(msg.sender, '\0', sizeof(msg.sender)) != NULL);
            CHECK(memchr(msg.body, '\0', sizeof(msg.body)) != NULL);
        }

        free(buf);
    }

    /* An unmutated telegram is among the inputs, so acceptance must happen;
     * otherwise the assertions above would be vacuous. */
    CHECK(accepted > 0);
}

int main(void)
{
    test_text_round_trip();
    test_multiline_body();
    test_empty_body();
    test_cmd_round_trip();
    test_encoder_rejects_bad_input();
    test_encoder_field_limit();
    test_decoder_rejects_malformed();
    test_decoder_needs_no_terminator();
    test_decoder_accepts_crlf();
    test_decoder_accepts_legacy_framing();
    test_decoder_rejects_oversized_body();
    test_sn_helpers();
    test_tracker_sync();
    test_tracker_in_order_and_duplicate();
    test_tracker_gap_range();
    test_tracker_resync_on_large_gap();
    test_tracker_wraparound();
    test_tracker_invalid_arguments();
    test_tracker_replay_closes_gap();
    test_tracker_pending_bound();
    test_decoder_fuzz();

    printf("test_telegram: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}



