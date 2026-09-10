/*  
    commands.c
 *
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

#include <tox/tox.h>
#include <tox/toxav.h>
#include <stdio.h>

#include "toxrelayer.h"
#include "misc.h"
#include "groupchats.h"
#include "commands.h"
#include "log.h"

extern struct Tox_Bot Tox_Bot;

void conference_basic_info_display( Tox *m );

//
//  command parser: You do not have permission to use this command.
//
static void authent_failed(Tox *m, uint32_t friendnum) 
{
    const char *outmsg = "You do not have permission to use this command.";

    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);

    return;
}
//
static void send_error(Tox *m, uint32_t friendnum, const char *message, int err) 
{
    char outmsg[TOX_MAX_MESSAGE_LENGTH];

    snprintf(outmsg, sizeof(outmsg), "%s (error %d)", message, err);
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);

    return;
}

/* Largest value accepted by /purge, in days. The bound exists so that
 * days * SECONDS_IN_DAY can never overflow uint64_t; 1000 years is far beyond
 * any operational lifetime and still rejects nonsense input. */
#define PURGE_DAYS_MAX 365000L

/* Parse the leading group-number argument of a command.
 *
 * A group number is a tox conference number: non-negative and representable as
 * an int. Returning false (rather than a silent 0, as atoi() did) is what lets
 * every caller report "invalid group number" instead of acting on group 0. */
static bool parse_group_number(const char *text, int *out)
{
    long value = 0;

    if (!parse_int_range(text, 0, (long) INT_MAX, &value)) {
        return false;
    }

    *out = (int) value;
    return true;
}

/* parse_group_number() above and unquote_str() (misc.h) are the only two helpers
 * the command handlers need; the parsing rules themselves live in misc.c, where
 * they are unit tested. */
//
// default <n>            : Sets default groupchat room to n
//
static void cmd_default(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    const char *outmsg = NULL;
    char msg[MAX_COMMAND_LENGTH];
    char name[TOX_MAX_NAME_LENGTH];

    // only the master have the command-action right !
    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 1) {
        outmsg = "Error: Room number required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);

        return;
    }

    int groupnum = 0;

    if (!parse_group_number(argv[1], &groupnum)) {
        outmsg = "Error: Invalid room number";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);

        return;
    }

    // Tox_Bot.default_groupnum ??
    Tox_Bot.default_groupnum = groupnum;
    
    snprintf(msg, sizeof(msg), "Default room number set to %d", groupnum);
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) msg, strlen(msg), NULL);

    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t len = tox_friend_get_name_size(m, friendnum, NULL);
    name[len] = '\0';

    log_timestamp("Default room number set to %d by %s", groupnum, name);

    return;
}
//
//  gmessage <n> <msg>     : Sends msg to groupchat n
//  gmessage 12 "Hello world!"
//
static void 
cmd_gmessage(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    const char *outmsg = NULL;

    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 1) {
        outmsg = "Error: Group number required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    if (argc < 2) {
        outmsg = "Error: Message required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    int groupnum = 0;

    if (!parse_group_number(argv[1], &groupnum)) {
        outmsg = "Error: Invalid group number";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    if (group_index(groupnum) == -1) {
        outmsg = "Error: Invalid group number";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    if (argv[2][0] != '\"') {
        outmsg = "Error: Message must be enclosed in quotes";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    /* remove opening and closing quotes */
    char msg[MAX_COMMAND_LENGTH];
    unquote_str(argv[2], msg, sizeof(msg));

    TOX_ERR_CONFERENCE_SEND_MESSAGE err;

    if (!tox_conference_send_message(m, groupnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) msg, strlen(msg), &err)) 
    {
        outmsg = "Error: Failed to send message.";
        send_error(m, friendnum, outmsg, err);
        return;
    }

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t nlen = tox_friend_get_name_size(m, friendnum, NULL);
    name[nlen] = '\0';

    outmsg = "Message sent.";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    log_timestamp("<%s> message to group %d: %s", name, groupnum, msg);

    return;
}
//
//  /group <type> <pass> :  Creates a new groupchat with type: 
//                                    text | audio (optional password)
//
static void 
cmd_group(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    const char *outmsg = NULL;

    if (argc < 1) {
        outmsg = "Please specify the group type: audio or text";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    uint8_t type = TOX_CONFERENCE_TYPE_AV ? !strcasecmp(argv[1], "audio") : TOX_CONFERENCE_TYPE_TEXT;

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t len = tox_friend_get_name_size(m, friendnum, NULL);
    name[len] = '\0';

    int groupnum = -1;

    if (type == TOX_CONFERENCE_TYPE_TEXT) 
    {
        TOX_ERR_CONFERENCE_NEW err;
        groupnum = tox_conference_new(m, &err);

        if (err != TOX_ERR_CONFERENCE_NEW_OK) {
            log_error_timestamp(err, "Group chat creation by %s failed to initialize", name);
            outmsg = "Group chat instance failed to initialize.";
            tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
            return;
        }
    } 
    else 
    if (type == TOX_CONFERENCE_TYPE_AV) 
    {
        groupnum = toxav_add_av_groupchat(m, NULL, NULL);

        if (groupnum == -1) {
            log_error_timestamp(-1, "Group chat creation by %s failed to initialize", name);
            outmsg = "Group chat instance failed to initialize.";
            tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
            return;
        }
    }

    const char *password = argc >= 2 ? argv[2] : NULL;

    if ( password && strlen(argv[2]) >= MAX_PASSWORD_SIZE) 
    {
        log_error_timestamp(-1, "Group chat creation by %s failed: Password too long", name);
        outmsg = "Group chat instance failed to initialize: Password too long";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    if ( group_add(groupnum, type, password) == -1) 
    {
        log_error_timestamp(-1, "Group chat creation by %s failed", name);
        outmsg = "Group chat creation failed";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        tox_conference_delete(m, groupnum, NULL);
        return;
    }

    const char *pw = password ? " (Password protected)" : "";
    log_timestamp("Group chat %d created by %s%s", groupnum, name, pw);

    char msg[MAX_COMMAND_LENGTH];
    snprintf(msg, sizeof(msg), "Group chat %d created%s", groupnum, pw);
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) msg, strlen(msg), NULL);

    return;
}
//
//  echo "help" command
//  "/info :  Dump current status and list active group chats";
//  "/invite :  Request invite to default group chat";
//  "/invite <n> <p> :  Request invite to group chat n (with password p if protected)";
//
//
static void cmd_help(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    /* Required by the callback signature; unused here. */
    (void) argc;
    (void) argv;
    const char *outmsg = NULL;

    outmsg = ">> Web3 IM-Relayer in Metaverse Cyber";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    outmsg = "/info          - Dump current status and list active group chats";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    outmsg = "/list         - List all the channels that";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    outmsg = "/join <ch>    - Join the msg channel";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    outmsg = "/part <ch>    - Leave the channel";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    outmsg = "/id           - Dump arkMeta ID";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    outmsg = "/default <n>  - Sets default groupchat room to n";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    outmsg = "/invite       - Request invite to default group chat";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    outmsg = "/invite <n> <p>       - Request invite to group chat n (with password p if protected)";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    outmsg = "/group <type> <pass>  - Creates a new groupchat with type: text | audio (optional password)";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    if (friend_is_master(m, friendnum)) {
        outmsg = "For a list of master commands see the commands.txt file";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    }

    return;
}
//
//  all the node have the right to get id information
//
static void cmd_id(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    /* Required by the callback signature; unused here. */
    (void) argc;
    (void) argv;
    char outmsg[TOX_ADDRESS_SIZE * 2 + 1] = "arkMeta ID :\n";
    char address[TOX_ADDRESS_SIZE];

    tox_self_get_address(m, (uint8_t *) address);

    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);

    for (size_t i = 0; i < TOX_ADDRESS_SIZE; ++i) {
        char d[3];
        snprintf(d, sizeof(d), "%02X", address[i] & 0xff);
        memcpy(outmsg + i * 2, d, 2);
    }

    outmsg[TOX_ADDRESS_SIZE * 2] = '\0';
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);

    return;
}
// 
//  "/info" command,echo information
//
static void cmd_info(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    /* Required by the callback signature; unused here. */
    (void) argc;
    (void) argv;
    char outmsg[MAX_COMMAND_LENGTH];
    char timestr[64];

    time_t curtime = get_time();
    get_elapsed_time_str(timestr, sizeof(timestr), curtime - Tox_Bot.start_time);
    snprintf(outmsg, sizeof(outmsg), "Uptime: %s", timestr);
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);

    uint32_t numfriends = tox_self_get_friend_list_size(m);
    snprintf(outmsg, sizeof(outmsg), "Friends: %d (%d online)", numfriends, Tox_Bot.num_online_friends);
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);

    snprintf(outmsg, sizeof(outmsg), "Inactive friends are purged after %"PRIu64" days",
             Tox_Bot.inactive_limit / SECONDS_IN_DAY);
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);

    /* List active group chats and number of peers in each */
    size_t num_chats = tox_conference_get_chatlist_size(m);

    if (num_chats == 0) {
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) "No active groupchats",
                                strlen("No active groupchats"), NULL);
        return;
    }

    /* Fixed storage: a zero-sized or input-sized variable length array is
     * undefined behaviour, so bound the count and use a constant buffer. */
    uint32_t groupchat_list[MAX_NUM_GROUPS];
    num_chats = MIN(num_chats, MAX_NUM_GROUPS);

    tox_conference_get_chatlist(m, groupchat_list);

    for (size_t i = 0; i < num_chats; ++i) {
        TOX_ERR_CONFERENCE_PEER_QUERY err;
        uint32_t groupnum = groupchat_list[i];
        uint32_t num_peers = tox_conference_peer_count(m, groupnum, &err);

        if (err == TOX_ERR_CONFERENCE_PEER_QUERY_OK) {
            int idx = group_index(groupnum);
            const char *title = "None";

            /* A conference toxcore knows about is not necessarily one this bot
             * has in its own table (for example after a partial load), and
             * g_chats[-1] is not a group. */
            if (idx != -1 && Tox_Bot.g_chats[idx].title_len) {
                title = Tox_Bot.g_chats[idx].title;
            }
            const char *type = tox_conference_get_type(m, groupnum, NULL) == TOX_CONFERENCE_TYPE_AV ? "Audio" : "Text";
            snprintf(outmsg, sizeof(outmsg), "Group %d | %s | peers: %d | Title: %s", groupnum, type,
                     num_peers, title);
            tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        }
    }
}
//
//  /invite :  Request invite to default group chat
//  /invite <n> <p> :  Request invite to group chat n (with password p if protected)
//
static void 
cmd_invite(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    const char *outmsg = NULL;
    int groupnum = Tox_Bot.default_groupnum;

    if (argc >= 1) {
        if (!parse_group_number(argv[1], &groupnum)) {
            outmsg = "Error: Invalid group number";
            tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
            return;
        }
    }

    int idx = group_index(groupnum);

    if (idx == -1) {
        outmsg = "Group doesn't exist.";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    int has_pass = Tox_Bot.g_chats[idx].has_pass;

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t len = tox_friend_get_name_size(m, friendnum, NULL);
    name[len] = '\0';

    const char *passwd = NULL;

    if (argc >= 2) {
        passwd = argv[2];
    }

    if (has_pass && (!passwd || strcmp(argv[2], Tox_Bot.g_chats[idx].password) != 0)) 
    {
        log_error_timestamp(-1, "Failed to invite %s to group %d (invalid password)", name, groupnum);
        outmsg = "Invalid password.";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    TOX_ERR_CONFERENCE_INVITE err;

    if (!tox_conference_invite(m, friendnum, groupnum, &err)) 
    {
        log_error_timestamp(err, "Failed to invite %s to group %d", name, groupnum);
        outmsg = "Invite failed";
        send_error(m, friendnum, outmsg, err);

        return;
    }

    log_timestamp("Invited %s to group %d", name, groupnum);

    return;
}
//
//  master~friendnum right
// 
static void cmd_leave(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    const char *outmsg = NULL;

    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 1) {
        outmsg = "Error: Group number required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    int groupnum = 0;

    if (!parse_group_number(argv[1], &groupnum)) {
        outmsg = "Error: Invalid group number";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    if (!tox_conference_delete(m, groupnum, NULL)) {
        outmsg = "Error: Invalid group number";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    char msg[MAX_COMMAND_LENGTH];

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t len = tox_friend_get_name_size(m, friendnum, NULL);
    name[len] = '\0';

    group_leave(groupnum);

    log_timestamp("Left group %d (%s)", groupnum, name);
    snprintf(msg, sizeof(msg), "Left group %d", groupnum);
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) msg, strlen(msg), NULL);
}
//
//  master~friendnum right
//
static void cmd_master(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    const char *outmsg = NULL;

    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 1) {
        outmsg = "Error: Tox ID required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    const char *id = argv[1];

    if (strlen(id) != TOX_ADDRESS_SIZE * 2) {
        outmsg = "Error: Invalid Tox ID";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    FILE *fp = fopen(MASTERLIST_FILE, "a");

    if (fp == NULL) {
        outmsg = "Error: could not find masterkeys file";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    if (fprintf(fp, "%s\n", id) < 0) {
        outmsg = "Error: could not write to masterkeys file";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL,
                                (const uint8_t *) outmsg, strlen(outmsg), NULL);
        (void) fclose(fp);
        return;
    }

    (void) fclose(fp);

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t len = tox_friend_get_name_size(m, friendnum, NULL);
    name[len] = '\0';

    log_timestamp("%s added master: %s", name, id);
    outmsg = "ID added to masterkeys list";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
}
//
//     master~friendnum right : tox_friend_get_name
//
static void cmd_name(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    const char *outmsg = NULL;

    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 1) {
        outmsg = "Error: Name required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    char name[TOX_MAX_NAME_LENGTH];

    if (argv[1][0] == '\"') {    /* remove opening and closing quotes */
        unquote_str(argv[1], name, sizeof(name));
    } else {
        snprintf(name, sizeof(name), "%s", argv[1]);
    }

    const int len = (int) strlen(name);
    tox_self_set_name(m, (uint8_t *) name, (uint16_t) len, NULL);

    char m_name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) m_name, NULL);
    size_t nlen = tox_friend_get_name_size(m, friendnum, NULL);
    m_name[nlen] = '\0';

    log_timestamp("%s set name to %s", m_name, name);
    save_data(m, DATA_FILE);
}
//
//  master~friendnum right
//
static void cmd_passwd(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) {
    const char *outmsg = NULL;

    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 1) {
        outmsg = "Error: group number required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    int groupnum = 0;

    if (!parse_group_number(argv[1], &groupnum)) {
        outmsg = "Error: Invalid group number";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    int idx = group_index(groupnum);

    if (idx == -1) {
        outmsg = "Error: Invalid group number";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t nlen = tox_friend_get_name_size(m, friendnum, NULL);
    name[nlen] = '\0';


    /* no password */
    if (argc < 2) {
        Tox_Bot.g_chats[idx].has_pass = false;
        memset(Tox_Bot.g_chats[idx].password, 0, MAX_PASSWORD_SIZE);

        outmsg = "No password set";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        log_timestamp("No password set for group %d by %s", groupnum, name);
        return;
    }

    if (strlen(argv[2]) >= MAX_PASSWORD_SIZE) {
        outmsg = "Password too long";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    Tox_Bot.g_chats[idx].has_pass = true;
    snprintf(Tox_Bot.g_chats[idx].password, sizeof(Tox_Bot.g_chats[idx].password), "%s", argv[2]);

    outmsg = "Password set";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    log_timestamp("Password for group %d set by %s", groupnum, name);

}
//
//  master~friendnum right
//
static void cmd_purge(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) 
{
    const char *outmsg = NULL;

    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 1) {
        outmsg = "Error: number > 0 required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    long parsed_days = 0;

    if (!parse_int_range(argv[1], 1, PURGE_DAYS_MAX, &parsed_days)) {
        outmsg = "Error: number > 0 required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    const uint64_t days = (uint64_t) parsed_days;
    const uint64_t seconds = days * SECONDS_IN_DAY;
    Tox_Bot.inactive_limit = seconds;

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t nlen = tox_friend_get_name_size(m, friendnum, NULL);
    name[nlen] = '\0';

    char msg[MAX_COMMAND_LENGTH];
    snprintf(msg, sizeof(msg), "Purge time set to %"PRIu64" days", days);
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) msg, strlen(msg), NULL);

    log_timestamp("Purge time set to %"PRIu64" days by %s", days, name);
}
//
//  master~friendnum right
//
static void cmd_status(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) {
    const char *outmsg = NULL;
    TOX_USER_STATUS type;
    const char *status = argv[1];

    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 1) {
        outmsg = "Error: status required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    if (strcasecmp(status, "online") == 0) {
        type = TOX_USER_STATUS_NONE;
    } else if (strcasecmp(status, "away") == 0) {
        type = TOX_USER_STATUS_AWAY;
    } else if (strcasecmp(status, "busy") == 0) {
        type = TOX_USER_STATUS_BUSY;
    } else {
        outmsg = "Invalid status. Valid statuses are: online, busy and away.";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    tox_self_set_status(m, type);

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t nlen = tox_friend_get_name_size(m, friendnum, NULL);
    name[nlen] = '\0';

    log_timestamp("%s set status to %s", name, status);
    save_data(m, DATA_FILE);
}
//
//      master~friendnum right
//      tox_conference_set_title
//
static void cmd_statusmessage(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]) {
    const char *outmsg = NULL;

    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 1) {
        outmsg = "Error: message required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    if (argv[1][0] != '\"') {
        outmsg = "Error: message must be enclosed in quotes";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    /* remove opening and closing quotes */
    char msg[MAX_COMMAND_LENGTH];
    unquote_str(argv[1], msg, sizeof(msg));

    tox_self_set_status_message(m, (uint8_t *) msg, strlen(msg), NULL);

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t nlen = tox_friend_get_name_size(m, friendnum, NULL);
    name[nlen] = '\0';

    log_timestamp("%s set status message to \"%s\"", name, msg);
    save_data(m, DATA_FILE);
}
//
//  master~friendnum right
//
static void cmd_title_set(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH])
{
    const char *outmsg = NULL;

    if (!friend_is_master(m, friendnum)) {
        authent_failed(m, friendnum);
        return;
    }

    if (argc < 2) {
        outmsg = "Error: Two arguments are required";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    if (argv[2][0] != '\"') {
        outmsg = "Error: title must be enclosed in quotes";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    int groupnum = 0;

    if (!parse_group_number(argv[1], &groupnum)) {
        outmsg = "Error: Invalid group number";
        tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
        return;
    }

    /* remove opening and closing quotes */
    char title[MAX_COMMAND_LENGTH];
    unquote_str(argv[2], title, sizeof(title));
    const int len = (int) strlen(title);

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnum, (uint8_t *) name, NULL);
    size_t nlen = tox_friend_get_name_size(m, friendnum, NULL);
    name[nlen] = '\0';

    TOX_ERR_CONFERENCE_TITLE err;

    if (!tox_conference_set_title(m, groupnum, (uint8_t *) title, len, &err)) {
        log_error_timestamp(err, "%s failed to set the title '%s' for group %d", name, title, groupnum);
        outmsg = "Failed to set title. This may be caused by an invalid group number or an empty room";
        send_error(m, friendnum, outmsg, err);
        return;
    }

    int idx = group_index(groupnum);

    if (idx != -1) {
        /* g_chats[].title is TOX_MAX_NAME_LENGTH bytes, which is shorter than a
         * command argument. Clamp before copying instead of relying on the
         * library having rejected an over-long title. */
        const size_t copy_len = MIN((size_t) len, sizeof(Tox_Bot.g_chats[idx].title) - 1);

        memcpy(Tox_Bot.g_chats[idx].title, title, copy_len);
        Tox_Bot.g_chats[idx].title[copy_len] = '\0';
        Tox_Bot.g_chats[idx].title_len = (int) copy_len;
    }

    outmsg = "Group title set";
    tox_friend_send_message(m, friendnum, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
    log_timestamp("%s set group %d title to %s", name, groupnum, title);
}

/* 
    Parses input command and puts args into arg array.
    Returns number of arguments on success, -1 on failure. 
*/
int commands_parse(const char *input, char (*args)[MAX_COMMAND_LENGTH])
{
    char *cmd = strdup(input);

    if (cmd == NULL) {
        exit(EXIT_FAILURE);
    }

    int num_args = 0;
    int i = 0;    /* index of last char in an argument */

    /* characters wrapped in double quotes count as one arg */
    while (num_args < MAX_NUM_ARGS) {
        int qt_ofst = 0;    /* set to 1 to offset index for quote char at end of arg */

        if (*cmd == '\"') {
            qt_ofst = 1;
            i = char_find(1, cmd, '\"');

            if (cmd[i] == '\0') {
                free(cmd);
                return -1;
            }
        } else {
            i = char_find(0, cmd, ' ');
        }

        /* An argument longer than the destination buffer is truncated: the
         * caller normally rejects such input, but this function must not depend
         * on that, since an over-long argument previously overflowed args[]. */
        size_t copy_len = (size_t) i + (size_t) qt_ofst;

        if (copy_len > (size_t) MAX_COMMAND_LENGTH - 1) {
            copy_len = (size_t) MAX_COMMAND_LENGTH - 1;
        }

        memcpy(args[num_args], cmd, copy_len);
        args[num_args++][copy_len] = '\0';

        if (cmd[i] == '\0') {  /* no more args */
            break;
        }

        /* cmd[i] is either the delimiter space (unquoted argument) or the
         * closing quote (quoted argument). Advance past it and past any
         * separating spaces; when nothing remains the argument list is
         * complete. Without this a quoted argument followed by more text
         * produced a spurious empty argument, and a quoted final argument
         * produced a trailing empty argument, both of which made argc one
         * larger than the caller expected. */
        const char *rest = &cmd[i + 1];

        while (*rest == ' ') {
            ++rest;
        }

        if (*rest == '\0') {
            break;
        }

        char tmp[MAX_COMMAND_LENGTH];
        snprintf(tmp, sizeof(tmp), "%s", rest);
        strcpy(cmd, tmp);    /* tmp will always fit inside cmd */
    }

    free(cmd);
    return num_args;
}

static struct {
    const char *name;

    void (*func)(Tox *m, uint32_t friendnum, int argc, char (*argv)[MAX_COMMAND_LENGTH]);
} commands[] = {
    { "/default",          cmd_default       },
    { "/group",            cmd_group         },
    { "/gmessage",         cmd_gmessage      },
    { "/help",             cmd_help          },
    { "/id",               cmd_id            },
    { "/info",             cmd_info          },
    { "/invite",           cmd_invite        },
    { "/leave",            cmd_leave         },
    { "/master",           cmd_master        },
    { "/name",             cmd_name          },
    { "/passwd",           cmd_passwd        },
    { "/purge",            cmd_purge         },
    { "/status",           cmd_status        },
    { "/statusmessage",    cmd_statusmessage },
    { "/title",            cmd_title_set     },
 /*   { "/list",             cmd_list_channels },    
    { "/join",             cmd_join_channel  },
    { "/part",             cmd_leave_channel },*/
    { NULL,               NULL              },
};
//
//
static int 
do_command(Tox *m, uint32_t friendnum, int num_args, char (*args)[MAX_COMMAND_LENGTH])
{
    for (size_t i = 0; commands[i].name; ++i) 
    {
        if (strcmp(args[0], commands[i].name) == 0) 
        {
            (commands[i].func)(m, friendnum, num_args - 1, args);
            return 0;
        }
    }

    return -1;
}
//
//  "/Command......"  incoming command parser
//  "/help  /info ........."
//
int execute(Tox *m, uint32_t friendnum, const char *input_msg, int length)
{
    if (length >= MAX_COMMAND_LENGTH) {
        return -1;
    }

    char args[MAX_NUM_ARGS][MAX_COMMAND_LENGTH];
    int num_args = commands_parse(input_msg, args);

    if (num_args == -1) {
        return -1;
    }

    return do_command(m, friendnum, num_args, args);
}

//----------------------------------------------------------------
//
//
void conference_basic_info_display( Tox *m )
{
    uint32_t groupchat_list[256];
    char public_key_str[128] = "";

    //
    // List active group chats and number of peers in each 
    size_t num_chats = tox_conference_get_chatlist_size(m);

    if (num_chats == 0) {
        console_out(" No active groupchats\n");
    };

    tox_conference_get_chatlist(m, groupchat_list);

    for (size_t i = 0; i < num_chats; ++i) 
    {
        TOX_ERR_CONFERENCE_PEER_QUERY err;
        uint32_t groupnum = groupchat_list[i];
        uint32_t num_peers = tox_conference_peer_count(m, groupnum, &err);
        char type[128];

        if (err == TOX_ERR_CONFERENCE_PEER_QUERY_OK) {

            if ( tox_conference_get_type(m, groupnum, NULL) == TOX_CONFERENCE_TYPE_AV ) {
                strcpy( type, "AV ");
            } else
                strcpy( type, "Text");

            Tox_Err_Conference_Title t_err;
            uint8_t title[256];
            int length = tox_conference_get_title_size(m, groupnum, &t_err);
            if (t_err != TOX_ERR_CONFERENCE_TITLE_OK)   length = 0;
            tox_conference_get_title(m, groupnum, title, &t_err);
            title[length] = '\0';

            console_out( "\n Room: %02d | %s | online peers: %d | Title: %s\n", groupnum, type, num_peers, title);
                            
            console_out("--------------------------------------------------------\n");
            
            // conference peer nickname 
            unsigned char nick_name[128] = "";
            for( uint32_t i = 0; i < num_peers; i++ ) {
                int length = tox_conference_peer_get_name_size( m, groupnum, i, NULL);
                tox_conference_peer_get_name(m, groupnum, i, nick_name, NULL);
                nick_name[length] = '\0';

                // conference peer public_key
                uint8_t public_key[TOX_PUBLIC_KEY_SIZE+1];
                public_key_str[0] = 0;

                TOX_ERR_CONFERENCE_PEER_QUERY err;
                tox_conference_peer_get_public_key(m, groupnum, i, public_key, &err);
                if( err == TOX_ERR_CONFERENCE_PEER_QUERY_OK ) {
                    char a[3];
                    for (size_t k = 0; k < TOX_PUBLIC_KEY_SIZE; ++k) {
                    snprintf(a, sizeof(a), "%02X", public_key[k] & 0xff);
                    strcat(public_key_str, a);
                    };
                } else
                    public_key_str[0] = 0;

                console_out( " %03d conference member   nick_name: %s\n", i, nick_name);
            }
        }
    };
    
// offline.... room number = num_chats
    console_out("\n offline conference member :\n");
    char nick_name[128] = "";

    for (size_t i = 0; i < num_chats; ++i) 
    {
                uint32_t groupnum = groupchat_list[i];
                // offline conference peer display message
                int offline_num = tox_conference_offline_peer_count(m, groupnum, NULL);
                if( offline_num <= 0 ) continue;

                for( int i = 0; i < offline_num; i++ ) {
                    int length = tox_conference_offline_peer_get_name_size( m, groupnum, i, NULL);
                    tox_conference_offline_peer_get_name(m, groupnum, i, (uint8_t *) nick_name, NULL);
                    nick_name[length] = '\0';

                    console_out(" %03d conference offline  nick_name: %s\n", groupnum, nick_name);
                }
    };

    console_out("\n");
    //tox_conference_connected_cb();
    return;
}