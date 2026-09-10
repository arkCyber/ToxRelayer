/*  commands.h
 *
 *  Text-command interface: parsing of an incoming command line and dispatch to
 *  the command implementations.
 */

#ifndef COMMANDS_H
#define COMMANDS_H

#include <tox/tox.h>

/* Maximum length of a command line, and of one parsed argument. */
#define MAX_COMMAND_LENGTH          TOX_MAX_MESSAGE_LENGTH

/* Maximum number of whitespace separated arguments including the command. */
#define MAX_NUM_ARGS                4

/* Execute a command line received from `friendnumber`.
 *
 * @return 0 when the command was recognised and dispatched, -1 when it was not
 *         recognised or the input was malformed.
 */
int execute(Tox *m, uint32_t friendnumber, const char *input, int length);

/* Split `input` into arguments.
 *
 * Arguments wrapped in double quotes count as one, and the quotes are retained
 * in the produced argument: the handlers that accept free text strip them (see
 * cmd_name and cmd_statusmessage). At most MAX_NUM_ARGS arguments are produced.
 *
 * Preconditions : input != NULL, args != NULL.
 * Postconditions: on a positive return, args[0] holds the command name (with
 *                 its leading '/'), every produced argument is NUL terminated
 *                 and no spurious empty argument is produced for input that ends
 *                 with a quoted argument.
 *
 * @return the number of arguments, or -1 when the input is malformed (an
 *         unterminated quoted argument).
 */
int commands_parse(const char *input, char (*args)[MAX_COMMAND_LENGTH]);

#endif    /* COMMANDS_H */

