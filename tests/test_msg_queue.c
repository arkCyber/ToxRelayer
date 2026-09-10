/*  test_msg_queue.c
 *
 *  Verification suite for the store-and-forward delivery queue.
 *
 *  The queue is transport agnostic, so every test drives it with a mock
 *  transport that records what was delivered and can be scripted to fail. Time
 *  is injected, therefore no test sleeps and every timeout path is exercised
 *  deterministically.
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_msg_queue \
 *         tests/test_msg_queue.c src/msg_queue.c
 *      ./test_msg_queue
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/msg_queue.h"

static int tests_run = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        tests_run++;                                                            \
        if (!(expr)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);      \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Mock transport                                                            */
/* ------------------------------------------------------------------------- */

#define MOCK_LOG_MAX 128

typedef struct {
    int     calls;
    int     last_friendnum;
    char    last_payload[MQ_PAYLOAD_MAX];
    size_t  last_len;
    int64_t next_receipt;
    int     scripted_result;      /* >= 0 receipt, or MQ_SEND_RETRYABLE/FATAL */
    char    log[MOCK_LOG_MAX][MQ_PAYLOAD_MAX];   /* delivery order           */
    int     log_count;
} mock_tx;

static void mock_reset(mock_tx *m)
{
    memset(m, 0, sizeof(*m));
    m->scripted_result = 0;       /* 0 selects "auto receipt" mode           */
}

static int mock_transport(void *ctx, int friendnum, const char *payload, size_t len)
{
    mock_tx *m = (mock_tx *) ctx;

    m->calls++;
    m->last_friendnum = friendnum;

    if (len < sizeof(m->last_payload)) {
        memcpy(m->last_payload, payload, len);
        m->last_payload[len] = '\0';
        m->last_len = len;
    }

    if (m->log_count < MOCK_LOG_MAX) {
        snprintf(m->log[m->log_count], sizeof(m->log[0]), "%.*s", (int) len, payload);
        m->log_count++;
    }

    if (m->scripted_result < 0) {
        return m->scripted_result;      /* simulated failure */
    }

    m->next_receipt++;
    return (int) m->next_receipt;
}

/* Counters used by the drop hook test. */
static int  g_drop_calls;
static int  g_drop_friendnum;
static uint32_t g_drop_attempts;

static void test_drop_hook(void *ctx, int friendnum, uint32_t attempts)
{
    (void) ctx;
    g_drop_calls++;
    g_drop_friendnum = friendnum;
    g_drop_attempts = attempts;
}


/* ------------------------------------------------------------------------- */
/* Test scenarios                                                            */
/* ------------------------------------------------------------------------- */

#define T0 ((time_t) 1700000000)

/* T1: a fresh queue is empty, not full, and has nothing in flight. */
static void test_initial_state(void)
{
    mq_init();

    CHECK(mq_count() == 0);
    CHECK(mq_is_empty());
    CHECK(!mq_is_full());
    CHECK(mq_in_flight() == 0);

    mq_stats s;
    mq_stats_get(&s);
    CHECK(s.capacity == MQ_CAPACITY);
    CHECK(s.total_enqueued == 0);
    CHECK(s.total_sent == 0);
}

/* T2: invalid arguments are rejected without changing the queue. */
static void test_enqueue_validation(void)
{
    mq_init();

    char big[MQ_PAYLOAD_MAX + 1];
    memset(big, 'A', sizeof(big));

    CHECK(mq_enqueue(-1, "x", 1, T0) == MQ_ERR_RANGE);
    CHECK(mq_enqueue(0, NULL, 1, T0) == MQ_ERR_RANGE);
    CHECK(mq_enqueue(0, "x", 0, T0) == MQ_ERR_RANGE);
    CHECK(mq_enqueue(0, big, sizeof(big), T0) == MQ_ERR_RANGE);
    CHECK(mq_count() == 0);

    /* mq_stats_get(NULL) must not crash. */
    mq_stats_get(NULL);
    CHECK(mq_count() == 0);
}

/* T3: messages are delivered in the order they were enqueued (FIFO). */
static void test_fifo_order(void)
{
    mock_tx m;
    mock_reset(&m);

    mq_init();
    CHECK(mq_enqueue(1, "alpha", 5, T0) == MQ_OK);
    CHECK(mq_enqueue(1, "beta",  4, T0) == MQ_OK);
    CHECK(mq_enqueue(2, "gamma", 5, T0) == MQ_OK);

    for (int i = 0; i < 3; ++i) {
        CHECK(mq_service(mock_transport, &m, T0 + i) == MQ_OUTCOME_SENT);
    }

    CHECK(m.calls == 3);
    CHECK(m.log_count == 3);
    CHECK(strcmp(m.log[0], "alpha") == 0);
    CHECK(strcmp(m.log[1], "beta") == 0);
    CHECK(strcmp(m.log[2], "gamma") == 0);
}

/* T4: the queue rejects further messages once every slot is occupied. */
static void test_full_queue(void)
{
    mq_init();

    for (size_t i = 0; i < MQ_CAPACITY; ++i) {
        CHECK(mq_enqueue(1, "x", 1, T0) == MQ_OK);
    }

    CHECK(mq_is_full());
    CHECK(mq_count() == MQ_CAPACITY);
    CHECK(mq_enqueue(1, "x", 1, T0) == MQ_ERR_FULL);

    mq_stats s;
    mq_stats_get(&s);
    CHECK(s.total_full_rejections == 1);
    CHECK(s.total_enqueued == MQ_CAPACITY);
}

/* T5: an acknowledgement removes exactly the matching in-flight message. */
static void test_confirm_removes_message(void)
{
    mock_tx m;
    mock_reset(&m);

    mq_init();
    CHECK(mq_enqueue(5, "hello", 5, T0) == MQ_OK);
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_SENT);
    CHECK(mq_in_flight() == 1);
    CHECK(mq_count() == 1);

    const int receipt = (int) m.next_receipt;
    CHECK(mq_confirm(5, receipt) == MQ_OK);

    CHECK(mq_count() == 0);
    CHECK(mq_in_flight() == 0);
    CHECK(mq_is_empty());

    mq_stats s;
    mq_stats_get(&s);
    CHECK(s.total_confirmed == 1);

    /* A second confirmation of the same receipt finds nothing. */
    CHECK(mq_confirm(5, receipt) == MQ_ERR_NOTFOUND);
}

/* T6: a message whose acknowledgement never arrives is retransmitted. */
static void test_ack_timeout_retransmits(void)
{
    mock_tx m;
    mock_reset(&m);

    mq_init();
    CHECK(mq_enqueue(7, "retry-me", 8, T0) == MQ_OK);

    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_SENT);
    CHECK(m.calls == 1);
    CHECK(mq_in_flight() == 1);

    /* Inside the acknowledgement window nothing happens. */
    CHECK(mq_service(mock_transport, &m, T0 + MQ_ACK_TIMEOUT_SEC - 1) == MQ_OUTCOME_IDLE);
    CHECK(m.calls == 1);

    /* Past the window the same payload is sent again. */
    CHECK(mq_service(mock_transport, &m, T0 + MQ_ACK_TIMEOUT_SEC) == MQ_OUTCOME_SENT);
    CHECK(m.calls == 2);
    CHECK(strcmp(m.log[1], "retry-me") == 0);
    CHECK(mq_count() == 1);   /* still exactly one message */
}

/* T7: with a permanently retryable transport the message is retired, and the
 *     drop hook reports the loss. This is the regression test for the original
 *     unbounded-retry defect (send_count was never incremented). */
static void test_bounded_retries_drop(void)
{
    mock_tx m;
    mock_reset(&m);
    m.scripted_result = MQ_SEND_RETRYABLE;

    mq_init();
    g_drop_calls = 0;
    g_drop_friendnum = -1;
    g_drop_attempts = 0;
    mq_set_drop_hook(test_drop_hook, NULL);

    CHECK(mq_enqueue(3, "poison", 6, T0) == MQ_OK);

    time_t now = T0;
    mq_outcome outcome = MQ_OUTCOME_IDLE;

    for (int i = 0; i < (int) MQ_MAX_ATTEMPTS + 2; ++i) {
        outcome = mq_service(mock_transport, &m, now);
        if (outcome == MQ_OUTCOME_DROPPED) {
            break;
        }
        now += MQ_RETRY_BACKOFF_SEC;      /* let the backoff expire */
    }

    CHECK(outcome == MQ_OUTCOME_DROPPED);
    CHECK(m.calls == (int) MQ_MAX_ATTEMPTS);
    CHECK(g_drop_calls == 1);
    CHECK(g_drop_friendnum == 3);
    CHECK(g_drop_attempts == MQ_MAX_ATTEMPTS);
    CHECK(mq_count() == 0);

    /* The queue is still usable afterwards. */
    m.scripted_result = 0;
    CHECK(mq_enqueue(3, "next", 4, now) == MQ_OK);
    CHECK(mq_service(mock_transport, &m, now + 1) == MQ_OUTCOME_SENT);
}

/* T8: a fatal transport error retires the message on the first attempt. */
static void test_fatal_drops_immediately(void)
{
    mock_tx m;
    mock_reset(&m);
    m.scripted_result = MQ_SEND_FATAL;

    mq_init();
    g_drop_calls = 0;
    mq_set_drop_hook(test_drop_hook, NULL);

    CHECK(mq_enqueue(9, "gone", 4, T0) == MQ_OK);
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_DROPPED);

    CHECK(m.calls == 1);
    CHECK(g_drop_calls == 1);
    CHECK(mq_count() == 0);
}

/* T9: several messages may be in flight simultaneously, so one message waiting
 *     for its acknowledgement does not block the messages behind it. */
static void test_multiple_in_flight(void)
{
    mock_tx m;
    mock_reset(&m);

    mq_init();
    CHECK(mq_enqueue(1, "one",   3, T0) == MQ_OK);
    CHECK(mq_enqueue(1, "two",   3, T0) == MQ_OK);
    CHECK(mq_enqueue(1, "three", 5, T0) == MQ_OK);

    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_SENT);
    CHECK(mq_in_flight() == 1);

    /* The first message is still unacknowledged, yet the second is sent. */
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_SENT);
    CHECK(mq_in_flight() == 2);

    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_SENT);
    CHECK(mq_in_flight() == 3);

    CHECK(m.log_count == 3);
    CHECK(strcmp(m.log[0], "one") == 0);
    CHECK(strcmp(m.log[1], "two") == 0);
    CHECK(strcmp(m.log[2], "three") == 0);

    /* Confirming them all drains the queue. */
    CHECK(mq_confirm(1, 1) == MQ_OK);
    CHECK(mq_confirm(1, 2) == MQ_OK);
    CHECK(mq_confirm(1, 3) == MQ_OK);
    CHECK(mq_is_empty());
}

/* T10: a near-maximum payload survives a retransmission byte for byte. */
static void test_payload_integrity(void)
{
    mock_tx m;
    mock_reset(&m);
    m.scripted_result = MQ_SEND_RETRYABLE;

    mq_init();

    char payload[MQ_PAYLOAD_MAX];
    for (size_t i = 0; i + 1 < sizeof(payload); ++i) {
        payload[i] = (char) ('A' + (i % 26));
    }
    payload[sizeof(payload) - 1] = '\0';

    const size_t len = MQ_PAYLOAD_MAX - 1;
    CHECK(mq_enqueue(2, payload, len, T0) == MQ_OK);

    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_DEFERRED);
    CHECK(m.last_len == len);
    CHECK(memcmp(m.last_payload, payload, len) == 0);

    /* Retransmission preserves every byte. */
    m.scripted_result = 0;
    CHECK(mq_service(mock_transport, &m, T0 + MQ_RETRY_BACKOFF_SEC) == MQ_OUTCOME_SENT);
    CHECK(m.last_len == len);
    CHECK(memcmp(m.last_payload, payload, len) == 0);
}

/* T11: argument validation for mq_confirm and the backoff window. */
static void test_confirm_validation_and_backoff(void)
{
    mock_tx m;
    mock_reset(&m);
    m.scripted_result = MQ_SEND_RETRYABLE;

    mq_init();
    CHECK(mq_enqueue(4, "slow", 4, T0) == MQ_OK);

    /* First attempt fails and is retryable. */
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_DEFERRED);
    CHECK(m.calls == 1);

    /* Within the backoff window nothing is retried. */
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_IDLE);
    CHECK(m.calls == 1);

    /* Once the window elapses the retry happens. */
    CHECK(mq_service(mock_transport, &m, T0 + MQ_RETRY_BACKOFF_SEC) == MQ_OUTCOME_DEFERRED);
    CHECK(m.calls == 2);

    /* Confirm is only valid for an in-flight message. */
    CHECK(mq_confirm(-1, 1) == MQ_ERR_RANGE);
    CHECK(mq_confirm(4, -1) == MQ_ERR_RANGE);
    CHECK(mq_confirm(4, 999) == MQ_ERR_NOTFOUND);

    /* A NULL transport is rejected without side effects. */
    CHECK(mq_service(NULL, &m, T0) == MQ_OUTCOME_IDLE);

    /* Empty queue: service is idle regardless of the transport. */
    mq_init();
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_IDLE);
}

/* T12: repeated fill/drain cycles force every ring index to be reused and must
 *      keep the occupancy bookkeeping exact. */
static void test_ring_wrap_around(void)
{
    mock_tx m;
    mock_reset(&m);

    mq_init();

    for (int lap = 0; lap < 2; ++lap) {
        for (size_t i = 0; i < MQ_CAPACITY; ++i) {
            CHECK(mq_enqueue(1, "x", 1, T0) == MQ_OK);
        }

        CHECK(mq_count() == MQ_CAPACITY);
        CHECK(mq_is_full());

        while (!mq_is_empty()) {
            const mq_outcome o = mq_service(mock_transport, &m, T0);

            if (o == MQ_OUTCOME_SENT) {
                CHECK(mq_confirm(1, (int) m.next_receipt) == MQ_OK);
            } else {
                CHECK(o == MQ_OUTCOME_IDLE);
                break;
            }
        }

        CHECK(mq_is_empty());
        CHECK(mq_in_flight() == 0);
    }

    mq_stats s;
    mq_stats_get(&s);
    CHECK(s.total_enqueued == 2 * MQ_CAPACITY);
    CHECK(s.total_confirmed == 2 * MQ_CAPACITY);
    CHECK(s.total_dropped == 0);
}

/* T13: counters stay consistent across mixed success and failure traffic. */
static void test_stats_accounting(void)
{
    mock_tx m;
    mock_reset(&m);

    mq_init();
    g_drop_calls = 0;
    mq_set_drop_hook(test_drop_hook, NULL);

    CHECK(mq_enqueue(1, "ok",    2, T0) == MQ_OK);
    CHECK(mq_enqueue(2, "fatal", 5, T0) == MQ_OK);
    CHECK(mq_enqueue(3, "full",  4, T0) == MQ_OK);

    /* 1: success */
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_SENT);

    /* 2: fatal */
    m.scripted_result = MQ_SEND_FATAL;
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_DROPPED);

    /* 3: success */
    m.scripted_result = 0;
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_SENT);

    mq_stats s;
    mq_stats_get(&s);
    CHECK(s.total_enqueued == 3);
    CHECK(s.total_sent == 2);
    CHECK(s.total_dropped == 1);
    CHECK(s.total_confirmed == 0);
    CHECK(s.total_full_rejections == 0);
    CHECK(s.count == 2);
    CHECK(s.in_flight == 2);
    CHECK(mq_in_flight() == 2);
}

int main(void)
{
    test_initial_state();
    test_enqueue_validation();
    test_fifo_order();
    test_full_queue();
    test_confirm_removes_message();
    test_ack_timeout_retransmits();
    test_bounded_retries_drop();
    test_fatal_drops_immediately();
    test_multiple_in_flight();
    test_payload_integrity();
    test_confirm_validation_and_backoff();
    test_ring_wrap_around();
    test_stats_accounting();

    printf("test_msg_queue: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}
