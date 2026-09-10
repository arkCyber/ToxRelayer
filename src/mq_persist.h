/*  mq_persist.h
 *
 *  Durable mirror of the store-and-forward delivery queue.
 *
 *  Without this module the queue lives only in memory, so every telegram that
 *  was still waiting for its peer when the process stopped was lost on restart.
 *  This module writes the queue to a single file and reads it back.
 *
 *  Design constraints
 *  ------------------
 *  - No toxcore dependency and no global state of its own: it drives
 *    msg_queue.h through its public interface (mq_foreach, mq_count, mq_enqueue),
 *    so the file format is verified against the real queue in the test suite.
 *  - Atomic replace: the file is written to `<path>.tmp`, flushed to the
 *    storage device and only then renamed over `<path>`; the containing
 *    directory is flushed afterwards so the new name itself survives a crash.
 *    A crash at any point leaves either the previous queue or the new one,
 *    never a torn file.
 *  - Validate before import: the whole file is read and checked before a single
 *    record is enqueued, so a corrupt file cannot leave the queue half restored.
 *  - Every failure is reported through a return code; nothing here calls
 *    exit()/abort() and nothing prints.
 *
 *  On-disk format, version 1 (all integers little endian):
 *
 *      offset  size  field
 *      0       8     magic "MQPQUEUE"
 *      8       4     format version (1)
 *      12      4     record count
 *      16      4     reserved (0)
 *      20      ...   records, oldest first:
 *                      4  friend number (int32, >= 0)
 *                      4  payload length (uint32, 1 .. MQ_PAYLOAD_MAX-1)
 *                      n  payload bytes
 */
#ifndef MQ_PERSIST_H
#define MQ_PERSIST_H

#include <time.h>

/* Return codes. All are negative except MQ_PERSIST_OK. */
#define MQ_PERSIST_OK          0
#define MQ_PERSIST_ERR_PATH  (-1)  /* NULL, empty or over-long path        */
#define MQ_PERSIST_ERR_IO    (-2)  /* open / read / write / rename failure */
#define MQ_PERSIST_ERR_FORMAT (-3) /* bad magic, version or record fields  */
#define MQ_PERSIST_ERR_FULL  (-4)  /* the queue had no room to restore all */
#define MQ_PERSIST_ERR_NOFILE (-5) /* no stored queue: normal first start  */

/* Write the whole queue to `path`, replacing any previous file atomically.
 *
 * Preconditions : path is a non-NULL, non-empty path of less than PATH_MAX
 *                 bytes.
 * Postconditions: on MQ_PERSIST_OK `path` holds exactly the messages that were
 *                 in the queue, oldest first. On any error `path` is left as it
 *                 was and no partial `<path>.tmp` remains.
 *
 * @return MQ_PERSIST_OK, MQ_PERSIST_ERR_PATH or MQ_PERSIST_ERR_IO.
 */
int mq_persist_save(const char *path);

/* Restore the queue from `path`.
 *
 * The restore is additive: messages are appended to whatever the queue already
 * holds, so callers that want a fresh start call mq_init() first. Restored
 * messages start with a fresh attempt budget and become eligible for delivery
 * immediately, because a restart is treated as a new opportunity to reach the
 * peer.
 *
 * @return the number of messages restored (>= 0), or MQ_PERSIST_ERR_PATH,
 *         MQ_PERSIST_ERR_IO, MQ_PERSIST_ERR_FORMAT, MQ_PERSIST_ERR_FULL or
 *         MQ_PERSIST_ERR_NOFILE. A missing file is not an error.
 */
int mq_persist_load(const char *path, time_t now);

#endif /* MQ_PERSIST_H */
