/*  relay.h
 *
 *  metaRelayer routing and store-and-forward policy.
 *
 *  This module contains every decision the relay makes about a received record:
 *  whether it is for us or must be forwarded, whether it is a duplicate,
 *  whether a gap must be requested, and how a replay request is answered. It has
 *  no toxcore dependency and no global state: storage, queueing and the peer
 *  directory are supplied by the caller through relay_env.
 *
 *  That structure is deliberate. The policy is the part where defects hide, and
 *  decoupling it means the policy can be verified directly, with a fake
 *  environment or with the real message database and delivery queue.
 */
#ifndef RELAY_H
#define RELAY_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "telegram.h"

/* Largest gap worth recovering by replay. Beyond this the receive tracker
 * resynchronises instead of asking for a very large replay. */
#ifndef RELAY_MAX_GAP
#define RELAY_MAX_GAP 64
#endif

/* Largest number of telegrams one replay request is answered with. */
#ifndef RELAY_MAX_REPLAY
#define RELAY_MAX_REPLAY 64
#endif

/* Capacity of a stored telegram body. */
#ifndef RELAY_BODY_MAX
#define RELAY_BODY_MAX 1024
#endif

/* One stored telegram, as required to answer a replay request. */
typedef struct {
    int    sn;
    long   timestamp;
    char   body[RELAY_BODY_MAX];
} relay_record;

/* Everything the relay needs from its environment.
 *
 * The first three callbacks describe the peer directory, the next three the
 * store-and-forward machinery, and the last is an optional mirror of the
 * receive serial number. Any callback may be NULL where noted; a NULL is
 * treated as "not available" rather than as an error.
 */
typedef struct {
    /* Our own public key in hexadecimal. A telegram addressed to it is ours. */
    const char *self_public_key;

    /* Map a destination public key to a channel number, or -1 when unknown. */
    int (*resolve_peer)(void *ctx, const char *public_key_hex);

    /* Public key of the peer on `channel`, or NULL when unknown. Required for
     * forwarding and for answering a replay. */
    const char *(*peer_key)(void *ctx, int channel);

    /* True when the peer on `channel` speaks the metaCom protocol. */
    bool (*peer_is_metacom)(void *ctx, int channel);

    /* Receive tracker for `channel`, or NULL when there is none. */
    sn_tracker *(*tracker)(void *ctx, int channel);

    /* Optional filter used by relay_recover_trackers(): return false for a
     * channel that does not exist in this deployment. NULL means "every channel
     * in [0, channels) is a candidate", which is what a test or a fully dense
     * deployment wants. Its purpose is start-up cost: on a relay with a handful
     * of contacts, probing all MAX_Friend_NUM channels is a database query per
     * channel for nothing. */
    bool (*channel_known)(void *ctx, int channel);

    /* Store one received telegram. Must return 0 on success, as
     * msg_database_add() does. NULL disables persistence. */
    int (*store)(void *ctx, int channel, int sn, long timestamp,
                 const char *sender, const char *body);

    /* Enqueue one record for delivery. Must return 0 on success, as
     * mq_enqueue() does. */
    int (*enqueue)(void *ctx, int channel, const char *payload, size_t len, time_t now);

    /* Fetch stored outgoing telegrams for a replay, oldest first. Must return
     * the number written or a negative value, as msg_database_range() does. */
    int (*fetch_range)(void *ctx, int channel, int from, int to,
                       relay_record *out, int max);

    /* Optional mirror of the highest received serial (the INI file). */
    void (*remember_serial)(void *ctx, int channel, int sn);

    void *ctx;
} relay_env;

/* What the relay did with a record. */
typedef enum {
    RELAY_STORED = 0,      /* addressed to us and stored                     */
    RELAY_DUPLICATE,       /* addressed to us but already seen               */
    RELAY_GAP_REQUESTED,   /* addressed to us; a replay of the gap was asked */
    RELAY_RECOVERED,       /* addressed to us and it closed a known gap      */
    RELAY_RESYNCED,        /* addressed to us; the gap was too large         */
    RELAY_RELAYED,         /* forwarded to another channel                   */
    RELAY_NO_ROUTE,        /* forwarding was required but the peer is unknown */
    RELAY_NO_PEER,         /* the record could not be re-addressed           */
    RELAY_SERVED,          /* we answered a replay request                   */
    RELAY_NOTHING_STORED,  /* replay request with an empty result            */
    RELAY_DISCARDED        /* malformed, or not actionable                   */
} relay_outcome;

/* Handle one raw record received from `channel`.
 *
 * Preconditions : env != NULL, env->self_public_key != NULL, raw != NULL,
 *                 len > 0.
 * Postconditions: the record has been decoded and acted upon, or rejected.
 *                 Nothing is written to memory owned by this module.
 *
 * @return what was done, so the caller can log it.
 */
relay_outcome relay_receive(relay_env *env, int channel,
                            const char *raw, size_t len, time_t now);

/* Rebuild the receive trackers from stored history.
 *
 * For each channel in [0, channels) that env->channel_known() accepts (every
 * channel when the filter is NULL) the highest stored serial is read back and
 * the tracker is advanced past it, so the first telegram after a restart is not
 * mistaken for a gap.
 *
 * A channel the filter rejects is left untouched. That is safe: a tracker that
 * was never prepared is unsynchronised, and the first telegram it accepts is
 * treated as the start of the sequence whether or not sn_tracker_init() ran.
 *
 * @return the number of channels that had a stored serial.
 */
int relay_recover_trackers(relay_env *env, int (*last_sn)(void *ctx, int channel, int *out),
                           int channels);

#endif /* RELAY_H */
