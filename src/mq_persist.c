/*  mq_persist.c
 *
 *  Durable mirror of the delivery queue. The contract of every entry point and
 *  the on-disk layout are documented in mq_persist.h.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mq_persist.h"
#include "msg_queue.h"

/* Format identity. */
static const char MQ_PERSIST_MAGIC[8] = { 'M', 'Q', 'P', 'Q', 'U', 'E', 'U', 'E' };

#define MQ_PERSIST_VERSION      1u
#define MQ_PERSIST_HEADER_SIZE  20u    /* magic 8 + version 4 + count 4 + pad 4 */
#define MQ_PERSIST_RECORD_HEAD  8u     /* friend number 4 + payload length 4    */

/* ------------------------------------------------------------------------- */
/* Little-endian scalar helpers                                              */
/*                                                                           */
/* The on-disk format is byte order independent, so a queue written on one    */
/* host is readable on another. All integers are encoded explicitly instead   */
/* of being dumped in host representation.                                    */
/* ------------------------------------------------------------------------- */

static void le_put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t) (value & 0xffu);
    out[1] = (uint8_t) ((value >> 8) & 0xffu);
    out[2] = (uint8_t) ((value >> 16) & 0xffu);
    out[3] = (uint8_t) ((value >> 24) & 0xffu);
}

static uint32_t le_get_u32(const uint8_t *in)
{
    return ((uint32_t) in[0])
         | ((uint32_t) in[1] << 8)
         | ((uint32_t) in[2] << 16)
         | ((uint32_t) in[3] << 24);
}

static void le_put_i32(uint8_t *out, int32_t value)
{
    le_put_u32(out, (uint32_t) value);
}

static int32_t le_get_i32(const uint8_t *in)
{
    return (int32_t) le_get_u32(in);
}

/* ------------------------------------------------------------------------- */
/* Saving                                                                    */
/* ------------------------------------------------------------------------- */

/* Flush the directory entry that rename() created.
 *
 * fsync()-ing the file makes its *contents* durable, but the name that points at
 * it is an update to the containing directory, which lives in a different place
 * on the storage device. Without this step a crash shortly after a successful
 * save can leave the old name (or no name) in place even though the new file is
 * complete -- which would resurrect the previous queue on the next start.
 *
 * Best effort by design: some platforms and file systems reject fsync() on a
 * directory, and when they do the rename has still happened, so this must not
 * turn a successful save into a failure. */
static void mq_sync_parent_dir(const char *path)
{
    char dir[PATH_MAX];
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        snprintf(dir, sizeof(dir), ".");
    } else if (slash == path) {
        snprintf(dir, sizeof(dir), "/");
    } else {
        const size_t len = (size_t) (slash - path);

        if (len >= sizeof(dir)) {
            return;
        }

        memcpy(dir, path, len);
        dir[len] = '\0';
    }

    const int fd = open(dir, O_RDONLY);

    if (fd < 0) {
        return;
    }

    (void) fsync(fd);
    (void) close(fd);
}

typedef struct {
    FILE *fp;
    bool  failed;
} mq_save_ctx;

static void mq_save_visit(void *raw_ctx, int friendnum, const char *payload, size_t len)
{
    mq_save_ctx *ctx = (mq_save_ctx *) raw_ctx;

    if (ctx->failed) {
        return;
    }

    /* friendnum is >= 0 and len < MQ_PAYLOAD_MAX for every queued message, so
     * neither cast can change the value. */
    uint8_t head[MQ_PERSIST_RECORD_HEAD];
    le_put_i32(head, friendnum);
    le_put_u32(head + 4, (uint32_t) len);

    if (fwrite(head, 1, sizeof(head), ctx->fp) != sizeof(head)) {
        ctx->failed = true;
        return;
    }

    if (fwrite(payload, 1, len, ctx->fp) != len) {
        ctx->failed = true;
    }
}

int mq_persist_save(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return MQ_PERSIST_ERR_PATH;
    }

    char tmp[PATH_MAX];

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) {
        return MQ_PERSIST_ERR_PATH;
    }

    FILE *fp = fopen(tmp, "wb");

    if (fp == NULL) {
        return MQ_PERSIST_ERR_IO;
    }

    uint8_t header[MQ_PERSIST_HEADER_SIZE];
    memcpy(header, MQ_PERSIST_MAGIC, sizeof(MQ_PERSIST_MAGIC));
    le_put_u32(header + 8, MQ_PERSIST_VERSION);
    le_put_u32(header + 12, (uint32_t) mq_count());
    le_put_u32(header + 16, 0u);

    bool ok = fwrite(header, 1, sizeof(header), fp) == sizeof(header);

    mq_save_ctx ctx = { fp, false };

    if (ok) {
        mq_foreach(mq_save_visit, &ctx);
        ok = !ctx.failed;
    }

    /* Push the bytes to the storage device before the rename, so the swap
     * cannot expose an empty or half-written file after a crash. */
    if (ok && fflush(fp) != 0) {
        ok = false;
    }

    if (ok && fsync(fileno(fp)) != 0) {
        ok = false;
    }

    if (fclose(fp) != 0) {
        ok = false;
    }

    if (!ok) {
        (void) unlink(tmp);
        return MQ_PERSIST_ERR_IO;
    }

    if (rename(tmp, path) != 0) {
        (void) unlink(tmp);
        return MQ_PERSIST_ERR_IO;
    }

    /* The file now has a new name; make that name durable too. */
    mq_sync_parent_dir(path);

    return MQ_PERSIST_OK;
}

/* ------------------------------------------------------------------------- */
/* Loading                                                                   */
/* ------------------------------------------------------------------------- */

/* Read the whole file into a fresh buffer.
 *
 * @return MQ_PERSIST_OK with *out and *size filled in on success, otherwise an
 *         error code. *out is NULL when no buffer was allocated. */
static int mq_read_all(const char *path, uint8_t **out, size_t *size)
{
    *out = NULL;
    *size = 0;

    FILE *fp = fopen(path, "rb");

    if (fp == NULL) {
        return (errno == ENOENT) ? MQ_PERSIST_ERR_NOFILE : MQ_PERSIST_ERR_IO;
    }

    int rc = MQ_PERSIST_ERR_IO;

    if (fseek(fp, 0, SEEK_END) == 0) {
        const long end = ftell(fp);

        if (end >= 0 && fseek(fp, 0, SEEK_SET) == 0) {
            const size_t length = (size_t) end;

            if (length < MQ_PERSIST_HEADER_SIZE) {
                rc = MQ_PERSIST_ERR_FORMAT;
            } else {
                uint8_t *buf = malloc(length);

                if (buf == NULL) {
                    rc = MQ_PERSIST_ERR_IO;
                } else if (fread(buf, 1, length, fp) != length) {
                    free(buf);
                    rc = MQ_PERSIST_ERR_IO;
                } else {
                    *out = buf;
                    *size = length;
                    rc = MQ_PERSIST_OK;
                }
            }
        }
    }

    if (fclose(fp) != 0 && rc == MQ_PERSIST_OK) {
        free(*out);
        *out = NULL;
        *size = 0;
        rc = MQ_PERSIST_ERR_IO;
    }

    return rc;
}

/* Walk the records once to prove they are all well formed. Nothing is enqueued
 * here, which is what makes the import all-or-nothing. */
static bool mq_validate_records(const uint8_t *buf, size_t size, uint32_t count)
{
    size_t off = MQ_PERSIST_HEADER_SIZE;

    for (uint32_t i = 0; i < count; ++i) {
        if (off + MQ_PERSIST_RECORD_HEAD > size) {
            return false;
        }

        const int32_t friendnum = le_get_i32(buf + off);
        const uint32_t len = le_get_u32(buf + off + 4);
        off += MQ_PERSIST_RECORD_HEAD;

        if (friendnum < 0 || len == 0 || len >= MQ_PAYLOAD_MAX) {
            return false;
        }

        if (off + len > size) {
            return false;
        }

        off += len;
    }

    return true;
}

int mq_persist_load(const char *path, time_t now)
{
    if (path == NULL || path[0] == '\0') {
        return MQ_PERSIST_ERR_PATH;
    }

    uint8_t *buf = NULL;
    size_t size = 0;

    const int file_rc = mq_read_all(path, &buf, &size);

    if (file_rc != MQ_PERSIST_OK) {
        return file_rc;
    }

    if (memcmp(buf, MQ_PERSIST_MAGIC, sizeof(MQ_PERSIST_MAGIC)) != 0
            || le_get_u32(buf + 8) != MQ_PERSIST_VERSION) {
        free(buf);
        return MQ_PERSIST_ERR_FORMAT;
    }

    const uint32_t count = le_get_u32(buf + 12);

    if (count > MQ_CAPACITY || !mq_validate_records(buf, size, count)) {
        free(buf);
        return MQ_PERSIST_ERR_FORMAT;
    }

    /* Every record is well formed, so importing cannot fail on malformed data;
     * the only remaining refusal is a queue that has no room. */
    size_t off = MQ_PERSIST_HEADER_SIZE;
    int loaded = 0;
    int result = MQ_PERSIST_OK;

    for (uint32_t i = 0; i < count; ++i) {
        const int32_t friendnum = le_get_i32(buf + off);
        const uint32_t len = le_get_u32(buf + off + 4);
        off += MQ_PERSIST_RECORD_HEAD;

        const int enqueued = mq_enqueue((int) friendnum,
                                        (const char *) (buf + off), len, now);
        off += len;

        if (enqueued == MQ_OK) {
            loaded++;
        } else if (enqueued == MQ_ERR_FULL) {
            result = MQ_PERSIST_ERR_FULL;
            break;
        } else {
            /* Only possible if the validation above drifted from the queue's
             * own preconditions; treat it as a corrupt file. */
            result = MQ_PERSIST_ERR_FORMAT;
            break;
        }
    }

    free(buf);

    return (result == MQ_PERSIST_OK) ? loaded : result;
}

