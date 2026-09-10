/*  misc.h
 *
 */

#ifndef MISC_H
#define MISC_H

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

#include <inttypes.h>
#include <sys/types.h>
#include <stdbool.h>
#include <time.h>
#include <tox/tox.h>

#define UNKNOWN_NAME        "Anonymous       "
#define DEFAULT_TOX_NAME    "alice@metaMsg   "      /* should always be the same as toxcore's default name */

#define MAX_STR_SIZE                TOX_MAX_MESSAGE_LENGTH         
                                                    /* must be >= TOX_MAX_MESSAGE_LENGTH */
#define MAX_CMDNAME_SIZE            64
#define TOXIC_MAX_NAME_LENGTH       256             /* Must be <= TOX_MAX_NAME_LENGTH */


bool timed_out(time_t timestamp, time_t curtime, uint64_t timeout);

/* Returns current unix timestamp */
time_t get_time(void);

/* Returns current unix timestamp (alias of get_time(), kept for the callers
 * that predate it). */
time_t get_unix_time(void);

/* Returns true when `nick` is a name the bot is willing to use or accept:
 * non empty, no leading space, no contiguous spaces and no control characters
 * or '/'. */
bool valid_nick(const char *nick);

/* Parse a complete signed decimal integer and range-check it.
 *
 * This replaces the atoi()/atol() calls this code used to make. Those return 0
 * for both "0" and "not a number", and their result on overflow is undefined, so
 * a caller that forgot one of the follow-up checks silently accepted garbage --
 * a malformed console command, or a hand-edited INI value, could turn into an
 * arbitrary group number or serial. Here every rejection is explicit:
 *
 *   - text must be non-NULL, non-empty and consist of an optional sign followed
 *     by one or more decimal digits, and nothing else;
 *   - the value must fit in a `long` (otherwise strtol() sets ERANGE);
 *   - the value must lie in [min, max].
 *
 * Preconditions : min <= max, out != NULL.
 * Postconditions: on success *out holds the parsed value. On any failure *out is
 *                 left exactly as the caller passed it, so a rejected input can
 *                 never overwrite a previously valid setting.
 *
 * @return true when the whole string was parsed and is in range.
 */
bool parse_int_range(const char *text, long min, long max, long *out);

/* converts hexidecimal string to binary */
char *hex_string_to_bin(const char *hex_string);

/* returns file size or 0 on error */
off_t file_size(const char *path);

/* Return true if a file exists at `path`. */
bool file_exists(const char *path);

/* copies data to msg buffer.
   returns length of msg, which will be no larger than size-1 */
uint16_t copy_tox_str(char *msg, size_t size, const char *data, uint16_t length);

/* Copy a quoted command argument into `out`, dropping the surrounding quotes.
 *
 * The callers have already checked that `arg` starts with a quote. The closing
 * quote is stripped only when one is present and the copy length is derived from
 * the string, so an argument that carries only an opening quote is copied
 * safely instead of causing a write one byte before `out`.
 *
 * Preconditions : arg and out may be NULL, out_size may be 0; all are tolerated.
 * Postconditions: when out_size > 0, *out is NUL terminated and holds at most
 *                 out_size-1 bytes of the unquoted argument.
 */
void unquote_str(const char *arg, char *out, size_t out_size);

/* returns index of the first instance of ch in s starting at idx.
   returns length of s if char not found */
int char_find(int idx, const char *s, char ch);

/* Converts seconds to string in format days hours minutes */
void get_elapsed_time_str(char *buf, int bufsize, uint64_t secs);

/*
 * Searches plain text file pointed to by path for lines that match public_key.
 *
 * Returns 1 if a match is found.
 * Returns 0 if a match is not found.
 * Returns -1 on file operation failure.
 *
 * public_key must be a binary representation of a Tox public key.
 */
int file_contains_key(const char *public_key, const char *path);

void get_time_str(char *buf, size_t bufsize);

int get_conference_nick_truncate(Tox *m, char *buf, uint32_t peernum, uint32_t conferencenum);

void filter_str(char *str, size_t len);

#endif /* MISC_H */

