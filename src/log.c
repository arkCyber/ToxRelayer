/*  
    log.c
 *
 */

#include <stdio.h>
#include <stdarg.h>

#include "misc.h"
#include "log.h"

#define TIMESTAMP_SIZE          64
#define MAX_MESSAGE_SIZE        512

//
static struct tm *get_wall_time(void)
{
    struct tm *timeinfo;
    time_t t = get_time();
    timeinfo = localtime((const time_t *) &t);

    return timeinfo;
}
//
void log_timestamp(const char *message, ...)
{
    char format[MAX_MESSAGE_SIZE];

    va_list args;
    va_start(args, message);
    const int formatted = vsnprintf(format, sizeof(format), message, args);
    va_end(args);

    if (formatted < 0) {
        return;                     /* formatting failed: nothing useful to log */
    }

    char ts[TIMESTAMP_SIZE];
    const size_t stamped = strftime(ts, TIMESTAMP_SIZE, "[%H:%M:%S]", get_wall_time());

    if (stamped == 0) {
        ts[0] = '\0';
    }

    console_out("%s %s\n", ts, format);
}
//
void log_error_timestamp(int err, const char *message, ...)
{
    char format[MAX_MESSAGE_SIZE];

    va_list args;
    va_start(args, message);
    const int formatted = vsnprintf(format, sizeof(format), message, args);
    va_end(args);

    if (formatted < 0) {
        return;
    }

    char ts[TIMESTAMP_SIZE];
    const size_t stamped = strftime(ts, TIMESTAMP_SIZE, "[%H:%M:%S]", get_wall_time());

    if (stamped == 0) {
        ts[0] = '\0';
    }

    console_err("%s %s (error %d)\n", ts, format, err);
}
//---------------------------------------------------------------------------
//  Console output. See log.h for the contract: the result of the underlying
//  write is observed here once instead of being ignored at every call site.
//---------------------------------------------------------------------------
void console_out(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vprintf(format, args);
    va_end(args);

    (void) written;                 /* a console write failure is not actionable */
}

void console_err(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vfprintf(stderr, format, args);
    va_end(args);

    (void) written;
}
