/*  test_misc.c
 *
 *  Verification suite for the utility module (misc.c).
 *
 *  Every function in misc.c is exercised here except
 *  get_conference_nick_truncate(), which requires a live toxcore conference and
 *  is covered by the integration tier instead.
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_misc \
 *         tests/test_misc.c src/misc.c src/log.c $(pkg-config --cflags --libs toxcore)
 *      ./test_misc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/misc.h"

static int tests_run = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        tests_run++;                                                            \
        if (!(expr)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);      \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

/* ------------------------------------------------------------------------- */
/* timed_out                                                                 */
/* ------------------------------------------------------------------------- */

static void test_timed_out(void)
{
    /* Not enough time has passed. */
    CHECK(!timed_out(1000, 1005, 10));
    CHECK(!timed_out(1000, 1000, 10));

    /* Exactly at the boundary counts as timed out. */
    CHECK(timed_out(1000, 1010, 10));
    CHECK(timed_out(1000, 1011, 10));

    /* A zero timeout expires as soon as time has moved on. */
    CHECK(timed_out(1000, 1001, 0));

    /* The clock moving backwards must not report a timeout. The previous
     * implementation compared timestamp + timeout <= curtime with mixed
     * signedness, which could overflow instead. */
    CHECK(!timed_out(2000, 1000, 10));

    /* A very large timeout is simply not expired: no overflow. */
    CHECK(!timed_out(0, 1000, 0xFFFFFFFFFFFFFFFFULL));

    /* A very old timestamp has long expired. */
    CHECK(timed_out(0, 1000000, 60));
}

/* ------------------------------------------------------------------------- */
/* parse_int_range                                                           */
/* ------------------------------------------------------------------------- */

static void test_parse_int_range(void)
{
    long out = 12345;

    /* Ordinary values, including both signs and both range boundaries. */
    CHECK(parse_int_range("0", 0, 100, &out) && out == 0);
    CHECK(parse_int_range("100", 0, 100, &out) && out == 100);
    CHECK(parse_int_range("-7", -10, -1, &out) && out == -7);
    CHECK(parse_int_range("+42", 0, 100, &out) && out == 42);
    CHECK(parse_int_range("999", 1, 999, &out) && out == 999);

    /* The sentinel the callers rely on: a rejected input must not touch *out.
     * atoi() would have returned 0 here and silently replaced a valid value. */
    out = 777;
    CHECK(!parse_int_range("not-a-number", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("12abc", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("abc12", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("1 2", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("0x10", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("+", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("-", 0, 1000, &out) && out == 777);

    /* Whitespace is not a valid spelling of a number. */
    CHECK(!parse_int_range(" 12", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("12 ", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("\t12", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("\n12", 0, 1000, &out) && out == 777);

    /* Out of range on either side, including the overflow that atoi() turns
     * into an unspecified value. */
    CHECK(!parse_int_range("101", 0, 100, &out) && out == 777);
    CHECK(!parse_int_range("-1", 0, 100, &out) && out == 777);
    CHECK(!parse_int_range("999999999999999999999999", 0, 1000, &out) && out == 777);
    CHECK(!parse_int_range("-999999999999999999999999", -1000, 0, &out) && out == 777);

    /* Degenerate arguments are refused rather than dereferenced. */
    CHECK(!parse_int_range(NULL, 0, 10, &out));
    CHECK(!parse_int_range("5", 0, 10, NULL));
    CHECK(!parse_int_range("5", 10, 0, &out));      /* inverted range */
    CHECK(out == 777);
}

/* ------------------------------------------------------------------------- */
/* unquote_str                                                               */
/* ------------------------------------------------------------------------- */

static void test_unquote_str(void)
{
    char out[64];

    /* A well formed quoted argument loses exactly the two quotes. */
    unquote_str("\"hello world\"", out, sizeof(out));
    CHECK(strcmp(out, "hello world") == 0);

    /* An empty quoted argument becomes an empty string. */
    unquote_str("\"\"", out, sizeof(out));
    CHECK(strcmp(out, "") == 0);

    /* Only one quote present: nothing is dropped from the middle and the copy
     * stays inside the buffer. The command handlers used to compute strlen-1
     * here, which wrote one byte before the buffer. */
    unquote_str("\"", out, sizeof(out));
    CHECK(strcmp(out, "") == 0);

    /* An unquoted argument is copied unchanged. */
    unquote_str("plain", out, sizeof(out));
    CHECK(strcmp(out, "plain") == 0);

    /* A lone opening / closing quote is dropped, not treated as content. */
    unquote_str("\"open", out, sizeof(out));
    CHECK(strcmp(out, "open") == 0);

    unquote_str("close\"", out, sizeof(out));
    CHECK(strcmp(out, "close") == 0);

    /* A quoted empty body inside a larger string. */
    unquote_str("\"a\"", out, sizeof(out));
    CHECK(strcmp(out, "a") == 0);

    /* Truncation is bounded and always NUL terminated. */
    char small[8];
    unquote_str("\"abcdefghijklmnop\"", small, sizeof(small));
    CHECK(strlen(small) == sizeof(small) - 1);
    CHECK(strcmp(small, "abcdefg") == 0);

    /* Degenerate arguments are tolerated rather than dereferenced. */
    out[0] = 'X';
    unquote_str(NULL, out, sizeof(out));
    CHECK(out[0] == '\0');

    unquote_str("\"abc\"", NULL, sizeof(out));

    char zero[4] = { 'X', 'X', 'X', 'X' };
    unquote_str("\"abc\"", zero, 0);
    CHECK(zero[0] == 'X');
}

/* ------------------------------------------------------------------------- */
/* Clock accessors                                                           */
/* ------------------------------------------------------------------------- */

static void test_clock_accessors(void)
{
    const time_t a = get_time();
    CHECK(a > 1600000000);          /* after 2020-09-13 */

    const time_t b = get_unix_time();
    CHECK(b >= a);
}

static void test_get_time_str(void)
{
    char buf[64];

    /* Normal use produces HH:MM. */
    memset(buf, 'X', sizeof(buf));
    get_time_str(buf, sizeof(buf));
    CHECK(strlen(buf) == 5);
    CHECK(buf[2] == ':');

    /* A NULL buffer is ignored rather than dereferenced. */
    get_time_str(NULL, sizeof(buf));

    /* A zero size writes nothing. */
    char zero[4] = { 'X', 'X', 'X', 'X' };
    get_time_str(zero, 0);
    CHECK(zero[0] == 'X');

    /* A tiny buffer is not overrun, and it is left empty rather than holding an
     * unspecified partial timestamp. */
    char tiny[2] = { 'X', 'X' };
    get_time_str(tiny, sizeof(tiny));
    CHECK(tiny[0] == '\0');
}

/* ------------------------------------------------------------------------- */
/* hex_string_to_bin                                                         */
/* ------------------------------------------------------------------------- */

static void test_hex_string_to_bin(void)
{
    char *bin = hex_string_to_bin("00FF10");
    CHECK(bin != NULL);
    CHECK((unsigned char) bin[0] == 0x00);
    CHECK((unsigned char) bin[1] == 0xFF);
    CHECK((unsigned char) bin[2] == 0x10);
    free(bin);

    /* Lower case is accepted too. */
    bin = hex_string_to_bin("deadBEEF");
    CHECK(bin != NULL);
    CHECK((unsigned char) bin[0] == 0xde);
    CHECK((unsigned char) bin[1] == 0xad);
    CHECK((unsigned char) bin[2] == 0xbe);
    CHECK((unsigned char) bin[3] == 0xef);
    free(bin);

    /* A 64 character public key yields exactly 32 bytes. */
    bin = hex_string_to_bin("0123456789ABCDEF0123456789ABCDEF"
                            "0123456789ABCDEF0123456789ABCDEF");
    CHECK(bin != NULL);
    CHECK((unsigned char) bin[0] == 0x01);
    CHECK((unsigned char) bin[31] == 0xEF);
    free(bin);

    /* Malformed input is rejected rather than parsed past the end of the
     * string, which is what the previous implementation did. */
    CHECK(hex_string_to_bin(NULL) == NULL);
    CHECK(hex_string_to_bin("") == NULL);
    CHECK(hex_string_to_bin("ABC") == NULL);        /* odd length */
    CHECK(hex_string_to_bin("GG") == NULL);         /* not hex    */
    CHECK(hex_string_to_bin("0\n") == NULL);        /* newline    */
    CHECK(hex_string_to_bin("A1B2Z4") == NULL);     /* bad digit  */
}

/* ------------------------------------------------------------------------- */
/* File helpers                                                              */
/* ------------------------------------------------------------------------- */

static void test_file_helpers(void)
{
    CHECK(file_exists("/definitely/not/here") == false);

    const char *path = "misc_test_file.tmp";
    FILE *fp = fopen(path, "wb");

    CHECK(fp != NULL);
    CHECK(fwrite("0123456789", 1, 10, fp) == 10);
    CHECK(fclose(fp) == 0);

    CHECK(file_exists(path) == true);
    CHECK(file_size(path) == 10);

    CHECK(unlink(path) == 0);
    CHECK(file_exists(path) == false);
    CHECK(file_size(path) == 0);        /* a missing file reports size 0 */
}

/* ------------------------------------------------------------------------- */
/* Buffer helpers                                                            */
/* ------------------------------------------------------------------------- */

static void test_copy_tox_str(void)
{
    char dst[16];

    CHECK(copy_tox_str(dst, sizeof(dst), "hello", 5) == 5);
    CHECK(strcmp(dst, "hello") == 0);

    /* The copy is truncated to size-1 and always terminated. */
    memset(dst, 'X', sizeof(dst));
    CHECK(copy_tox_str(dst, 4, "hello", 5) == 3);
    CHECK(strcmp(dst, "hel") == 0);

    /* A zero length copy produces an empty string. */
    CHECK(copy_tox_str(dst, sizeof(dst), "hello", 0) == 0);
    CHECK(dst[0] == '\0');

    /* Only the declared number of bytes is consumed from the source. */
    CHECK(copy_tox_str(dst, sizeof(dst), "abcdef", 3) == 3);
    CHECK(strcmp(dst, "abc") == 0);
}

static void test_char_find(void)
{
    CHECK(char_find(0, "hello", 'h') == 0);
    CHECK(char_find(0, "hello", 'o') == 4);
    CHECK(char_find(1, "hello", 'l') == 2);
    CHECK(char_find(3, "hello", 'l') == 3);

    /* Not found: the index of the terminator is returned. */
    CHECK(char_find(0, "hello", 'z') == 5);
    CHECK(char_find(5, "hello", 'h') == 5);

    /* An index beyond the string is not searched. The previous implementation
     * evaluated s[idx] unconditionally and read past the end of the buffer
     * (caught by AddressSanitizer while writing this test). */
    CHECK(char_find(10, "hello", 'h') == 5);
    CHECK(char_find(1000, "x", 'x') == 1);

    /* Degenerate arguments are rejected without reading memory. */
    CHECK(char_find(0, NULL, 'a') == 0);
    CHECK(char_find(-1, "hello", 'h') == 0);
    CHECK(char_find(0, "", 'a') == 0);
}

static void test_filter_str(void)
{
    char s[] = "a\nb\tc\rd/e";

    filter_str(s, strlen(s));

    /* Newlines, tabs, carriage returns and NUL bytes become spaces. */
    CHECK(strcmp(s, "a b c d e") == 0);

    /* Only the first `len` characters are inspected. */
    char t[] = "ab\ncd";
    filter_str(t, 2);
    CHECK(t[2] == '\n');
    CHECK(t[3] == 'c');
}

/* ------------------------------------------------------------------------- */
/* Nick validation                                                           */
/* ------------------------------------------------------------------------- */

static void test_valid_nick(void)
{
    CHECK(valid_nick("alice") == true);
    CHECK(valid_nick("a") == true);
    CHECK(valid_nick("a b") == true);

    CHECK(valid_nick("") == false);           /* empty             */
    CHECK(valid_nick(" leading") == false);   /* leading space     */
    CHECK(valid_nick("two  spaces") == false);/* contiguous spaces */
    CHECK(valid_nick("with/slash") == false);
    CHECK(valid_nick("with\ttab") == false);
    CHECK(valid_nick("with\nnewline") == false);
}

/* ------------------------------------------------------------------------- */
/* Elapsed time formatting                                                   */
/* ------------------------------------------------------------------------- */

static void test_get_elapsed_time_str(void)
{
    char buf[64];

    get_elapsed_time_str(buf, sizeof(buf), 0);
    CHECK(strcmp(buf, "0d 0h 0m") == 0);

    get_elapsed_time_str(buf, sizeof(buf), 60);
    CHECK(strcmp(buf, "0d 0h 1m") == 0);

    get_elapsed_time_str(buf, sizeof(buf), 3661);
    CHECK(strcmp(buf, "0d 1h 1m") == 0);

    /* One day, one hour and one minute. */
    get_elapsed_time_str(buf, sizeof(buf), (24 * 3600) + 3600 + 60);
    CHECK(strcmp(buf, "1d 1h 1m") == 0);
}

/* ------------------------------------------------------------------------- */
/* Key list lookup                                                           */
/* ------------------------------------------------------------------------- */

static void to_bin(const char *hex, unsigned char *out)
{
    char *tmp = hex_string_to_bin(hex);
    CHECK(tmp != NULL);
    memcpy(out, tmp, TOX_PUBLIC_KEY_SIZE);
    free(tmp);
}

static void test_file_contains_key(void)
{
    const char *path = "misc_test_keys.tmp";
    (void) unlink(path);

    const char *key_a = "0102030405060708090A0B0C0D0E0F10"
                        "1112131415161718191A1B1C1D1E1F20";
    const char *key_b = "FF02030405060708090A0B0C0D0E0F10"
                        "1112131415161718191A1B1C1D1E1F20";
    const char *key_c = "AA02030405060708090A0B0C0D0E0F10"
                        "1112131415161718191A1B1C1D1E1F20";

    unsigned char bin_a[TOX_PUBLIC_KEY_SIZE];
    unsigned char bin_b[TOX_PUBLIC_KEY_SIZE];
    unsigned char bin_c[TOX_PUBLIC_KEY_SIZE];

    to_bin(key_a, bin_a);
    to_bin(key_b, bin_b);
    to_bin(key_c, bin_c);

    /* A missing file is created and reported as "no match". */
    CHECK(file_contains_key((const char *) bin_a, path) == 0);

    /* Add key A. The line is written with a terminator, which must be ignored
     * when the key is parsed. */
    FILE *fp = fopen(path, "a");
    CHECK(fp != NULL);
    CHECK(fprintf(fp, "%s\n", key_a) > 0);
    CHECK(fclose(fp) == 0);

    CHECK(file_contains_key((const char *) bin_a, path) == 1);
    CHECK(file_contains_key((const char *) bin_b, path) == 0);
    CHECK(file_contains_key((const char *) bin_c, path) == 0);

    /* A second key appended without a terminator is still found. */
    fp = fopen(path, "a");
    CHECK(fp != NULL);
    CHECK(fprintf(fp, "%s", key_b) > 0);
    CHECK(fclose(fp) == 0);

    CHECK(file_contains_key((const char *) bin_a, path) == 1);
    CHECK(file_contains_key((const char *) bin_b, path) == 1);

    /* Malformed lines must neither match nor derail the scan. */
    fp = fopen(path, "a");
    CHECK(fp != NULL);
    CHECK(fprintf(fp, "\nshort\nZZZZ\n") > 0);
    CHECK(fclose(fp) == 0);

    CHECK(file_contains_key((const char *) bin_a, path) == 1);
    CHECK(file_contains_key((const char *) bin_b, path) == 1);

    /* Argument validation. */
    CHECK(file_contains_key(NULL, path) == -1);
    CHECK(file_contains_key((const char *) bin_a, NULL) == -1);

    /* An unreadable path is reported as an error, not as "no match". */
    CHECK(file_contains_key((const char *) bin_a, "/definitely/not/here/keys") == -1);

    CHECK(unlink(path) == 0);
}

int main(void)
{
    char template[] = "/tmp/toxrelayer_misc_XXXXXX";
    char *dir = mkdtemp(template);

    if (dir == NULL) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }

    if (chdir(dir) != 0) {
        perror("chdir");
        return EXIT_FAILURE;
    }

    test_timed_out();
    test_parse_int_range();
    test_unquote_str();
    test_clock_accessors();
    test_get_time_str();
    test_hex_string_to_bin();
    test_file_helpers();
    test_copy_tox_str();
    test_char_find();
    test_filter_str();
    test_valid_nick();
    test_get_elapsed_time_str();
    test_file_contains_key();

    printf("test_misc: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}

