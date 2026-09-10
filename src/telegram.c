/*  telegram.c
 *
 *  metaRelayer telegraph protocol: encoding, decoding and receive-sequence
 *  tracking. The contract of every entry point is documented in telegram.h.
 *
 *  This module is deliberately free of toxcore, globals and I/O so that its
 *  behaviour can be verified exhaustively by unit tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telegram.h"

#define TG_TERMINATOR "NNNN"

/* ------------------------------------------------------------------------- */
/* Serial-number helpers                                                     */
/* ------------------------------------------------------------------------- */

bool tg_sn_valid(int sn)
{
    return sn >= TG_SN_MIN && sn <= TG_SN_MAX;
}

int tg_sn_next(int sn)
{
    if (!tg_sn_valid(sn) || sn == TG_SN_MAX) {
        return TG_SN_MIN;
    }

    return sn + 1;
}

int tg_sn_prev(int sn)
{
    if (!tg_sn_valid(sn) || sn == TG_SN_MIN) {
        return TG_SN_MAX;
    }

    return sn - 1;
}

int tg_sn_distance(int from, int to)
{
    int d = (to - from) % TG_SN_RANGE;

    if (d < 0) {
        d += TG_SN_RANGE;
    }

    return d;
}

/* ------------------------------------------------------------------------- */
/* Bounded string builder                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;        /* total bytes available, including the NUL */
    size_t len;        /* bytes written so far, excluding the NUL  */
    bool   overflow;
} tg_buf;

static void tg_buf_init(tg_buf *b, char *buf, size_t cap)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->overflow = false;

    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void tg_buf_putn(tg_buf *b, const char *src, size_t n)
{
    if (b->overflow || b->cap == 0) {
        return;
    }

    if (b->len + n + 1 > b->cap) {
        b->overflow = true;
        return;
    }

    memcpy(b->buf + b->len, src, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void tg_buf_put(tg_buf *b, const char *src)
{
    tg_buf_putn(b, src, strlen(src));
}

static void tg_buf_putc(tg_buf *b, char c)
{
    tg_buf_putn(b, &c, 1);
}

/* ------------------------------------------------------------------------- */
/* Field validation                                                          */
/* ------------------------------------------------------------------------- */

/* A wire field must be non-empty, fit the field width and contain no line
 * separator: a newline inside a key field would allow one peer to forge the
 * framing of another field. */
static bool tg_field_ok(const char *field)
{
    if (field == NULL || field[0] == '\0') {
        return false;
    }

    const size_t len = strlen(field);

    if (len >= TG_FIELD_MAX) {
        return false;
    }

    return strchr(field, '\n') == NULL && strchr(field, '\r') == NULL;
}

/* A body must not contain a line ending in the terminator, otherwise the
 * receiver cannot tell payload from framing. */
static bool tg_body_ok(const char *body)
{
    if (body == NULL) {
        return true;
    }

    const size_t tlen = strlen(TG_TERMINATOR);
    const char *p = body;
    const char *line_start = body;

    for (;;) {
        if (*p == '\n' || *p == '\0') {
            const size_t line_len = (size_t)(p - line_start);

            if (line_len >= tlen &&
                strncmp(p - tlen, TG_TERMINATOR, tlen) == 0) {
                return false;
            }

            if (*p == '\0') {
                break;
            }

            line_start = p + 1;
        }

        ++p;
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* Encoding                                                                  */
/* ------------------------------------------------------------------------- */

int tg_encode_text(char *out, size_t out_size, const char *receiver, const char *sender,
                   int sn, long timestamp, const char *body)
{
    if (out == NULL || out_size == 0) {
        return TG_ERR_ARG;
    }

    out[0] = '\0';

    if (!tg_field_ok(receiver) || !tg_field_ok(sender)) {
        return TG_ERR_FORMAT;
    }

    if (!tg_sn_valid(sn)) {
        return TG_ERR_RANGE;
    }

    if (!tg_body_ok(body)) {
        return TG_ERR_FORMAT;
    }

    tg_buf b;
    tg_buf_init(&b, out, out_size);

    tg_buf_put(&b, "ZCZC TEXT\n");
    tg_buf_put(&b, receiver);
    tg_buf_putc(&b, '\n');
    tg_buf_put(&b, sender);
    tg_buf_putc(&b, '\n');

    char meta[32];
    snprintf(meta, sizeof(meta), "%04d %ld\n", sn, timestamp);
    tg_buf_put(&b, meta);

    if (body != NULL && body[0] != '\0') {
        /* The terminator follows the body directly; the body keeps exactly the
         * bytes the caller supplied, which makes decode(encode(x)) == x for
         * every accepted x and matches the framing of earlier builds. */
        tg_buf_put(&b, body);
    }

    tg_buf_put(&b, TG_TERMINATOR "\n");

    if (b.overflow) {
        out[0] = '\0';
        return TG_ERR_TOOSMALL;
    }

    return (int) b.len;
}

int tg_encode_cmd(char *out, size_t out_size, const char *receiver, const char *sender,
                  int from, int to)
{
    if (out == NULL || out_size == 0) {
        return TG_ERR_ARG;
    }

    out[0] = '\0';

    if (!tg_field_ok(receiver) || !tg_field_ok(sender)) {
        return TG_ERR_FORMAT;
    }

    if (!tg_sn_valid(from) || !tg_sn_valid(to)) {
        return TG_ERR_RANGE;
    }

    tg_buf b;
    tg_buf_init(&b, out, out_size);

    tg_buf_put(&b, "ZCZC CMD\n");
    tg_buf_put(&b, receiver);
    tg_buf_putc(&b, '\n');
    tg_buf_put(&b, sender);
    tg_buf_putc(&b, '\n');

    char req[32];
    snprintf(req, sizeof(req), "R:%d-%d\n", from, to);
    tg_buf_put(&b, req);

    tg_buf_put(&b, TG_TERMINATOR "\n");

    if (b.overflow) {
        out[0] = '\0';
        return TG_ERR_TOOSMALL;
    }

    return (int) b.len;
}

/* ------------------------------------------------------------------------- */
/* Decoding                                                                  */
/* ------------------------------------------------------------------------- */

/* Line cursor over a raw buffer that need not be NUL terminated. */
typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
} tg_cursor;

/* Read the next line into dst.
 *
 * `complete` is cleared when the line did not fit, which the decoder treats as
 * a malformed telegram rather than silently truncating data.
 *
 * @return false when the input is exhausted.
 */
static bool tg_next_line(tg_cursor *c, char *dst, size_t dst_size, bool *complete)
{
    if (c->pos >= c->len || dst_size == 0) {
        return false;
    }

    size_t i = 0;
    bool truncated = false;

    while (c->pos < c->len && c->data[c->pos] != '\n' && c->data[c->pos] != '\r') {
        if (i + 1 < dst_size) {
            dst[i++] = c->data[c->pos];
        } else {
            truncated = true;
        }

        c->pos++;
    }

    dst[i] = '\0';

    if (c->pos < c->len && c->data[c->pos] == '\r') {
        c->pos++;
    }

    if (c->pos < c->len && c->data[c->pos] == '\n') {
        c->pos++;
    }

    if (complete != NULL) {
        *complete = !truncated;
    }

    return true;
}

tg_kind tg_decode(const char *raw, size_t len, tg_message *out)
{
    if (raw == NULL || out == NULL || len == 0) {
        return TG_INVALID;
    }

    memset(out, 0, sizeof(*out));

    tg_cursor c = { raw, len, 0 };
    char line[TG_FIELD_MAX + 16];
    bool complete = false;

    if (!tg_next_line(&c, line, sizeof(line), &complete) || !complete) {
        return TG_INVALID;
    }

    if (strcmp(line, "ZCZC TEXT") == 0) {
        out->kind = TG_TEXT;
    } else if (strcmp(line, "ZCZC CMD") == 0) {
        out->kind = TG_CMD;
    } else {
        return TG_INVALID;
    }

    /* Receiver */
    if (!tg_next_line(&c, line, sizeof(line), &complete) || !complete) {
        return TG_INVALID;
    }

    if (line[0] == '\0' || strlen(line) >= sizeof(out->receiver)) {
        return TG_INVALID;
    }

    memcpy(out->receiver, line, strlen(line) + 1);

    /* Sender */
    if (!tg_next_line(&c, line, sizeof(line), &complete) || !complete) {
        return TG_INVALID;
    }

    if (line[0] == '\0' || strlen(line) >= sizeof(out->sender)) {
        return TG_INVALID;
    }

    memcpy(out->sender, line, strlen(line) + 1);

    /* Metadata line */
    if (!tg_next_line(&c, line, sizeof(line), &complete) || !complete) {
        return TG_INVALID;
    }

    if (out->kind == TG_TEXT) {
        char *end = NULL;
        const long sn = strtol(line, &end, 10);

        if (end == line || *end != ' ') {
            return TG_INVALID;
        }

        char *ts_end = NULL;
        const long ts = strtol(end + 1, &ts_end, 10);

        if (ts_end == end + 1) {
            return TG_INVALID;
        }

        if (!tg_sn_valid((int) sn)) {
            return TG_INVALID;
        }

        out->sn = (int) sn;
        out->timestamp = ts;
    } else {
        if (line[0] != 'R' || line[1] != ':') {
            return TG_INVALID;
        }

        char *end = NULL;
        const long from = strtol(line + 2, &end, 10);

        if (end == line + 2 || *end != '-') {
            return TG_INVALID;
        }

        char *to_end = NULL;
        const long to = strtol(end + 1, &to_end, 10);

        if (to_end == end + 1) {
            return TG_INVALID;
        }

        if (!tg_sn_valid((int) from) || !tg_sn_valid((int) to)) {
            return TG_INVALID;
        }

        out->sn = (int) from;
        out->sn_to = (int) to;
    }

    /* Body, up to and including the terminator. */
    size_t body_len = 0;
    bool terminated = false;

    while (tg_next_line(&c, line, sizeof(line), &complete)) {
        const size_t line_len = strlen(line);

        /* A line ending in the terminator closes the record. Accepting a suffix
         * rather than requiring a line of exactly "NNNN" keeps compatibility
         * with telegrams written by earlier builds. */
        if (line_len >= 4 && strcmp(line + line_len - 4, "NNNN") == 0) {
            const size_t keep = line_len - 4;

            if (body_len + keep + 1 >= sizeof(out->body)) {
                return TG_INVALID;
            }

            memcpy(out->body + body_len, line, keep);
            body_len += keep;
            out->body[body_len] = '\0';

            terminated = true;
            break;
        }

        if (body_len + line_len + 1 >= sizeof(out->body)) {
            return TG_INVALID;   /* refuse to store a silently truncated message */
        }

        memcpy(out->body + body_len, line, line_len);
        body_len += line_len;
        out->body[body_len++] = '\n';
        out->body[body_len] = '\0';
    }

    if (!terminated) {
        return TG_INVALID;
    }

    return out->kind;
}

/* ------------------------------------------------------------------------- */
/* Receive sequence tracking                                                 */
/* ------------------------------------------------------------------------- */

void sn_tracker_init(sn_tracker *t, int first_expected)
{
    if (t == NULL) {
        return;
    }

    t->expected = tg_sn_valid(first_expected) ? first_expected : TG_SN_MIN;
    t->synced = false;
    t->pending_count = 0;
}

void sn_tracker_resume(sn_tracker *t, int last_received)
{
    if (t == NULL) {
        return;
    }

    if (!tg_sn_valid(last_received)) {
        sn_tracker_init(t, TG_SN_MIN);
        return;
    }

    sn_tracker_init(t, tg_sn_next(last_received));
    t->synced = true;
}

/* Remove `sn` from the outstanding set.
 *
 * @return true when it was present, i.e. this is a telegram we had asked for. */
static bool sn_pending_take(sn_tracker *t, int sn)
{
    for (size_t i = 0; i < t->pending_count; ++i) {
        if (t->pending[i] == sn) {
            t->pending[i] = t->pending[--t->pending_count];
            return true;
        }
    }

    return false;
}

/* Remember the serials in the inclusive range [from, to].
 *
 * @return false when the set is full, which the caller answers with a resync. */
static bool sn_pending_add_range(sn_tracker *t, int from, int to)
{
    if (t->pending_count >= SN_MAX_PENDING) {
        return false;
    }

    int sn = from;

    for (;;) {
        if (t->pending_count >= SN_MAX_PENDING) {
            return false;
        }

        t->pending[t->pending_count++] = sn;

        if (sn == to) {
            return true;
        }

        sn = tg_sn_next(sn);
    }
}

sn_result sn_tracker_accept(sn_tracker *t, int sn, int max_gap,
                            int *gap_from, int *gap_to)
{
    if (gap_from != NULL) {
        *gap_from = 0;
    }

    if (gap_to != NULL) {
        *gap_to = 0;
    }

    if (t == NULL || !tg_sn_valid(sn)) {
        return SN_RESYNC;
    }

    /* A telegram we previously asked the peer to replay. It arrives "behind"
     * `expected` by construction, so it must be recognised before the duplicate
     * test would reject it. */
    if (sn_pending_take(t, sn)) {
        return SN_RECOVERED;
    }

    if (!t->synced) {
        t->synced = true;
        t->expected = tg_sn_next(sn);
        return SN_IN_ORDER;
    }

    const int dist = tg_sn_distance(t->expected, sn);

    if (dist == 0) {
        t->expected = tg_sn_next(sn);
        return SN_IN_ORDER;
    }

    /* More than half the sequence space behind us: a replay, not a gap. */
    if (dist > TG_SN_RANGE / 2) {
        return SN_DUPLICATE;
    }

    /* `dist` is exactly the number of telegrams that never arrived: the serials
     * [expected .. sn-1]. They are recoverable only when they fit both the
     * caller's budget and the outstanding set. */
    if (dist <= max_gap &&
        sn_pending_add_range(t, t->expected, tg_sn_prev(sn))) {
        if (gap_from != NULL) {
            *gap_from = t->expected;
        }

        if (gap_to != NULL) {
            *gap_to = tg_sn_prev(sn);
        }

        t->expected = tg_sn_next(sn);
        return SN_GAP;
    }

    /* Too much missing to recover by replay: resynchronise silently. */
    t->pending_count = 0;
    t->expected = tg_sn_next(sn);
    return SN_RESYNC;
}
