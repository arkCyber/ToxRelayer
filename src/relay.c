/*  relay.c
 *
 *  metaRelayer routing and store-and-forward policy. See relay.h for the
 *  contract of every entry point and for why the environment is injected.
 */

#include <string.h>

#include "relay.h"

/* Largest telegram this module can build: the two key fields, the metadata line
 * and the maximum body, plus framing. It exceeds the largest possible telegram
 * by a comfortable margin. */
#define RELAY_WIRE_MAX 1400

/* Ask the peer on `channel` to replay the inclusive serial range [from, to].
 * The request travels through the same delivery path as ordinary traffic, so it
 * inherits the queue's retry policy. */
static relay_outcome relay_request_gap(relay_env *env, int channel,
                                       int from, int to, time_t now)
{
    if (env->enqueue == NULL || env->peer_key == NULL) {
        return RELAY_DISCARDED;
    }

    const char *peer = env->peer_key(env->ctx, channel);

    if (peer == NULL) {
        return RELAY_NO_PEER;
    }

    char wire[RELAY_WIRE_MAX];
    const int n = tg_encode_cmd(wire, sizeof(wire), peer, env->self_public_key, from, to);

    if (n <= 0) {
        return RELAY_DISCARDED;
    }

    if (env->enqueue(env->ctx, channel, wire, (size_t) n, now) != 0) {
        return RELAY_DISCARDED;
    }

    return RELAY_GAP_REQUESTED;
}

/* A telegram addressed to us: account for its serial and store it. */
static relay_outcome relay_deliver(relay_env *env, int channel,
                                   const tg_message *tg, time_t now)
{
    sn_tracker *tracker = (env->tracker != NULL) ? env->tracker(env->ctx, channel) : NULL;

    int gap_from = 0;
    int gap_to = 0;
    sn_result state = SN_IN_ORDER;

    if (tracker != NULL) {
        state = sn_tracker_accept(tracker, tg->sn, RELAY_MAX_GAP, &gap_from, &gap_to);
    }

    if (state == SN_DUPLICATE) {
        return RELAY_DUPLICATE;
    }

    if (env->store != NULL) {
        /* A persistence failure does not stop the sequence accounting: the
         * telegram did arrive, so a later gap must still be detectable. */
        (void) env->store(env->ctx, channel, tg->sn, tg->timestamp, tg->sender, tg->body);
    }

    if (env->remember_serial != NULL) {
        env->remember_serial(env->ctx, channel, tg->sn);
    }

    switch (state) {
    case SN_GAP:
        return (relay_request_gap(env, channel, gap_from, gap_to, now) == RELAY_GAP_REQUESTED)
                   ? RELAY_GAP_REQUESTED
                   : RELAY_STORED;

    case SN_RECOVERED:
        return RELAY_RECOVERED;

    case SN_RESYNC:
        return RELAY_RESYNCED;

    case SN_IN_ORDER:
    default:
        return RELAY_STORED;
    }
}

/* A telegram for somebody else: route it on. */
static relay_outcome relay_forward(relay_env *env, int channel,
                                   const char *raw, const tg_message *tg, time_t now)
{
    if (env->resolve_peer == NULL || env->enqueue == NULL) {
        return RELAY_DISCARDED;
    }

    const int destination = env->resolve_peer(env->ctx, tg->receiver);

    if (destination < 0) {
        return RELAY_NO_ROUTE;
    }

    /* A metaCom peer receives the record unchanged, so its framing and serial
     * number survive the hop; a plain peer receives only the body. */
    const bool metacom = (env->peer_is_metacom != NULL)
                             && env->peer_is_metacom(env->ctx, channel);

    const char *payload = metacom ? raw : tg->body;
    const size_t len = metacom ? strlen(raw) : strlen(tg->body);

    if (env->enqueue(env->ctx, destination, payload, len, now) != 0) {
        return RELAY_DISCARDED;
    }

    return RELAY_RELAYED;
}

/* A control telegram: replay what the peer reports it never received. */
static relay_outcome relay_serve_replay(relay_env *env, int channel,
                                        const tg_message *cmd, time_t now)
{
    if (env->fetch_range == NULL || env->enqueue == NULL || env->peer_key == NULL) {
        return RELAY_DISCARDED;
    }

    const char *peer = env->peer_key(env->ctx, channel);

    if (peer == NULL) {
        return RELAY_NO_PEER;
    }

    relay_record records[RELAY_MAX_REPLAY];
    const int n = env->fetch_range(env->ctx, channel, cmd->sn, cmd->sn_to,
                                   records, RELAY_MAX_REPLAY);

    if (n <= 0) {
        return RELAY_NOTHING_STORED;
    }

    int queued = 0;

    for (int i = 0; i < n; ++i) {
        /* Rebuild the telegram with the verified encoder. The database stores
         * bodies, so a replay is always regenerated in a well formed state
         * rather than forwarded as an opaque blob. */
        char wire[RELAY_WIRE_MAX];
        const int len = tg_encode_text(wire, sizeof(wire), peer, env->self_public_key,
                                       records[i].sn, records[i].timestamp,
                                       records[i].body);

        if (len <= 0) {
            continue;               /* unencodable: skipped, never sent malformed */
        }

        if (env->enqueue(env->ctx, channel, wire, (size_t) len, now) == 0) {
            queued++;
        }
    }

    return (queued > 0) ? RELAY_SERVED : RELAY_NOTHING_STORED;
}

relay_outcome relay_receive(relay_env *env, int channel,
                            const char *raw, size_t len, time_t now)
{
    if (env == NULL || env->self_public_key == NULL || raw == NULL || len == 0 || channel < 0) {
        return RELAY_DISCARDED;
    }

    tg_message tg;

    switch (tg_decode(raw, len, &tg)) {
    case TG_TEXT:
        return (strcmp(tg.receiver, env->self_public_key) == 0)
                   ? relay_deliver(env, channel, &tg, now)
                   : relay_forward(env, channel, raw, &tg, now);

    case TG_CMD:
        /* A control record addressed elsewhere is none of our business. */
        if (strcmp(tg.receiver, env->self_public_key) != 0) {
            return RELAY_DISCARDED;
        }

        return relay_serve_replay(env, channel, &tg, now);

    case TG_INVALID:
    default:
        return RELAY_DISCARDED;
    }
}

int relay_recover_trackers(relay_env *env, int (*last_sn)(void *ctx, int channel, int *out),
                           int channels)
{
    if (env == NULL || last_sn == NULL || env->tracker == NULL || channels <= 0) {
        return 0;
    }

    int recovered = 0;

    for (int channel = 0; channel < channels; ++channel) {
        if (env->channel_known != NULL && !env->channel_known(env->ctx, channel)) {
            continue;           /* not a channel of this deployment */
        }

        sn_tracker *tracker = env->tracker(env->ctx, channel);

        if (tracker == NULL) {
            continue;
        }

        int sn = 0;

        if (last_sn(env->ctx, channel, &sn) != 0) {
            sn_tracker_init(tracker, TG_SN_MIN);
            continue;
        }

        sn_tracker_resume(tracker, sn);

        if (tg_sn_valid(sn)) {
            recovered++;
        }
    }

    return recovered;
}
