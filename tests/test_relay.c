/*  test_relay.c
 *
 *  Verification suite for the relay policy (relay.c).
 *
 *  The relay module is wired to the *real* telegram codec, the real delivery
 *  queue and the real message database; only the peer directory and the toxcore
 *  facing glue are faked. That makes this a genuine test of the store-and-forward
 *  policy: what is stored, what is queued, and what is sent back on a replay
 *  request are all observable.
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_relay \
 *         tests/test_relay.c src/relay.c src/telegram.c src/msg_queue.c \
 *         src/msg_database.c $(pkg-config --cflags --libs sqlite3)
 *      ./test_relay
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/relay.h"
#include "../src/msg_queue.h"
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

#define SELF_KEY "0000000000000000000000000000000000000000000000000000000000000000"

/* Each channel gets its own generated peer key so that tests can use a fresh
 * channel and stay independent of the rows earlier tests wrote to the database
 * (which is keyed by channel). */
#define CHANNELS 16

/* Named channels used by the tests. */
#define CH_A 0
#define CH_B 1
#define CH_C 2
#define CH_D 3
#define CH_E 4
#define CH_F 5
#define CH_G 6
#define CH_H 7
#define CH_I 8
#define CH_J 9
#define CH_K 10
#define CH_L 11
#define CH_M 12
#define CH_N 13

#define NOW ((time_t) 1700000000)

/* ------------------------------------------------------------------------- */
/* Fake environment                                                          */
/* ------------------------------------------------------------------------- */

static char g_keys[CHANNELS][65];

typedef struct {
    const char *keys[CHANNELS];     /* public key per channel; NULL = absent   */
    bool        metacom[CHANNELS];
    sn_tracker  tracker[CHANNELS];

    /* Observation of what the relay asked for. */
    char   sent[64][RELAY_BODY_MAX + 256];
    size_t sent_len[64];
    int    sent_channel[64];
    int    sent_count;

    int    serial_mirror[CHANNELS]; /* last serial passed to remember_serial   */
} fake_env;

static int fake_resolve_peer(void *ctx, const char *key)
{
    fake_env *f = ctx;

    for (int i = 0; i < CHANNELS; ++i) {
        if (f->keys[i] != NULL && strcmp(f->keys[i], key) == 0) {
            return i;
        }
    }

    return -1;
}

static const char *fake_peer_key(void *ctx, int channel)
{
    fake_env *f = ctx;

    if (channel < 0 || channel >= CHANNELS) {
        return NULL;
    }

    return f->keys[channel];
}

static bool fake_peer_is_metacom(void *ctx, int channel)
{
    fake_env *f = ctx;

    if (channel < 0 || channel >= CHANNELS) {
        return false;
    }

    return f->metacom[channel];
}

static sn_tracker *fake_tracker(void *ctx, int channel)
{
    fake_env *f = ctx;

    if (channel < 0 || channel >= CHANNELS) {
        return NULL;
    }

    return &f->tracker[channel];
}

static int fake_store(void *ctx, int channel, int sn, long timestamp,
                      const char *sender, const char *body)
{
    (void) ctx;
    return msg_database_add(channel, sn, (time_t) timestamp, sender, "incoming", body);
}

static int fake_enqueue(void *ctx, int channel, const char *payload, size_t len, time_t now)
{
    fake_env *f = ctx;

    if (f->sent_count >= 64 || len > sizeof(f->sent[0]) - 1) {
        return -1;
    }

    memcpy(f->sent[f->sent_count], payload, len);
    f->sent[f->sent_count][len] = '\0';
    f->sent_len[f->sent_count] = len;
    f->sent_channel[f->sent_count] = channel;
    f->sent_count++;

    /* Also drive the real queue so its policy is part of the test. */
    return (mq_enqueue(channel, payload, len, now) == MQ_OK) ? 0 : -1;
}

static int fake_fetch_range(void *ctx, int channel, int from, int to,
                            relay_record *out, int max)
{
    (void) ctx;

    struct CHAT_MSG_RECORD records[RELAY_MAX_REPLAY];
    const int n = msg_database_range(channel, "outgoing", from, to, records, max);

    if (n <= 0) {
        return n;
    }

    for (int i = 0; i < n; ++i) {
        out[i].sn = records[i].msg_sn;
        out[i].timestamp = (long) records[i].datetime;
        snprintf(out[i].body, sizeof(out[i].body), "%s", records[i].message);
    }

    return n;
}

static void fake_remember_serial(void *ctx, int channel, int sn)
{
    fake_env *f = ctx;

    if (channel >= 0 && channel < CHANNELS) {
        f->serial_mirror[channel] = sn;
    }
}

static void fake_reset(fake_env *f)
{
    memset(f, 0, sizeof(*f));

    for (int i = 0; i < CHANNELS; ++i) {
        /* A distinct, well formed peer key per channel: printable, 64 bytes and
         * never equal to SELF_KEY. */
        snprintf(g_keys[i], sizeof(g_keys[i]), "a%063d", i);
        f->keys[i] = g_keys[i];

        f->metacom[i] = ((i % 2) == 0);     /* even channels are metaCom peers */

        sn_tracker_init(&f->tracker[i], TG_SN_MIN);
        f->serial_mirror[i] = -1;
    }
}

static void wire_env(relay_env *env, fake_env *f)
{
    memset(env, 0, sizeof(*env));

    env->self_public_key = SELF_KEY;
    env->resolve_peer = fake_resolve_peer;
    env->peer_key = fake_peer_key;
    env->peer_is_metacom = fake_peer_is_metacom;
    env->tracker = fake_tracker;
    env->store = fake_store;
    env->enqueue = fake_enqueue;
    env->fetch_range = fake_fetch_range;
    env->remember_serial = fake_remember_serial;
    env->ctx = f;
}

/* Clear the observation buffers so each test asserts only on what it caused. */
static void observe_reset(fake_env *f)
{
    f->sent_count = 0;
}

/* ------------------------------------------------------------------------- */
/* T1: argument validation                                                   */
/* ------------------------------------------------------------------------- */

static void test_arguments(fake_env *f, relay_env *env)
{
    observe_reset(f);

    char wire[RELAY_BODY_MAX + 256];
    const int n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_A], 1, NOW, "x");
    CHECK(n > 0);

    CHECK(relay_receive(NULL, CH_A, wire, (size_t) n, NOW) == RELAY_DISCARDED);
    CHECK(relay_receive(env, -1, wire, (size_t) n, NOW) == RELAY_DISCARDED);
    CHECK(relay_receive(env, CH_A, NULL, 0, NOW) == RELAY_DISCARDED);
    CHECK(relay_receive(env, CH_A, wire, 0, NOW) == RELAY_DISCARDED);

    relay_env no_self = *env;
    no_self.self_public_key = NULL;
    CHECK(relay_receive(&no_self, CH_A, wire, (size_t) n, NOW) == RELAY_DISCARDED);

    /* A rejected call must not have stored or sent anything. */
    int sn = -1;
    CHECK(msg_database_last_sn(CH_A, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 0);
    CHECK(f->sent_count == 0);
}

/* ------------------------------------------------------------------------- */
/* T2: malformed and foreign records                                         */
/* ------------------------------------------------------------------------- */

static void test_malformed(fake_env *f, relay_env *env)
{
    observe_reset(f);

    static const char *const bad[] = {
        "", "garbage", "ZCZC TEXT\n", "ZCZC TEXT\nA\nB\n0001 1\nno terminator",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(relay_receive(env, CH_A, bad[i], strlen(bad[i]), NOW) == RELAY_DISCARDED);
    }

    /* A control record addressed to somebody else is ignored. */
    char cmd[RELAY_BODY_MAX + 256];
    const int n = tg_encode_cmd(cmd, sizeof(cmd), f->keys[CH_B], f->keys[CH_A], 1, 2);
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_A, cmd, (size_t) n, NOW) == RELAY_DISCARDED);

    CHECK(f->sent_count == 0);
}

/* ------------------------------------------------------------------------- */
/* T3: a telegram addressed to us is stored                                  */
/* ------------------------------------------------------------------------- */

static void test_deliver_and_store(fake_env *f, relay_env *env)
{
    observe_reset(f);

    char wire[RELAY_BODY_MAX + 256];
    const int n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_C], 1, NOW, "hello");
    CHECK(n > 0);

    CHECK(relay_receive(env, CH_C, wire, (size_t) n, NOW) == RELAY_STORED);

    /* Persisted as an incoming record, holding the body rather than the wire
     * form so a replay can be regenerated by the encoder. */
    int sn = 0;
    CHECK(msg_database_last_sn(CH_C, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 1);

    struct CHAT_MSG recent[MAX_MSG_TOP];
    CHECK(msg_database_top(recent, MAX_MSG_TOP) > 0);
    CHECK(strcmp(recent[0].message, "hello") == 0);

    /* The serial mirror was updated. */
    CHECK(f->serial_mirror[CH_C] == 1);

    /* Nothing was sent anywhere. */
    CHECK(f->sent_count == 0);
}

/* ------------------------------------------------------------------------- */
/* T4: a repeated serial is a duplicate, not a second record                 */
/* ------------------------------------------------------------------------- */

static void test_duplicate(fake_env *f, relay_env *env)
{
    observe_reset(f);

    char wire[RELAY_BODY_MAX + 256];
    const int n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_D], 5, NOW, "once");
    CHECK(n > 0);

    CHECK(relay_receive(env, CH_D, wire, (size_t) n, NOW) == RELAY_STORED);
    CHECK(relay_receive(env, CH_D, wire, (size_t) n, NOW) == RELAY_DUPLICATE);

    int sn = 0;
    CHECK(msg_database_last_sn(CH_D, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 5);
    CHECK(f->sent_count == 0);
}

/* ------------------------------------------------------------------------- */
/* T5: a gap triggers a replay request carrying the exact range              */
/* ------------------------------------------------------------------------- */

static void test_gap_requests_replay(fake_env *f, relay_env *env)
{
    observe_reset(f);

    char wire[RELAY_BODY_MAX + 256];
    int n;

    /* Serials 1 and 2 arrive ... */
    n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_E], 1, NOW, "one");
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_E, wire, (size_t) n, NOW) == RELAY_STORED);

    n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_E], 2, NOW, "two");
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_E, wire, (size_t) n, NOW) == RELAY_STORED);

    /* ... then 5 arrives, so 3 and 4 are missing. */
    n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_E], 5, NOW, "five");
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_E, wire, (size_t) n, NOW) == RELAY_GAP_REQUESTED);

    /* The request is a control record addressed to the peer, asking for 3-4. */
    CHECK(f->sent_count == 1);
    CHECK(f->sent_channel[0] == CH_E);

    tg_message cmd;
    CHECK(tg_decode(f->sent[0], f->sent_len[0], &cmd) == TG_CMD);
    CHECK(strcmp(cmd.receiver, f->keys[CH_E]) == 0);
    CHECK(strcmp(cmd.sender, SELF_KEY) == 0);
    CHECK(cmd.sn == 3);
    CHECK(cmd.sn_to == 4);

    /* The telegram that revealed the gap is still stored. */
    int sn = 0;
    CHECK(msg_database_last_sn(CH_E, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 5);
}

/* ------------------------------------------------------------------------- */
/* T6: the replayed telegrams are recognised as recoveries                   */
/* ------------------------------------------------------------------------- */

static void test_recovered(fake_env *f, relay_env *env)
{
    observe_reset(f);

    char wire[RELAY_BODY_MAX + 256];

    /* Continues on CH_E: the peer replays 3 and 4. */
    int n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_E], 3, NOW, "three");
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_E, wire, (size_t) n, NOW) == RELAY_RECOVERED);

    n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_E], 4, NOW, "four");
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_E, wire, (size_t) n, NOW) == RELAY_RECOVERED);

    /* A second copy of a recovered telegram is a duplicate again. */
    n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_E], 3, NOW, "three");
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_E, wire, (size_t) n, NOW) == RELAY_DUPLICATE);

    /* Normal traffic resumes. */
    n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_E], 6, NOW, "six");
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_E, wire, (size_t) n, NOW) == RELAY_STORED);

    /* No further replay request was made. */
    CHECK(f->sent_count == 0);
}

/* ------------------------------------------------------------------------- */
/* T7: an unrecoverably large gap resynchronises                             */
/* ------------------------------------------------------------------------- */

static void test_resync(fake_env *f, relay_env *env)
{
    observe_reset(f);

    char wire[RELAY_BODY_MAX + 256];

    int n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_G], 1, NOW, "start");
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_G, wire, (size_t) n, NOW) == RELAY_STORED);

    /* A jump strictly beyond the recovery budget. The tracker expects serial 2
     * here, and a gap is recoverable only while its size is at most
     * RELAY_MAX_GAP, so the arriving serial must exceed 2 + RELAY_MAX_GAP. */
    const int far = 3 + RELAY_MAX_GAP;

    n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[CH_G], far, NOW, "jump");
    CHECK(n > 0);
    CHECK(relay_receive(env, CH_G, wire, (size_t) n, NOW) == RELAY_RESYNCED);

    /* A resync must not ask for a huge replay. */
    CHECK(f->sent_count == 0);
}

/* ------------------------------------------------------------------------- */
/* T8: forwarding keeps the record intact for a metaCom peer                 */
/* ------------------------------------------------------------------------- */

static void test_relay_to_metacom(fake_env *f, relay_env *env)
{
    observe_reset(f);

    /* Received from an even channel, which speaks metaCom. */
    const int source = CH_I;
    const int destination = CH_H;

    char wire[RELAY_BODY_MAX + 256];
    const int n = tg_encode_text(wire, sizeof(wire), f->keys[destination], f->keys[source],
                                 9, NOW, "route me");
    CHECK(n > 0);

    CHECK(relay_receive(env, source, wire, (size_t) n, NOW) == RELAY_RELAYED);

    /* The destination received the record byte for byte, framing included. */
    CHECK(f->sent_count == 1);
    CHECK(f->sent_channel[0] == destination);
    CHECK(f->sent_len[0] == (size_t) n);
    CHECK(memcmp(f->sent[0], wire, (size_t) n) == 0);

    /* Nothing was stored locally: it was not for us. */
    int sn = -1;
    CHECK(msg_database_last_sn(destination, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 0);
}

/* ------------------------------------------------------------------------- */
/* T9: forwarding to a plain peer sends only the body                        */
/* ------------------------------------------------------------------------- */

static void test_relay_to_plain(fake_env *f, relay_env *env)
{
    observe_reset(f);

    /* Received from an odd channel, which does not speak metaCom. */
    const int source = CH_J;
    const int destination = CH_H;

    char wire[RELAY_BODY_MAX + 256];
    const int n = tg_encode_text(wire, sizeof(wire), f->keys[destination], f->keys[source],
                                 10, NOW, "plain text");
    CHECK(n > 0);

    CHECK(relay_receive(env, source, wire, (size_t) n, NOW) == RELAY_RELAYED);

    CHECK(f->sent_count == 1);
    CHECK(f->sent_channel[0] == destination);
    CHECK(strcmp(f->sent[0], "plain text") == 0);
}

/* ------------------------------------------------------------------------- */
/* T10: an unroutable telegram is reported, not silently dropped             */
/* ------------------------------------------------------------------------- */

static void test_no_route(fake_env *f, relay_env *env)
{
    observe_reset(f);

    /* A well formed key that is not in the peer table. */
    static const char unknown[] =
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";

    char wire[RELAY_BODY_MAX + 256];
    const int n = tg_encode_text(wire, sizeof(wire), unknown, f->keys[CH_K], 11, NOW, "lost");
    CHECK(n > 0);

    CHECK(relay_receive(env, CH_K, wire, (size_t) n, NOW) == RELAY_NO_ROUTE);
    CHECK(f->sent_count == 0);
}

/* ------------------------------------------------------------------------- */
/* T11: a replay request is answered from stored history                     */
/* ------------------------------------------------------------------------- */

static void test_serve_replay(fake_env *f, relay_env *env)
{
    observe_reset(f);

    const int channel = CH_L;

    /* Three telegrams were sent to this peer and stored as outgoing. */
    for (int i = 0; i < 3; ++i) {
        char body[32];
        snprintf(body, sizeof(body), "sent-%d", i + 1);
        CHECK(msg_database_add(channel, i + 1, (time_t) (NOW + i), SELF_KEY,
                               "outgoing", body) == MSG_DB_OK);
    }

    /* The peer asks for serials 1 to 3. */
    char request[RELAY_BODY_MAX + 256];
    const int n = tg_encode_cmd(request, sizeof(request), SELF_KEY, f->keys[channel], 1, 3);
    CHECK(n > 0);

    CHECK(relay_receive(env, channel, request, (size_t) n, NOW) == RELAY_SERVED);

    /* Three well formed telegrams went back, in order, carrying the bodies. */
    CHECK(f->sent_count == 3);

    for (int i = 0; i < 3; ++i) {
        char expect[32];
        snprintf(expect, sizeof(expect), "sent-%d", i + 1);

        CHECK(f->sent_channel[i] == channel);

        tg_message tg;
        CHECK(tg_decode(f->sent[i], f->sent_len[i], &tg) == TG_TEXT);
        CHECK(strcmp(tg.receiver, f->keys[channel]) == 0);
        CHECK(strcmp(tg.sender, SELF_KEY) == 0);
        CHECK(tg.sn == i + 1);
        CHECK(strcmp(tg.body, expect) == 0);
    }

    /* The request itself was not stored as an incoming telegram. */
    int sn = -1;
    CHECK(msg_database_last_sn(channel, "incoming", &sn) == MSG_DB_OK);
    CHECK(sn == 0);
}

/* ------------------------------------------------------------------------- */
/* T12: a replay request for unknown serials is reported                     */
/* ------------------------------------------------------------------------- */

static void test_serve_replay_empty(fake_env *f, relay_env *env)
{
    observe_reset(f);

    const int channel = CH_M;

    char request[RELAY_BODY_MAX + 256];
    const int n = tg_encode_cmd(request, sizeof(request), SELF_KEY, f->keys[channel], 400, 410);
    CHECK(n > 0);

    CHECK(relay_receive(env, channel, request, (size_t) n, NOW) == RELAY_NOTHING_STORED);
    CHECK(f->sent_count == 0);
}

/* ------------------------------------------------------------------------- */
/* T13: trackers are resumed from stored history after a restart             */
/* ------------------------------------------------------------------------- */

/* Counts how many channels the recovery actually probes, so the start-up bound
 * can be asserted rather than trusted. */
static int g_probe_calls;
static int g_filter_channel;

static bool test_channel_filter(void *ctx, int channel)
{
    (void) ctx;
    return channel == g_filter_channel;
}

static int test_last_sn(void *ctx, int channel, int *out)
{
    (void) ctx;
    ++g_probe_calls;
    return (msg_database_last_sn(channel, "incoming", out) == MSG_DB_OK) ? 0 : -1;
}

static void test_recover_trackers(fake_env *f, relay_env *env)
{
    observe_reset(f);

    const int channel = CH_N;

    /* History says the last telegram received was serial 7. */
    CHECK(msg_database_add(channel, 7, (time_t) NOW, f->keys[channel],
                           "incoming", "before restart") == MSG_DB_OK);

    /* A fresh tracker, as if the process had just started. */
    sn_tracker_init(&f->tracker[channel], TG_SN_MIN);
    CHECK(f->tracker[channel].synced == false);

    CHECK(relay_recover_trackers(env, test_last_sn, CHANNELS) >= 1);
    CHECK(f->tracker[channel].synced == true);
    CHECK(f->tracker[channel].expected == 8);

    /* The first telegram of the new session is therefore not a gap ... */
    char wire[RELAY_BODY_MAX + 256];
    int n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[channel], 8, NOW, "after restart");
    CHECK(n > 0);
    CHECK(relay_receive(env, channel, wire, (size_t) n, NOW) == RELAY_STORED);

    /* ... and the last telegram of the previous session is a duplicate. */
    n = tg_encode_text(wire, sizeof(wire), SELF_KEY, f->keys[channel], 7, NOW, "before restart");
    CHECK(n > 0);
    CHECK(relay_receive(env, channel, wire, (size_t) n, NOW) == RELAY_DUPLICATE);

    /* Degenerate arguments are tolerated. */
    CHECK(relay_recover_trackers(NULL, test_last_sn, CHANNELS) == 0);
    CHECK(relay_recover_trackers(env, NULL, CHANNELS) == 0);
    CHECK(relay_recover_trackers(env, test_last_sn, 0) == 0);

    /* Start-up cost: with a channel filter in place only the accepted channels
     * are probed. This is what keeps a relay with a handful of contacts from
     * running one database query per channel across the whole channel space. */
    g_probe_calls = 0;
    g_filter_channel = channel;
    env->channel_known = test_channel_filter;

    CHECK(relay_recover_trackers(env, test_last_sn, CHANNELS) == 1);
    CHECK(g_probe_calls == 1);

    /* A filter that accepts nothing probes nothing at all. */
    g_probe_calls = 0;
    g_filter_channel = -1;

    CHECK(relay_recover_trackers(env, test_last_sn, CHANNELS) == 0);
    CHECK(g_probe_calls == 0);

    env->channel_known = NULL;
}

int main(void)
{
    char template[] = "/tmp/toxrelayer_relay_XXXXXX";
    char *dir = mkdtemp(template);

    if (dir == NULL) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }

    if (chdir(dir) != 0) {
        perror("chdir");
        return EXIT_FAILURE;
    }

    CHECK(msg_database_open() == MSG_DB_OK);

    static fake_env f;
    relay_env env;

    fake_reset(&f);
    wire_env(&env, &f);

    mq_init();

    test_arguments(&f, &env);
    test_malformed(&f, &env);
    test_deliver_and_store(&f, &env);
    test_duplicate(&f, &env);
    test_gap_requests_replay(&f, &env);
    test_recovered(&f, &env);
    test_resync(&f, &env);
    test_relay_to_metacom(&f, &env);
    test_relay_to_plain(&f, &env);
    test_no_route(&f, &env);
    test_serve_replay(&f, &env);
    test_serve_replay_empty(&f, &env);
    test_recover_trackers(&f, &env);

    msg_database_close();

    printf("test_relay: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}



