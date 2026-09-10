/*  misc.c
 *
 *  This file is part of toxrelayer.
 */

#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

#include <tox/tox.h>
#include "misc.h"
#include "log.h"

bool valid_nick(const char *nick);
static bool is_valid_char(char ch);
void filter_str(char *str, size_t len);
time_t get_unix_time(void);
void filter_str(char *str, size_t len);

static const char invalid_chars[] = {'/', '\n', '\t', '\v', '\r', '\0'};

/*
    Returns true once `timeout` seconds have elapsed since `timestamp`.
 *
 *  Written as an elapsed-time comparison rather than `timestamp + timeout <=
 *  curtime`: the addition mixes a signed time_t with an unsigned timeout (which
 *  the compiler flags) and would also overflow for very large timeouts.
 */
bool timed_out(time_t timestamp, time_t curtime, uint64_t timeout)
{
    if (curtime <= timestamp) {
        return false;
    }

    return (uint64_t) (curtime - timestamp) >= timeout;
}
//
time_t get_time(void)
{
    return time(NULL);
}
//----------------------------------------------------------------
//
/* Value of a single hexadecimal digit, or -1 when the character is not one. */
static int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    return -1;
}

char *hex_string_to_bin(const char *hex_string)
{
    if (hex_string == NULL) {
        return NULL;
    }

    const size_t hex_len = strlen(hex_string);

    /* Only an even number of hex digits describes whole bytes; anything else
     * (an empty string, a stray newline, an odd length) is rejected instead of
     * being parsed past the end of the buffer. */
    if (hex_len == 0 || (hex_len % 2) != 0) {
        return NULL;
    }

    const size_t bin_len = hex_len / 2;
    uint8_t *val = malloc(bin_len);

    if (val == NULL) {
        return NULL;
    }

    /* Decoded by hand rather than with sscanf("%2x"): the indices are provably
     * inside the input, no format string is parsed at run time, and the result
     * does not depend on the locale. */
    for (size_t i = 0; i < bin_len; ++i) {
        const int hi = hex_digit_value(hex_string[i * 2]);
        const int lo = hex_digit_value(hex_string[(i * 2) + 1]);

        if (hi < 0 || lo < 0) {
            free(val);
            return NULL;
        }

        val[i] = (uint8_t) ((hi << 4) | lo);
    }

    return (char *) val;
}
//----------------------------------------------------------------
//
off_t file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) == -1) {
        return 0;
    }

    return st.st_size;
}
//----------------------------------------------------------------
//
bool file_exists(const char *path)
{
    struct stat s;
    return stat(path, &s) == 0;
}
//----------------------------------------------------------------
//
uint16_t copy_tox_str(char *msg, size_t size, const char *src_data, uint16_t length)
{
    int len = MIN(length, size - 1);
    memcpy( msg, src_data, len);
    msg[len] = '\0';
    return len;
}
//
/*  Returns the index of the first instance of `ch` in `s`, searching from `idx`.
 *
 *  Returns the index one past the last character when the character is not
 *  found, or `strlen(s)` when `idx` already lies beyond the string. An index
 *  beyond the string is not searched: the previous implementation evaluated
 *  s[idx] unconditionally and read past the end of the buffer.
 */
int char_find(int idx, const char *s, char ch)
{
    if (s == NULL || idx < 0) {
        return 0;
    }

    const size_t len = strlen(s);

    if ((size_t) idx >= len) {
        return (int) len;       /* nothing left to search */
    }

    int i = idx;

    for (i = idx; s[i] != '\0'; ++i) {
        if (s[i] == ch) {
            break;
        }
    }

    return i;
}
//----------------------------------------------------------------
//
void get_elapsed_time_str(char *buf, int bufsize, uint64_t secs)
{
    long unsigned int minutes = (secs % 3600) / 60;
    long unsigned int hours = (secs / 3600) % 24;
    long unsigned int days = (secs / 3600) / 24;

    snprintf(buf, bufsize, "%lud %luh %lum", days, hours, minutes);
}

/*
 * Searches plain text file pointed to by path for lines that match public_key.
 *
 * Returns 1 if a match is found.
 * Returns 0 if a match is not found.
 * Returns -1 on file operation failure.
 *
 * public_key must be a binary representation of a Tox public key.
 */
int file_contains_key(const char *public_key, const char *path)
{
    char id[256];
    struct stat s;

    if (public_key == NULL || path == NULL) {
        return -1;
    }

    if (stat(path, &s) != 0) {
        FILE *created = fopen(path, "w");

        if (created == NULL) {
            console_err("Warning: failed to create '%s' file\n", path);
            return -1;
        }

        console_err("Warning: creating new '%s' file. Did you lose the old one?\n", path);
        if (fclose(created) != 0) {
            console_err("Warning: failed to close '%s'\n", path);
        }
        return 0;
    }

    FILE *fp = fopen(path, "r");

    if (fp == NULL) {
        console_err("Warning: failed to read '%s' file\n", path);
        return -1;
    }

    while (fgets(id, sizeof(id), fp) != NULL) {
        /* The line terminator is not part of the key. Bound the index so
         * its range is provable rather than derived from the input. */
        const size_t line_len = strcspn(id, "\r\n");
        id[line_len < sizeof(id) ? line_len : sizeof(id) - 1] = '\0';

        if (strlen(id) < ((size_t) TOX_PUBLIC_KEY_SIZE * 2)) {
            continue;
        }

        char *key_bin = hex_string_to_bin(id);

        if (key_bin == NULL) {
            continue;                   /* malformed line: skip it */
        }

        const int match = (memcmp(key_bin, public_key, TOX_PUBLIC_KEY_SIZE) == 0);
        free(key_bin);

        if (match) {
            if (fclose(fp) != 0) {
                console_err("Warning: failed to close '%s'\n", path);
            }
            return 1;
        }
    }

    if (fclose(fp) != 0) {
        console_err("Warning: failed to close '%s'\n", path);
    }

    return 0;
}

#define TIMESTAMP_DEFAULT       "%H:%M"
#define LOG_TIMESTAMP_DEFAULT   "%Y/%m/%d [%H:%M:%S]"
/* 
    Puts the current time in buf in the format of specified by the config 
*/
void get_time_str(char *buf, size_t bufsize)
{
    if (buf == NULL || bufsize == 0) {
        return;
    }

    *buf = 0;

    time_t now = get_time();
    const struct tm *tm_info = localtime(&now);

    if (tm_info == NULL) {
        return;
    }

    /* strftime() returns 0 when the formatted result would not fit. In that case
     * the buffer is emptied rather than left holding an unspecified partial
     * string, so callers always see either a complete timestamp or "". */
    if (strftime(buf, bufsize, TIMESTAMP_DEFAULT, tm_info) == 0) {
        buf[0] = '\0';
    }
}
//
//  same as get_nick_truncate but for conferences 
//
int get_conference_nick_truncate(Tox *m, char *buf, uint32_t peernum, uint32_t conferencenum)
{
    Tox_Err_Conference_Peer_Query err;
    size_t len = tox_conference_peer_get_name_size(m, conferencenum, peernum, &err);

    if (err != TOX_ERR_CONFERENCE_PEER_QUERY_OK) {
        goto on_error;
    } else {
        if (!tox_conference_peer_get_name(m, conferencenum, peernum, (uint8_t *) buf, NULL)) {
            goto on_error;
        }
    }

    len = MIN(len, TOXIC_MAX_NAME_LENGTH - 1);
    buf[len] = '\0';
    filter_str(buf, len);
    return len;

on_error:
    /* Bounded: the contract of this function is that `buf` holds
     * TOXIC_MAX_NAME_LENGTH bytes, which is the same size the success path
     * writes up to. */
    copy_tox_str(buf, TOXIC_MAX_NAME_LENGTH, UNKNOWN_NAME,
                 (uint16_t) strlen(UNKNOWN_NAME));
    len = strlen(UNKNOWN_NAME);
    buf[len] = '\0';
    return len;
}
//
time_t get_unix_time(void)
{
    return time(NULL);
}

/* Parse a complete signed decimal integer and range-check it. The contract is
 * documented in misc.h; this is the single place in the code base that turns
 * operator or INI text into a number. */
bool parse_int_range(const char *text, long min, long max, long *out)
{
    if (text == NULL || out == NULL || min > max || text[0] == '\0') {
        return false;
    }

    /* strtol() would silently skip leading whitespace, which would make " 12"
     * and "12" two spellings of the same value. Rejecting it keeps the accepted
     * input set exact, so an operator who fat-fingers a trailing newline into an
     * INI value gets a rejection instead of a surprise. */
    if (isspace((unsigned char) text[0])) {
        return false;
    }

    /* strtol() reports overflow through errno and always leaves `end` pointing
     * at the first unconsumed character, which together let the whole string be
     * validated without a second pass. */
    errno = 0;

    char *end = NULL;
    const long value = strtol(text, &end, 10);

    if (end == text || *end != '\0' || errno == ERANGE) {
        return false;               /* empty, trailing junk, or overflow */
    }

    if (value < min || value > max) {
        return false;               /* well formed number, wrong range */
    }

    *out = value;
    return true;
}

//----------------------------------------------------------------
 
//----------------------------------------------------------------
//
// (get_wall_time was removed: it was defined but never called. log.c keeps its
//  own copy for the timestamped log helpers.)

/* Copy a quoted command argument into `out`, dropping the surrounding quotes.
 * The contract is documented in misc.h. */
void unquote_str(const char *arg, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    out[0] = '\0';

    if (arg == NULL) {
        return;
    }

    size_t len = strlen(arg);

    if (len > 0 && arg[0] == '"') {
        ++arg;
        --len;
    }

    if (len > 0 && arg[len - 1] == '"') {
        --len;
    }

    if (len > out_size - 1) {
        len = out_size - 1;
    }

    memcpy(out, arg, len);
    out[len] = '\0';
}

/* Converts all newline/tab chars to spaces (use for strings that should be contained to a single line) */
void filter_str(char *str, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        char ch = str[i];

        if (!is_valid_char(ch) || str[i] == '\0') {
            str[i] = ' ';
        }
    }
}


/*
 * Helper function for `valid_nick()`.
 *
 * Returns true if `ch` is not in the `invalid_chars` array.
 */
static bool is_valid_char(char ch)
{
    char tmp;

    for (size_t i = 0; (tmp = invalid_chars[i]); ++i) {
        if (tmp == ch) {
            return false;
        }
    }

    return true;
}

/* Returns true if nick is valid.
 *
 * A valid toxic nick:
 * - cannot be empty
 * - cannot start with a space
 * - must not contain a forward slash (for logfile naming purposes)
 * - must not contain contiguous spaces
 * - must not contain a newline or tab seqeunce
 */
bool valid_nick(const char *nick)
{
    if (!nick[0] || nick[0] == ' ') {
        return false;
    }

    for (size_t i = 0; nick[i]; ++i) {
        char ch = nick[i];

        if ((ch == ' ' && nick[i + 1] == ' ') || !is_valid_char(ch)) {
            return false;
        }
    }

    return true;
}
