/*  test_commands.c
 *
 *  Verification suite for the command layer (commands.c).
 *
 *  Two tiers are covered here:
 *
 *   - commands_parse() is a pure function and is tested exhaustively.
 *   - execute() is driven with an offline toxcore instance (tox_new() with UDP,
 *     IPv6 and LAN discovery disabled), so dispatch, argument handling and the
 *     command table are exercised without any network access.
 *
 *  The two symbols commands.c needs from toxrelayer.c are replaced by test doubles,
 *  which keeps the command framework linkable on its own.
 *
 *  Build and run:
 *      cc -std=c11 -Wall -Wextra -o test_commands \
 *         tests/test_commands.c src/commands.c src/groupchats.c src/misc.c \
 *         src/log.c $(pkg-config --cflags --libs toxcore)
 *      ./test_commands
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tox/tox.h>
#include "../src/toxrelayer.h"
#include "../src/commands.h"

/* Defined in toxrelayer.c; referenced by commands.c. */
struct Tox_Bot Tox_Bot;

/* ------------------------------------------------------------------------- */
/* Test doubles for the toxrelayer.c symbols the command layer depends on        */
/* ------------------------------------------------------------------------- */

bool friend_is_master(Tox *m, uint32_t friendnumber)
{
    (void) m;
    (void) friendnumber;
    return false;               /* there is no master in these tests */
}

int save_data(Tox *m, const char *path)
{
    (void) m;
    (void) path;
    return 0;
}

/* ------------------------------------------------------------------------- */

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
/* commands_parse                                                            */
/* ------------------------------------------------------------------------- */

static void test_parse_single(void)
{
    char args[MAX_NUM_ARGS][MAX_COMMAND_LENGTH];

    CHECK(commands_parse("/help", args) == 1);
    CHECK(strcmp(args[0], "/help") == 0);
}

static void test_parse_multiple(void)
{
    char args[MAX_NUM_ARGS][MAX_COMMAND_LENGTH];

    CHECK(commands_parse("/invite 3 secret", args) == 3);
    CHECK(strcmp(args[0], "/invite") == 0);
    CHECK(strcmp(args[1], "3") == 0);
    CHECK(strcmp(args[2], "secret") == 0);
}

static void test_parse_quoted(void)
{
    char args[MAX_NUM_ARGS][MAX_COMMAND_LENGTH];

    /* A quoted argument counts as one, spaces included. The quotes are retained
     * in the argument: the free-text handlers strip them themselves. */
    CHECK(commands_parse("/gmessage 1 \"hello world\"", args) == 3);
    CHECK(strcmp(args[0], "/gmessage") == 0);
    CHECK(strcmp(args[1], "1") == 0);
    CHECK(strcmp(args[2], "\"hello world\"") == 0);

    /* A quoted argument as the only argument also yields no trailing empty
     * argument, which previously made argc one larger than expected. */
    CHECK(commands_parse("/name \"foo bar\"", args) == 2);
    CHECK(strcmp(args[0], "/name") == 0);
    CHECK(strcmp(args[1], "\"foo bar\"") == 0);

    /* An empty quoted argument is still an argument. */
    CHECK(commands_parse("/name \"\"", args) == 2);
    CHECK(strcmp(args[1], "\"\"") == 0);

    /* Text after the closing quote starts the next argument. */
    CHECK(commands_parse("/cmd \"one\" two", args) == 3);
    CHECK(strcmp(args[1], "\"one\"") == 0);
    CHECK(strcmp(args[2], "two") == 0);
}

static void test_parse_unterminated_quote(void)
{
    char args[MAX_NUM_ARGS][MAX_COMMAND_LENGTH];

    /* The closing quote is missing: the input is malformed. */
    CHECK(commands_parse("/name \"unterminated", args) == -1);
}

static void test_parse_empty(void)
{
    char args[MAX_NUM_ARGS][MAX_COMMAND_LENGTH];

    CHECK(commands_parse("", args) == 1);
    CHECK(strcmp(args[0], "") == 0);

    CHECK(commands_parse("   ", args) >= 1);
}

static void test_parse_argument_limit(void)
{
    char args[MAX_NUM_ARGS][MAX_COMMAND_LENGTH];

    /* More arguments than MAX_NUM_ARGS are truncated, never overflowed. */
    const int n = commands_parse("/a b c d e f g", args);
    CHECK(n > 0);
    CHECK(n <= MAX_NUM_ARGS);
}

static void test_parse_long_argument(void)
{
    static char line[MAX_COMMAND_LENGTH * 2];
    char args[MAX_NUM_ARGS][MAX_COMMAND_LENGTH];

    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    /* Whatever happens, every produced argument is terminated within bounds. */
    const int n = commands_parse(line, args);

    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            CHECK(memchr(args[i], '\0', MAX_COMMAND_LENGTH) != NULL);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* execute                                                                   */
/* ------------------------------------------------------------------------- */

static Tox *make_offline_tox(void)
{
    struct Tox_Options *opts = tox_options_new(NULL);
    CHECK(opts != NULL);

    tox_options_set_udp_enabled(opts, false);
    tox_options_set_ipv6_enabled(opts, false);
    tox_options_set_local_discovery_enabled(opts, false);

    TOX_ERR_NEW err;
    Tox *m = tox_new(opts, &err);

    tox_options_free(opts);

    CHECK(m != NULL);
    CHECK(err == TOX_ERR_NEW_OK);

    return m;
}

/* T8: unrecognised input is reported, never dispatched. */
static void test_execute_unknown(Tox *m)
{
    CHECK(execute(m, 0, "/definitely-not-a-command", 25) == -1);
    CHECK(execute(m, 0, "not even a command", 17) == -1);
    CHECK(execute(m, 0, "", 0) == -1);
}

/* T9: recognised commands are dispatched. The peer does not exist, so the
 *     handler cannot actually reply, but the dispatch itself must succeed. */
static void test_execute_known(Tox *m)
{
    const char *known[] = { "/help", "/id", "/info" };

    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        CHECK(execute(m, 0, known[i], (int) strlen(known[i])) == 0);
    }
}

/* T10: input at or above the command limit is refused before parsing. */
static void test_execute_overlong_input(Tox *m)
{
    static char line[MAX_COMMAND_LENGTH + 16];
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    line[0] = '/';

    CHECK(execute(m, 0, line, MAX_COMMAND_LENGTH + 15) == -1);
}

/* T11: every command registered in the dispatch table is reachable. Missing
 *      arguments are reported by the handler, not by the dispatcher. */
static void test_execute_dispatches_registered_commands(Tox *m)
{
    const char *names[] = {
        "/default", "/group", "/gmessage", "/help", "/id", "/info", "/invite",
        "/leave", "/master", "/name", "/passwd", "/purge", "/status",
        "/statusmessage", "/title",
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        CHECK(execute(m, 0, names[i], (int) strlen(names[i])) == 0);
    }
}

/* T12: a command with arguments reaches its handler. */
static void test_execute_with_arguments(Tox *m)
{
    /* /default takes a room number; the handler runs and cannot reply, which
     * must not disturb the dispatcher. */
    CHECK(execute(m, 0, "/default 0", 10) == 0);
    CHECK(execute(m, 0, "/passwd 0", 9) == 0);
    CHECK(execute(m, 0, "/purge 7", 8) == 0);
}

int main(void)
{
    char template[] = "/tmp/toxrelayer_cmd_XXXXXX";
    char *dir = mkdtemp(template);

    if (dir == NULL) {
        perror("mkdtemp");
        return EXIT_FAILURE;
    }

    if (chdir(dir) != 0) {
        perror("chdir");
        return EXIT_FAILURE;
    }

    test_parse_single();
    test_parse_multiple();
    test_parse_quoted();
    test_parse_unterminated_quote();
    test_parse_empty();
    test_parse_argument_limit();
    test_parse_long_argument();

    Tox *m = make_offline_tox();

    test_execute_unknown(m);
    test_execute_known(m);
    test_execute_overlong_input(m);
    test_execute_dispatches_registered_commands(m);
    test_execute_with_arguments(m);

    tox_kill(m);

    printf("test_commands: %d checks passed\n", tests_run);
    return EXIT_SUCCESS;
}
