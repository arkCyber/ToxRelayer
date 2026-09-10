/*  log.h
 *
 */

#ifndef LOG_H
#define LOG_H

/* ---------------------------------------------------------------------------
 * Format-string checking
 *
 * Every logging entry point takes a printf style format string, but until they
 * carried this attribute the compiler could not see through the variadic call,
 * so a mismatched format or argument list was only discovered at run time (and
 * on some platforms is undefined behaviour). The attribute makes the compiler
 * check each call site against the literal format, exactly as it does for
 * printf() itself.
 *
 * The macro expands to nothing on compilers that do not support the attribute,
 * so the declarations stay portable.
 * ------------------------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#define LOG_FORMAT(fmt_index, first_arg_index) \
    __attribute__((format(printf, fmt_index, first_arg_index)))
#else
#define LOG_FORMAT(fmt_index, first_arg_index)
#endif

#define UNKNOWN_NAME        "Anonymous       "
#define DEFAULT_TOX_NAME    "alice@metaMsg   "      /* should always be the same as toxcore's default name */

#define MAX_STR_SIZE                TOX_MAX_MESSAGE_LENGTH         
                                                    /* must be >= TOX_MAX_MESSAGE_LENGTH */
#define MAX_CMDNAME_SIZE            64
#define TOXIC_MAX_NAME_LENGTH       256             /* Must be <= TOX_MAX_NAME_LENGTH */


/* Print `message` to stdout prefixed with a timestamp */
void log_timestamp(const char *message, ...) LOG_FORMAT(1, 2);

/* Print `message` with `err` to stderr prefixed with a timestamp */
void log_error_timestamp(int err, const char *message, ...) LOG_FORMAT(2, 3);

/* ---------------------------------------------------------------------------
 * Console output
 *
 * All plain console output goes through these two helpers so that the result of
 * the underlying write is observed exactly once instead of being silently
 * ignored at every call site (CERT ERR33-C). A failure to write to the console
 * cannot be acted upon and never changes control flow.
 * ------------------------------------------------------------------------- */
void console_out(const char *format, ...) LOG_FORMAT(1, 2);
void console_err(const char *format, ...) LOG_FORMAT(1, 2);

#endif // LOG_H

