/*  test_mq_persist.c
 *
 *  Verification suite for the durable delivery-queue mirror.
 *
 *  The suite drives the real msg_queue through the real mq_persist file format.
 *  It runs inside a throw-away mkdtemp() directory, never touches production
 *  data and needs no network. Corrupt files are crafted byte by byte so every
 *  rejection path is exercised, and every rejection is checked to leave the
 *  live queue untouched.
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_mq_persist \
 *         tests/test_mq_persist.c src/msg_queue.c src/mq_persist.c
 *      ./test_mq_persist
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../src/mq_persist.h"
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

#define T0 ((time_t) 1700000000)

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Collect what mq_foreach() reports, so FIFO order can be asserted exactly. */
#define VISIT_MAX 16

typedef struct {
    int    count;
    int    friendnum[VISIT_MAX];
    size_t len[VISIT_MAX];
    char   payload[VISIT_MAX][MQ_PAYLOAD_MAX];
} visit_log;

static visit_log g_visits;

static void visit_collect(void *ctx, int friendnum, const char *payload, size_t len)
{
    visit_log *log = (visit_log *) ctx;

    if (log->count >= VISIT_MAX) {
        return;
    }

    const int i = log->count++;
    log->friendnum[i] = friendnum;
    log->len[i] = len;

    if (len < MQ_PAYLOAD_MAX) {
        memcpy(log->payload[i], payload, len);
        log->payload[i][len] = '\0';
    }
}

static void visit_reset(void)
{
    memset(&g_visits, 0, sizeof(g_visits));
}

/* Mock transport used by the restart scenario. */
typedef struct {
    int  calls;
    char log[VISIT_MAX][MQ_PAYLOAD_MAX];
    int  next_receipt;
    int  scripted_result;    /* 0 makes the mock succeed with a fresh receipt */
} mock_tx;

static mock_tx g_tx;

static int mock_transport(void *ctx, int friendnum, const char *payload, size_t len)
{
    (void) friendnum;
    mock_tx *m = (mock_tx *) ctx;

    if (len < MQ_PAYLOAD_MAX && m->calls < VISIT_MAX) {
        snprintf(m->log[m->calls], sizeof(m->log[0]), "%.*s", (int) len, payload);
    }

    m->calls++;

    if (m->scripted_result != 0) {
        return m->scripted_result;
    }

    m->next_receipt++;
    return m->next_receipt;
}

/* Change-hook counter. */
static int g_change_calls;

static void count_change(void *ctx)
{
    (void) ctx;
    g_change_calls++;
}

/* Write raw bytes so malformed files can be presented to the loader. */
static void write_raw(const char *path, const void *data, size_t len)
{
    FILE *fp = fopen(path, "wb");

    CHECK(fp != NULL);
    CHECK(fwrite(data, 1, len, fp) == len);
    CHECK(fclose(fp) == 0);
}

/* Idempotent file-existence probe that does not rely on misc.c. */
static int path_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

/* Little-endian encoders mirroring the documented on-disk format. */
static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t) (value & 0xffu);
    out[1] = (uint8_t) ((value >> 8) & 0xffu);
    out[2] = (uint8_t) ((value >> 16) & 0xffu);
    out[3] = (uint8_t) ((value >> 24) & 0xffu);
}

/* Build a complete file: header plus the supplied records. */
typedef struct {
    int32_t friendnum;
    uint32_t len;
    char     payload[MQ_PAYLOAD_MAX];
} raw_record;

static size_t build_file(uint8_t *out, uint32_t version, uint32_t count,
                         const raw_record *records, int record_count)
{
    static const char magic[8] = { 'M', 'Q', 'P', 'Q', 'U', 'E', 'U', 'E' };

    memcpy(out, magic, sizeof(magic));
    put_u32(out + 8, version);
    put_u32(out + 12, count);
    put_u32(out + 16, 0u);

    size_t off = 20;

    for (int i = 0; i < record_count; ++i) {
        put_u32(out + off, (uint32_t) records[i].friendnum);
        put_u32(out + off + 4, records[i].len);
        off += 8;
        memcpy(out + off, records[i].payload, records[i].len);
        off += records[i].len;
    }

    return off;
}

/* ------------------------------------------------------------------------- */
/* Test scenarios                                                            */
/* ------------------------------------------------------------------------- */

#define QUEUE_PATH "mq_queue.dat"

/* T1: a saved queue is restored exactly, in FIFO order. */
static void test_round_trip(void)
{
    mq_init();
    CHECK(mq_enqueue(3, "alpha", 5, T0) == MQ_OK);
    CHECK(mq_enqueue(7, "beta", 4, T0) == MQ_OK);
    CHECK(mq_enqueue(1, "gamma", 5, T0) == MQ_OK);

    CHECK(mq_persist_save(QUEUE_PATH) == MQ_PERSIST_OK);
    CHECK(path_exists(QUEUE_PATH));
    CHECK(!path_exists(QUEUE_PATH ".tmp"));

    mq_init();
    CHECK(mq_count() == 0);

    const int restored = mq_persist_load(QUEUE_PATH, T0 + 100);
    CHECK(restored == 3);
    CHECK(mq_count() == 3);

    visit_reset();
    mq_foreach(visit_collect, &g_visits);

    CHECK(g_visits.count == 3);
    CHECK(g_visits.friendnum[0] == 3);
    CHECK(strcmp(g_visits.payload[0], "alpha") == 0);
    CHECK(g_visits.friendnum[1] == 7);
    CHECK(strcmp(g_visits.payload[1], "beta") == 0);
    CHECK(g_visits.friendnum[2] == 1);
    CHECK(strcmp(g_visits.payload[2], "gamma") == 0);
}

/* T2: absent and invalid paths behave as documented. */
static void test_path_validation(void)
{
    mq_init();

    CHECK(mq_persist_load("no_such_queue_file.dat", T0) == MQ_PERSIST_ERR_NOFILE);
    CHECK(mq_persist_load(NULL, T0) == MQ_PERSIST_ERR_PATH);
    CHECK(mq_persist_load("", T0) == MQ_PERSIST_ERR_PATH);
    CHECK(mq_persist_save(NULL) == MQ_PERSIST_ERR_PATH);
    CHECK(mq_persist_save("") == MQ_PERSIST_ERR_PATH);
    CHECK(mq_persist_save("/no/such/directory/queue.dat") == MQ_PERSIST_ERR_IO);
}

/* T3: saving replaces the previous file and never leaves a temporary behind. */
static void test_atomic_replace(void)
{
    mq_init();
    CHECK(mq_enqueue(1, "old", 3, T0) == MQ_OK);
    CHECK(mq_persist_save(QUEUE_PATH) == MQ_PERSIST_OK);

    mq_init();
    CHECK(mq_enqueue(2, "new", 3, T0) == MQ_OK);
    CHECK(mq_persist_save(QUEUE_PATH) == MQ_PERSIST_OK);

    mq_init();
    CHECK(mq_persist_load(QUEUE_PATH, T0) == 1);

    visit_reset();
    mq_foreach(visit_collect, &g_visits);
    CHECK(g_visits.count == 1);
    CHECK(g_visits.friendnum[0] == 2);
    CHECK(strcmp(g_visits.payload[0], "new") == 0);
    CHECK(!path_exists(QUEUE_PATH ".tmp"));
}

/* T3b: a path that names a subdirectory exercises the directory flush that
 *      follows the atomic replace, and confirms the temporary is created beside
 *      the target rather than in the current directory. */
static void test_nested_path(void)
{
    const char *subdir = "persist_sub";
    const char *path = "persist_sub/queue.dat";

    (void) rmdir(subdir);
    CHECK(mkdir(subdir, 0700) == 0);

    mq_init();
    CHECK(mq_enqueue(4, "nested", 6, T0) == MQ_OK);
    CHECK(mq_persist_save(path) == MQ_PERSIST_OK);
    CHECK(path_exists(path));
    CHECK(!path_exists("persist_sub/queue.dat.tmp"));

    mq_init();
    CHECK(mq_persist_load(path, T0) == 1);

    visit_reset();
    mq_foreach(visit_collect, &g_visits);
    CHECK(g_visits.count == 1);
    CHECK(g_visits.friendnum[0] == 4);
    CHECK(strcmp(g_visits.payload[0], "nested") == 0);

    CHECK(unlink(path) == 0);
    CHECK(rmdir(subdir) == 0);
    CHECK(!path_exists(subdir));
}

/* T4: an empty queue round-trips as an empty queue. */
static void test_empty_queue(void)
{
    mq_init();
    CHECK(mq_persist_save(QUEUE_PATH) == MQ_PERSIST_OK);

    mq_init();
    CHECK(mq_count() == 0);
    CHECK(mq_persist_load(QUEUE_PATH, T0) == 0);
    CHECK(mq_count() == 0);
}

/* T5: binary payloads, including a maximal one, survive byte for byte. */
static void test_payload_fidelity(void)
{
    char payload[MQ_PAYLOAD_MAX];
    payload[0] = '\0';                       /* embedded NUL */
    payload[1] = 'A';
    memset(payload + 2, 0x7f, MQ_PAYLOAD_MAX - 4);
    payload[MQ_PAYLOAD_MAX - 2] = 'B';

    const size_t len = MQ_PAYLOAD_MAX - 1;

    mq_init();
    CHECK(mq_enqueue(42, payload, len, T0) == MQ_OK);
    CHECK(mq_persist_save(QUEUE_PATH) == MQ_PERSIST_OK);

    mq_init();
    CHECK(mq_persist_load(QUEUE_PATH, T0) == 1);

    visit_reset();
    mq_foreach(visit_collect, &g_visits);
    CHECK(g_visits.count == 1);
    CHECK(g_visits.len[0] == len);
    CHECK(memcmp(g_visits.payload[0], payload, len) == 0);
}


/* T6: a corrupt file is rejected without touching the live queue. */
static void test_corrupt_file_rejected(void)
{
    uint8_t file[256];
    raw_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.friendnum = 5;
    rec.len = 4;
    memcpy(rec.payload, "data", 4);

    /* A queue that already holds one message must stay exactly as it is. */
    mq_init();
    CHECK(mq_enqueue(9, "keep", 4, T0) == MQ_OK);

    /* Bad magic. */
    size_t n = build_file(file, 1u, 1u, &rec, 1);
    file[0] = 'X';
    write_raw(QUEUE_PATH, file, n);
    CHECK(mq_persist_load(QUEUE_PATH, T0) == MQ_PERSIST_ERR_FORMAT);

    /* Unsupported version. */
    n = build_file(file, 2u, 1u, &rec, 1);
    write_raw(QUEUE_PATH, file, n);
    CHECK(mq_persist_load(QUEUE_PATH, T0) == MQ_PERSIST_ERR_FORMAT);

    /* File shorter than the header. */
    write_raw(QUEUE_PATH, file, 8);
    CHECK(mq_persist_load(QUEUE_PATH, T0) == MQ_PERSIST_ERR_FORMAT);

    CHECK(mq_count() == 1);
}

/* T7: record fields are range checked before anything is imported. */
static void test_invalid_records_rejected(void)
{
    uint8_t file[4096];
    raw_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.friendnum = 2;
    rec.len = 4;
    memcpy(rec.payload, "body", 4);

    mq_init();

    /* Header claims two records but only one is present (truncation). */
    size_t n = build_file(file, 1u, 2u, &rec, 1);
    write_raw(QUEUE_PATH, file, n);
    CHECK(mq_persist_load(QUEUE_PATH, T0) == MQ_PERSIST_ERR_FORMAT);

    /* Zero-length payload. */
    rec.len = 0;
    n = build_file(file, 1u, 1u, &rec, 1);
    write_raw(QUEUE_PATH, file, n);
    CHECK(mq_persist_load(QUEUE_PATH, T0) == MQ_PERSIST_ERR_FORMAT);

    /* Payload length equal to the module limit. */
    rec.len = MQ_PAYLOAD_MAX;
    n = build_file(file, 1u, 1u, &rec, 1);
    write_raw(QUEUE_PATH, file, n);
    CHECK(mq_persist_load(QUEUE_PATH, T0) == MQ_PERSIST_ERR_FORMAT);

    /* Negative friend number. */
    rec.friendnum = -1;
    rec.len = 4;
    memcpy(rec.payload, "body", 4);
    n = build_file(file, 1u, 1u, &rec, 1);
    write_raw(QUEUE_PATH, file, n);
    CHECK(mq_persist_load(QUEUE_PATH, T0) == MQ_PERSIST_ERR_FORMAT);

    /* Record count beyond the queue capacity. */
    put_u32(file + 8, 1u);
    put_u32(file + 12, MQ_CAPACITY + 1u);
    write_raw(QUEUE_PATH, file, 20);
    CHECK(mq_persist_load(QUEUE_PATH, T0) == MQ_PERSIST_ERR_FORMAT);

    CHECK(mq_count() == 0);
}

/* T8: a full queue refuses the restore instead of silently dropping records. */
static void test_full_queue_restore(void)
{
    uint8_t file[512];
    raw_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.friendnum = 1;
    rec.len = 4;
    memcpy(rec.payload, "body", 4);

    size_t n = build_file(file, 1u, 1u, &rec, 1);
    write_raw(QUEUE_PATH, file, n);

    mq_init();

    for (size_t i = 0; i < MQ_CAPACITY; ++i) {
        CHECK(mq_enqueue((int) i, "full", 4, T0) == MQ_OK);
    }

    CHECK(mq_is_full());
    CHECK(mq_persist_load(QUEUE_PATH, T0) == MQ_PERSIST_ERR_FULL);
    CHECK(mq_count() == MQ_CAPACITY);
}

/* T9: a restart keeps the delivery order and grants a fresh attempt budget. */
static void test_restart_delivery(void)
{
    mq_init();
    CHECK(mq_enqueue(4, "first", 5, T0) == MQ_OK);
    CHECK(mq_enqueue(4, "second", 6, T0) == MQ_OK);
    CHECK(mq_enqueue(4, "third", 5, T0) == MQ_OK);
    CHECK(mq_persist_save(QUEUE_PATH) == MQ_PERSIST_OK);

    /* Simulate the process restarting: memory is wiped, then restored. */
    mq_init();
    CHECK(mq_persist_load(QUEUE_PATH, T0 + 3600) == 3);

    memset(&g_tx, 0, sizeof(g_tx));

    for (int i = 0; i < 3; ++i) {
        CHECK(mq_service(mock_transport, &g_tx, T0 + 3600) == MQ_OUTCOME_SENT);
        CHECK(mq_confirm(4, g_tx.next_receipt) == MQ_OK);
    }

    CHECK(mq_is_empty());
    CHECK(g_tx.calls == 3);
    CHECK(strcmp(g_tx.log[0], "first") == 0);
    CHECK(strcmp(g_tx.log[1], "second") == 0);
    CHECK(strcmp(g_tx.log[2], "third") == 0);
}

/* T10: the change hook fires only when the stored set changes. */
static void test_change_hook(void)
{
    mock_tx m;
    memset(&m, 0, sizeof(m));

    mq_init();
    g_change_calls = 0;
    mq_set_change_hook(count_change, NULL);

    CHECK(mq_enqueue(1, "one", 3, T0) == MQ_OK);
    CHECK(g_change_calls == 1);

    /* A rejected enqueue must not announce a change. */
    CHECK(mq_enqueue(-1, "bad", 3, T0) == MQ_ERR_RANGE);
    CHECK(g_change_calls == 1);

    /* A delivery attempt leaves the stored set unchanged. */
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_SENT);
    CHECK(g_change_calls == 1);

    /* Confirmation removes the message, which is a change. */
    CHECK(mq_confirm(1, m.next_receipt) == MQ_OK);
    CHECK(g_change_calls == 2);

    /* A retry that leaves a message queued is not a change. */
    CHECK(mq_enqueue(2, "retry", 5, T0) == MQ_OK);
    CHECK(g_change_calls == 3);

    m.scripted_result = MQ_SEND_RETRYABLE;
    CHECK(mq_service(mock_transport, &m, T0) == MQ_OUTCOME_DEFERRED);
    CHECK(g_change_calls == 3);

    /* Retirement is a change. Step past the retry backoff so the entry is
     * eligible again on this pass. */
    m.scripted_result = MQ_SEND_FATAL;
    CHECK(mq_service(mock_transport, &m, T0 + MQ_RETRY_BACKOFF_SEC + 1) == MQ_OUTCOME_DROPPED);
    CHECK(g_change_calls == 4);

    mq_set_change_hook(NULL, NULL);
}

int main(void)
{
    char template[] = "/tmp/toxrelayer_mq_persist_XXXXXX";
    char *dir = mkdtemp(template);

    if (dir == NULL) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }

    if (chdir(dir) != 0) {
        perror("chdir");
        return EXIT_FAILURE;
    }

    test_round_trip();
    test_path_validation();
    test_atomic_replace();
    test_nested_path();
    test_empty_queue();
    test_payload_fidelity();
    test_corrupt_file_rejected();
    test_invalid_records_rejected();
    test_full_queue_restore();
    test_restart_delivery();
    test_change_hook();

    printf("test_mq_persist: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}

