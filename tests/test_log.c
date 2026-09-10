/*  test_log.c
 *
 *  Verification suite for the logging and console-output module (log.c).
 *
 *  Output is captured by temporarily redirecting the standard stream to a
 *  temporary file, so the tests assert on what a user would actually see rather
 *  than on internal state.
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_log \
 *         tests/test_log.c src/log.c src/misc.c $(pkg-config --cflags --libs toxcore)
 *      ./test_log
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/log.h"

static int tests_run = 0;

#define CHECK(expr)                                                             \
    do {                                                                        \
        tests_run++;                                                            \
        if (!(expr)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);      \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while (0)

#define CAPACITY 4096

/* Begin redirecting `fd` (STDOUT_FILENO or STDERR_FILENO) into a temporary
 * file. `saved` receives the original descriptor. */
static FILE *capture_begin(int fd, int *saved)
{
    fflush(NULL);

    *saved = dup(fd);
    CHECK(*saved >= 0);

    FILE *tmp = tmpfile();
    CHECK(tmp != NULL);
    CHECK(dup2(fileno(tmp), fd) >= 0);

    return tmp;
}

/* Restore `fd` and copy what was written into `buf`. */
static void capture_end(int fd, int saved, FILE *tmp, char *buf, size_t size)
{
    fflush(NULL);
    CHECK(dup2(saved, fd) >= 0);
    CHECK(close(saved) == 0);

    rewind(tmp);
    const size_t got = fread(buf, 1, size - 1, tmp);
    buf[got] = '\0';

    CHECK(fclose(tmp) == 0);
}

/* ------------------------------------------------------------------------- */
/* console_out / console_err                                                 */
/* ------------------------------------------------------------------------- */

/* T1: console_out writes exactly the formatted text to stdout. */
static void test_console_out(void)
{
    char buf[CAPACITY];
    int saved;
    FILE *tmp = capture_begin(STDOUT_FILENO, &saved);

    console_out("plain %s %d\n", "text", 42);

    capture_end(STDOUT_FILENO, saved, tmp, buf, sizeof(buf));

    CHECK(strcmp(buf, "plain text 42\n") == 0);
}

/* T2: console_err writes to stderr and nothing to stdout. */
static void test_console_err(void)
{
    char err[CAPACITY];
    char out[CAPACITY];
    int saved_out;
    int saved_err;

    FILE *tmp_out = capture_begin(STDOUT_FILENO, &saved_out);
    FILE *tmp_err = capture_begin(STDERR_FILENO, &saved_err);

    console_err("failure %d\n", 7);

    capture_end(STDERR_FILENO, saved_err, tmp_err, err, sizeof(err));
    capture_end(STDOUT_FILENO, saved_out, tmp_out, out, sizeof(out));

    CHECK(strcmp(err, "failure 7\n") == 0);
    CHECK(out[0] == '\0');
}

/* T3: an empty format produces no output and does not crash. */
static void test_console_empty(void)
{
    char buf[CAPACITY];
    int saved;
    FILE *tmp = capture_begin(STDOUT_FILENO, &saved);

    console_out("%s", "");

    capture_end(STDOUT_FILENO, saved, tmp, buf, sizeof(buf));

    CHECK(buf[0] == '\0');
}

/* ------------------------------------------------------------------------- */
/* log_timestamp / log_error_timestamp                                       */
/* ------------------------------------------------------------------------- */

/* T4: log_timestamp prefixes the message with an [HH:MM:SS] stamp. */
static void test_log_timestamp(void)
{
    char buf[CAPACITY];
    int saved;
    FILE *tmp = capture_begin(STDOUT_FILENO, &saved);

    log_timestamp("hello %s", "world");

    capture_end(STDOUT_FILENO, saved, tmp, buf, sizeof(buf));

    /* Expected shape: "[HH:MM:SS] hello world\n" */
    CHECK(buf[0] == '[');
    CHECK(buf[3] == ':');
    CHECK(buf[6] == ':');
    CHECK(buf[9] == ']');
    CHECK(buf[10] == ' ');
    CHECK(strstr(buf, "hello world\n") != NULL);
    CHECK(strlen(buf) == 23);       /* 10 stamp + 1 space + 11 text + newline */
}

/* T5: log_error_timestamp writes to stderr and appends the error code. */
static void test_log_error_timestamp(void)
{
    char buf[CAPACITY];
    int saved;
    FILE *tmp = capture_begin(STDERR_FILENO, &saved);

    log_error_timestamp(-1, "could not %s", "open");

    capture_end(STDERR_FILENO, saved, tmp, buf, sizeof(buf));

    CHECK(buf[0] == '[');
    CHECK(buf[9] == ']');
    CHECK(strstr(buf, "could not open (error -1)\n") != NULL);
}

/* T6: a format string longer than the internal buffer is truncated, not
 *     overflowed, and the timestamp is still written. */
static void test_log_truncation(void)
{
    char big[1200];
    char buf[CAPACITY];
    int saved;

    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    FILE *tmp = capture_begin(STDOUT_FILENO, &saved);
    log_timestamp("%s", big);
    capture_end(STDOUT_FILENO, saved, tmp, buf, sizeof(buf));

    /* The stamp is intact and the whole line fits the capture buffer. */
    CHECK(buf[0] == '[');
    CHECK(buf[9] == ']');
    CHECK(strlen(buf) < CAPACITY);
    CHECK(strchr(buf, '\n') != NULL);
}

/* T7: a message containing format-like text is not re-interpreted. */
static void test_log_no_double_formatting(void)
{
    char buf[CAPACITY];
    int saved;
    FILE *tmp = capture_begin(STDOUT_FILENO, &saved);

    log_timestamp("%s", "%d %s literal");

    capture_end(STDOUT_FILENO, saved, tmp, buf, sizeof(buf));

    CHECK(strstr(buf, "%d %s literal\n") != NULL);
}

int main(void)
{
    test_console_out();
    test_console_err();
    test_console_empty();
    test_log_timestamp();
    test_log_error_timestamp();
    test_log_truncation();
    test_log_no_double_formatting();

    printf("test_log: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}
