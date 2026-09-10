/*  msg_queue.c
 *
 *  Store-and-forward delivery queue. The contract of every entry point, the
 *  ring-buffer invariant and the retry policy are documented in msg_queue.h.
 *
 *  Invariants maintained at all times (verified by mq_check_invariants()):
 *    I1  g_count <= MQ_CAPACITY
 *    I2  g_count == number of slots whose state != MQ_SLOT_FREE
 *    I3  g_in_flight == number of slots in state MQ_SLOT_AWAITING_ACK
 *    I4  g_head < MQ_CAPACITY and g_tail < MQ_CAPACITY
 */

#include <assert.h>
#include <string.h>

#include "msg_queue.h"

/* Compile time guarantees on the configuration. */
_Static_assert(MQ_CAPACITY > 0, "MQ_CAPACITY must be positive");
_Static_assert(MQ_PAYLOAD_MAX >= 1024, "MQ_PAYLOAD_MAX is implausibly small");
_Static_assert(MQ_MAX_ATTEMPTS > 0, "MQ_MAX_ATTEMPTS must be positive");

typedef enum {
    MQ_SLOT_FREE = 0,
    MQ_SLOT_PENDING,
    MQ_SLOT_AWAITING_ACK
} mq_slot_state;

typedef struct {
    /* Fields are ordered largest-alignment-first so the compiler inserts no
     * padding: q_entry is stored MQ_CAPACITY times, and the analyser treats
     * avoidable padding as a defect. */
    size_t         len;
    time_t         enqueued_at;
    time_t         last_attempt;
    int64_t        receipt;        /* valid only in MQ_SLOT_AWAITING_ACK */
    int            friendnum;
    uint32_t       attempts;
    mq_slot_state  state;
    bool           attempted;      /* false until the first delivery attempt */
    char           payload[MQ_PAYLOAD_MAX];
} mq_entry;

static mq_entry g_ring[MQ_CAPACITY];
static size_t   g_head;       /* oldest occupied slot                      */
static size_t   g_tail;       /* next candidate insertion slot             */
static size_t   g_count;      /* occupied slots (PENDING + AWAITING_ACK)    */
static size_t   g_in_flight;  /* slots in MQ_SLOT_AWAITING_ACK              */
static mq_stats g_stats;
static mq_drop_fn g_drop_fn;
static void      *g_drop_ctx;
static mq_change_fn g_change_fn;
static void        *g_change_ctx;

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

static void mq_notify_change(void);

static void mq_check_invariants(void)
{
    assert(g_head < MQ_CAPACITY);
    assert(g_tail < MQ_CAPACITY);
    assert(g_count <= MQ_CAPACITY);

    size_t occupied = 0;
    size_t in_flight = 0;

    for (size_t i = 0; i < MQ_CAPACITY; ++i) {
        if (g_ring[i].state != MQ_SLOT_FREE) {
            occupied++;
        }
        if (g_ring[i].state == MQ_SLOT_AWAITING_ACK) {
            in_flight++;
        }
    }

    assert(occupied == g_count);        /* I2 */
    assert(in_flight == g_in_flight);   /* I3 */
}

/* Release a slot and account for it. The slot must currently be occupied. */
static void mq_release_slot(mq_entry *e)
{
    assert(e != NULL);
    assert(e->state != MQ_SLOT_FREE);

    if (e->state == MQ_SLOT_AWAITING_ACK) {
        assert(g_in_flight > 0);
        g_in_flight--;
    }

    assert(g_count > 0);
    g_count--;

    e->state = MQ_SLOT_FREE;
    e->receipt = -1;
    e->len = 0;
    e->attempts = 0;
    e->attempted = false;
    e->payload[0] = '\0';
}

/* Retire a message that exhausted its attempt budget. */
static void mq_retire(mq_entry *e)
{
    const int friendnum = e->friendnum;
    const uint32_t attempts = e->attempts;

    mq_release_slot(e);
    g_stats.total_dropped++;

    if (g_drop_fn != NULL) {
        g_drop_fn(g_drop_ctx, friendnum, attempts);
    }

    mq_notify_change();
}

/* Tell a mirror that the set of stored messages changed. Called only after the
 * transition has completed, so the observer always sees a consistent queue. */
static void mq_notify_change(void)
{
    if (g_change_fn != NULL) {
        g_change_fn(g_change_ctx);
    }
}

/* Advance g_tail to a free slot. Returns false when the ring is full. */
static bool mq_find_free_slot(size_t *out)
{
    for (size_t i = 0; i < MQ_CAPACITY; ++i) {
        if (g_ring[g_tail].state == MQ_SLOT_FREE) {
            *out = g_tail;
            return true;
        }

        g_tail = (g_tail + 1) % MQ_CAPACITY;
    }

    return false;
}

/* Advance g_head past released slots so it always points at the oldest
 * occupied entry (or at g_tail when the queue is empty). */
static void mq_advance_head(void)
{
    while (g_count > 0 && g_ring[g_head].state == MQ_SLOT_FREE) {
        g_head = (g_head + 1) % MQ_CAPACITY;
    }

    if (g_count == 0) {
        g_head = g_tail;
    }
}


/* ------------------------------------------------------------------------- */
/* Public interface                                                          */
/* ------------------------------------------------------------------------- */

void mq_init(void)
{
    memset(g_ring, 0, sizeof(g_ring));

    g_head = 0;
    g_tail = 0;
    g_count = 0;
    g_in_flight = 0;

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.capacity = MQ_CAPACITY;

    g_drop_fn = NULL;
    g_drop_ctx = NULL;
    g_change_fn = NULL;
    g_change_ctx = NULL;

    for (size_t i = 0; i < MQ_CAPACITY; ++i) {
        g_ring[i].state = MQ_SLOT_FREE;
        g_ring[i].receipt = -1;
    }

    mq_check_invariants();
}

size_t mq_count(void)
{
    return g_count;
}

bool mq_is_empty(void)
{
    return g_count == 0;
}

bool mq_is_full(void)
{
    return g_count == MQ_CAPACITY;
}

size_t mq_in_flight(void)
{
    return g_in_flight;
}

void mq_set_drop_hook(mq_drop_fn fn, void *ctx)
{
    g_drop_fn = fn;
    g_drop_ctx = ctx;
}

void mq_set_change_hook(mq_change_fn fn, void *ctx)
{
    g_change_fn = fn;
    g_change_ctx = ctx;
}

void mq_foreach(mq_visit_fn fn, void *ctx)
{
    if (fn == NULL) {
        return;
    }

    /* Walk forward from the oldest occupied slot; g_count bounds the walk so a
     * null callback can never loop over a slot twice. */
    size_t visited = 0;

    for (size_t i = 0; i < MQ_CAPACITY && visited < g_count; ++i) {
        const mq_entry *e = &g_ring[(g_head + i) % MQ_CAPACITY];

        if (e->state == MQ_SLOT_FREE) {
            continue;
        }

        fn(ctx, e->friendnum, e->payload, e->len);
        visited++;
    }
}

void mq_stats_get(mq_stats *out)
{
    if (out == NULL) {
        return;
    }

    *out = g_stats;
    out->capacity = MQ_CAPACITY;
    out->count = g_count;
    out->in_flight = g_in_flight;
}

int mq_enqueue(int friendnum, const char *payload, size_t len, time_t now)
{
    if (friendnum < 0 || payload == NULL || len == 0 || len >= MQ_PAYLOAD_MAX) {
        return MQ_ERR_RANGE;
    }

    size_t slot = 0;

    if (!mq_find_free_slot(&slot)) {
        g_stats.total_full_rejections++;
        return MQ_ERR_FULL;
    }

    mq_entry *e = &g_ring[slot];

    memcpy(e->payload, payload, len);
    e->payload[len] = '\0';

    e->len          = len;
    e->friendnum    = friendnum;
    e->enqueued_at  = now;
    e->last_attempt = 0;
    e->attempted    = false;  /* eligible for an immediate first attempt */
    e->attempts     = 0;
    e->receipt      = -1;
    e->state        = MQ_SLOT_PENDING;

    g_count++;
    g_stats.total_enqueued++;

    if (g_count == 1) {
        g_head = slot;        /* the queue was empty: this becomes the oldest */
    }

    g_tail = (slot + 1) % MQ_CAPACITY;

    mq_check_invariants();
    mq_notify_change();
    return MQ_OK;
}

int mq_confirm(int friendnum, int receipt)
{
    if (friendnum < 0 || receipt < 0) {
        return MQ_ERR_RANGE;
    }

    for (size_t i = 0; i < MQ_CAPACITY; ++i) {
        mq_entry *e = &g_ring[i];

        if (e->state == MQ_SLOT_AWAITING_ACK &&
            e->friendnum == friendnum &&
            e->receipt == (int64_t) receipt) {
            mq_release_slot(e);
            g_stats.total_confirmed++;
            mq_advance_head();
            mq_check_invariants();
            mq_notify_change();
            return MQ_OK;
        }
    }

    return MQ_ERR_NOTFOUND;
}

mq_outcome mq_service(mq_transport_fn tx, void *ctx, time_t now)
{
    if (tx == NULL || g_count == 0) {
        return MQ_OUTCOME_IDLE;
    }

    mq_advance_head();

    /* Scan forward from the oldest entry. Several messages may be in flight at
     * once, so an entry still inside its acknowledgement window is skipped
     * instead of blocking the entries behind it. */
    for (size_t inspected = 0, idx = g_head;
         inspected < MQ_CAPACITY;
         ++inspected, idx = (idx + 1) % MQ_CAPACITY) {

        mq_entry *e = &g_ring[idx];

        if (e->state == MQ_SLOT_FREE) {
            continue;
        }

        if (e->state == MQ_SLOT_AWAITING_ACK) {
            if (now - e->last_attempt < MQ_ACK_TIMEOUT_SEC) {
                continue;                     /* still legitimately in flight */
            }

            /* The peer never acknowledged: assume the packet was lost. */
            assert(g_in_flight > 0);
            g_in_flight--;
            e->receipt = -1;
            e->state = MQ_SLOT_PENDING;
            e->attempts++;

            if (e->attempts >= MQ_MAX_ATTEMPTS) {
                mq_retire(e);
                mq_advance_head();
                mq_check_invariants();
                return MQ_OUTCOME_DROPPED;
            }
            /* fall through and retry on this pass */
        }

        /* e->state == MQ_SLOT_PENDING */
        if (e->attempted && (now - e->last_attempt) < MQ_RETRY_BACKOFF_SEC) {
            continue;                         /* respect the backoff window */
        }

        const int rc = tx(ctx, e->friendnum, e->payload, e->len);
        e->last_attempt = now;
        e->attempted = true;

        if (rc >= 0) {
            e->receipt = rc;
            e->state = MQ_SLOT_AWAITING_ACK;
            g_in_flight++;
            g_stats.total_sent++;
            mq_check_invariants();
            return MQ_OUTCOME_SENT;
        }

        e->attempts++;

        if (rc == MQ_SEND_FATAL || e->attempts >= MQ_MAX_ATTEMPTS) {
            mq_retire(e);
            mq_advance_head();
            mq_check_invariants();
            return MQ_OUTCOME_DROPPED;
        }

        mq_check_invariants();
        return MQ_OUTCOME_DEFERRED;
    }

    return MQ_OUTCOME_IDLE;
}
