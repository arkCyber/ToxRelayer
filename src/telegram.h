/*  telegram.h
 *
 *  metaRelayer telegraph protocol: encoding, decoding and receive-sequence
 *  tracking for the "ZCZC ... NNNN" wire format used between relay nodes.
 *
 *  Wire format (fields separated by '\n', record terminated by NNNN):
 *
 *      ZCZC TEXT                  ZCZC CMD
 *      <receiver public key>      <receiver public key>
 *      <sender public key>        <sender public key>
 *      <sn> <unix timestamp>      R:<from>-<to>
 *      <message body>             NNNN
 *      NNNN
 *
 *  Design notes
 *  ------------
 *  - Pure functions over caller-owned buffers: no globals, no toxcore, no I/O.
 *    The module is therefore fully testable and its behaviour is deterministic.
 *  - Every decode is bounds-checked; a malformed telegram yields TG_INVALID and
 *    never reads past `len`.
 *  - Serial numbers live in 1..999 and wrap; all comparisons are wrap aware,
 *    which the previous inline code did not handle.
 */
#ifndef TELEGRAM_H
#define TELEGRAM_H

#include <stdbool.h>
#include <stddef.h>

/* Field capacities. */
#define TG_FIELD_MAX   128
#define TG_BODY_MAX    1024

/* Serial numbers are exchanged modulo TG_SN_MAX, in the range 1..TG_SN_MAX. */
#define TG_SN_MIN      1
#define TG_SN_MAX      999
#define TG_SN_RANGE    (TG_SN_MAX - TG_SN_MIN + 1)

typedef enum {
    TG_INVALID = 0,
    TG_TEXT,
    TG_CMD
} tg_kind;

typedef struct {
    tg_kind kind;
    char    receiver[TG_FIELD_MAX];
    char    sender[TG_FIELD_MAX];
    int     sn;          /* TEXT: message serial; CMD: first requested serial */
    int     sn_to;       /* CMD: last requested serial; 0 when unused        */
    long    timestamp;   /* TEXT: sender timestamp; 0 for CMD                */
    char    body[TG_BODY_MAX];
} tg_message;

/* --------------------------------------------------------------------------
 * Serial-number helpers (wrap aware, 1..TG_SN_MAX)
 * ------------------------------------------------------------------------ */

/* True when `sn` is a valid serial number. */
bool tg_sn_valid(int sn);

/* Next / previous serial number with wrap-around. */
int tg_sn_next(int sn);
int tg_sn_prev(int sn);

/* Number of forward steps from `from` to `to`, in 0..TG_SN_RANGE-1. */
int tg_sn_distance(int from, int to);

/* --------------------------------------------------------------------------
 * Encoding
 *
 * All encoders are NUL terminating and never write more than `out_size` bytes.
 * ------------------------------------------------------------------------ */

/* Serialize a TEXT telegram.
 *
 * Preconditions : out != NULL, out_size > 0, receiver/sender are non-NULL and
 *                 fit in TG_FIELD_MAX-1 bytes, 1 <= sn <= TG_SN_MAX, body may
 *                 be NULL (treated as empty).
 * Postconditions: on success *out holds a NUL-terminated telegram.
 *
 * @return the number of bytes written (excluding the NUL), or a negative
 *         TG_ERR_* code.
 */
int tg_encode_text(char *out, size_t out_size, const char *receiver, const char *sender,
                   int sn, long timestamp, const char *body);

/* Serialize a control telegram requesting the serial range [from, to].
 *
 * @return bytes written, or a negative TG_ERR_* code.
 */
int tg_encode_cmd(char *out, size_t out_size, const char *receiver, const char *sender,
                  int from, int to);

/* Error codes shared by the encoders. */
#define TG_OK             0
#define TG_ERR_ARG      (-1)   /* NULL or otherwise unusable argument   */
#define TG_ERR_RANGE    (-2)   /* serial number or range out of bounds  */
#define TG_ERR_TOOSMALL (-3)   /* destination buffer cannot hold result */
#define TG_ERR_FORMAT   (-4)   /* field too long for the wire format    */

/* --------------------------------------------------------------------------
 * Decoding
 * ------------------------------------------------------------------------ */

/* Parse a telegram that occupies the first `len` bytes of `raw`.
 *
 * Preconditions : raw != NULL, out != NULL. `raw` need not be NUL terminated;
 *                 decoding never reads past raw[len-1].
 * Postconditions: on TG_TEXT / TG_CMD, every string field in *out is
 *                 NUL terminated. On TG_INVALID the contents of *out are
 *                 unspecified.
 *
 * @return the decoded kind.
 */
tg_kind tg_decode(const char *raw, size_t len, tg_message *out);

/* --------------------------------------------------------------------------
 * Receive sequence tracking
 * ------------------------------------------------------------------------ */

typedef enum {
    SN_IN_ORDER = 0,   /* the serial is the one we expected                */
    SN_DUPLICATE,      /* the serial was already seen                      */
    SN_GAP,            /* a recoverable gap: request [gap_from, gap_to]    */
    SN_RESYNC,         /* the gap is too large to recover: resynchronise   */
    SN_RECOVERED       /* a telegram we had asked for has now arrived      */
} sn_result;

/* Largest number of outstanding missing telegrams that can be tracked. Must be
 * at least as large as the `max_gap` a caller passes to sn_tracker_accept(). */
#define SN_MAX_PENDING 256

typedef struct {
    int     expected;     /* next serial we expect, in 1..TG_SN_MAX */
    bool    synced;       /* false until the first serial is accepted */
    /* Serials reported as missing and not yet seen again. Without this set a
     * replayed telegram would arrive "behind" `expected` and be mistaken for a
     * duplicate, so the replay would be discarded and the gap never closed. */
    int     pending[SN_MAX_PENDING];
    size_t  pending_count;
} sn_tracker;

/* Prepare a tracker. `first_expected` must be a valid serial; when it is not,
 * TG_SN_MIN is assumed. */
void sn_tracker_init(sn_tracker *t, int first_expected);

/* Resume a tracker after a restart.
 *
 * The next expected serial becomes one past `last_received`, and the tracker is
 * marked synchronised, so that the first telegram of the new session is not
 * mistaken for a gap. When `last_received` is not a valid serial the tracker is
 * merely reset to its initial state.
 */
void sn_tracker_resume(sn_tracker *t, int last_received);

/* Account for an arriving serial number.
 *
 * Preconditions : t != NULL, tg_sn_valid(sn), max_gap >= 0 where `max_gap` is
 *                 the largest number of missing telegrams that is still worth
 *                 recovering by replay.
 * Postconditions: t->expected always ends up one step past the highest serial
 *                 accepted so far, wrapping as needed, except when the input is
 *                 invalid (then the tracker is unchanged). On SN_GAP, *gap_from
 *                 and *gap_to describe the missing inclusive range, which holds
 *                 exactly tg_sn_distance(previous_expected, sn) serials.
 *
 * @return the classification of the arrival. SN_GAP means the caller should ask
 *         the peer to replay [gap_from, gap_to]; the replayed telegrams are then
 *         reported as SN_RECOVERED instead of being mistaken for duplicates.
 */
sn_result sn_tracker_accept(sn_tracker *t, int sn, int max_gap,
                            int *gap_from, int *gap_to);

#endif /* TELEGRAM_H */
