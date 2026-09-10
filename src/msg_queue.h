/*  msg_queue.h
 *
 *  Store-and-forward delivery queue.
 *
 *  Design notes
 *  ------------
 *  - Transport agnostic: this module never references toxcore. Delivery happens
 *    through an mq_transport_fn supplied by the caller, which makes the whole
 *    policy testable without a network.
 *  - Ring buffer with an explicit element count, so "empty" and "full" are
 *    unambiguous and every slot is usable.
 *  - Every message carries a bounded attempt counter. A message that keeps
 *    failing is retired (dead letter) instead of blocking or livelocking the
 *    queue forever. The original implementation never incremented its counter.
 *  - Time is injected as a parameter, so timeout behaviour is deterministic in
 *    tests and requires no sleeping.
 *  - No entry point prints to stdout or aborts; callers receive status codes.
 */
#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Capacity of the delivery queue (number of stored telegrams). */
#ifndef MQ_CAPACITY
#define MQ_CAPACITY 4048
#endif

/* Maximum stored payload size. toxrelayer.c statically asserts that this covers
 * TOX_MAX_MESSAGE_LENGTH, so the two can never drift apart. */
#ifndef MQ_PAYLOAD_MAX
#define MQ_PAYLOAD_MAX 1372
#endif

/* How long one attempt may stay unacknowledged before it is retried. */
#ifndef MQ_ACK_TIMEOUT_SEC
#define MQ_ACK_TIMEOUT_SEC 25
#endif

/* Minimum spacing between two attempts for the same message. */
#ifndef MQ_RETRY_BACKOFF_SEC
#define MQ_RETRY_BACKOFF_SEC 1
#endif

/* Attempts before a message is retired as undeliverable. */
#ifndef MQ_MAX_ATTEMPTS
#define MQ_MAX_ATTEMPTS 8
#endif

/* Return codes. */
#define MQ_OK            0
#define MQ_ERR_RANGE   (-1)   /* invalid argument             */
#define MQ_ERR_FULL    (-2)   /* queue has no free slot       */
#define MQ_ERR_NOTFOUND (-3)  /* no matching in-flight message */

/* Transport result codes returned by mq_transport_fn. */
#define MQ_SEND_FATAL     (-1)  /* this message can never be delivered   */
#define MQ_SEND_RETRYABLE (-2)  /* peer unreachable now, try again later */

/* Deliver `len` bytes to `friendnum`.
 *
 * @return a receipt >= 0 when the peer accepted the message and an
 *         acknowledgement is expected, MQ_SEND_RETRYABLE when delivery should
 *         be attempted again later, or MQ_SEND_FATAL when the message can never
 *         be delivered. */
typedef int (*mq_transport_fn)(void *ctx, int friendnum, const char *payload, size_t len);

/* Invoked when a message is retired after MQ_MAX_ATTEMPTS. Optional. */
typedef void (*mq_drop_fn)(void *ctx, int friendnum, uint32_t attempts);

/* Invoked when the set of stored messages changes (enqueue, confirmation or
 * retirement). Optional. It carries no payload: it exists so a caller that
 * mirrors the queue to disk knows when a flush is worth doing. */
typedef void (*mq_change_fn)(void *ctx);

/* Invoked once per stored message, oldest first, by mq_foreach().
 *
 * The queue owns `payload`; it is valid only for the duration of the call and
 * carries `len` bytes, which are not guaranteed to be NUL terminated. */
typedef void (*mq_visit_fn)(void *ctx, int friendnum, const char *payload, size_t len);

/* What mq_service() did with the message it inspected. */
typedef enum {
    MQ_OUTCOME_IDLE = 0,   /* queue empty, or nothing actionable this tick   */
    MQ_OUTCOME_SENT,       /* handed to the transport, awaiting confirmation */
    MQ_OUTCOME_DEFERRED,   /* not ready yet, left in the queue               */
    MQ_OUTCOME_DROPPED     /* retired after MQ_MAX_ATTEMPTS                  */
} mq_outcome;

typedef struct {
    size_t capacity;
    size_t count;
    size_t in_flight;
    size_t total_enqueued;
    size_t total_sent;
    size_t total_confirmed;
    size_t total_dropped;
    size_t total_full_rejections;
} mq_stats;

/* Reset the queue to empty and clear all counters. */
void mq_init(void);

/* Current occupancy. */
size_t mq_count(void);
bool   mq_is_empty(void);
bool   mq_is_full(void);

/* Number of messages handed to the transport but not yet acknowledged. */
size_t mq_in_flight(void);

/* Append one message.
 *
 * Preconditions : friendnum >= 0, payload != NULL, 0 < len < MQ_PAYLOAD_MAX.
 * Postconditions: on MQ_OK mq_count() has increased by exactly one and the
 *                 payload has been copied into the queue.
 *
 * @return MQ_OK, MQ_ERR_RANGE or MQ_ERR_FULL (the latter is counted in
 *         mq_stats.total_full_rejections).
 */
int mq_enqueue(int friendnum, const char *payload, size_t len, time_t now);

/* Mark the in-flight message identified by (friendnum, receipt) as delivered.
 *
 * @return MQ_OK, MQ_ERR_RANGE (receipt < 0 or friendnum < 0) or
 *         MQ_ERR_NOTFOUND.
 */
int mq_confirm(int friendnum, int receipt);

/* Drive the queue forward by inspecting exactly one message.
 *
 * Scans forward from the oldest message, so several messages may be in flight
 * at once, and returns after the first action taken. Callers invoke it from
 * their event loop; passing an explicit `now` keeps the timeout logic
 * deterministic.
 *
 * @return the mq_outcome of the single action performed.
 */
mq_outcome mq_service(mq_transport_fn tx, void *ctx, time_t now);

/* Install a hook that is called for every retired (dropped) message. */
void mq_set_drop_hook(mq_drop_fn fn, void *ctx);

/* Install a hook that is called whenever the set of stored messages changes.
 *
 * It is called after a successful mq_enqueue(), after a successful mq_confirm()
 * and after a message is retired. It is not called for a delivery attempt that
 * leaves the occupancy unchanged, so it never fires from the timeout path. */
void mq_set_change_hook(mq_change_fn fn, void *ctx);

/* Visit every stored message exactly once, oldest first (FIFO order).
 *
 * Preconditions : fn != NULL. ctx is passed through untouched.
 * Postconditions: fn has been called once per occupied slot; the queue is
 *                 unmodified, so the callback must not mutate it.
 */
void mq_foreach(mq_visit_fn fn, void *ctx);

/* Copy the current counters. */
void mq_stats_get(mq_stats *out);

#endif /* MSG_QUEUE_H */
