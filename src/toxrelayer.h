/*  toxrelayer.h
 *
 *
 *  Copyright (C) 2021 toxrelayer All Rights Reserved.
 *
 *  This file is part of toxrelayer.
 *
 *  toxrelayer is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  toxrelayer is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with toxrelayer. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef TOXRELAYER_H
#define TOXRELAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <tox/tox.h>
#include "groupchats.h"

#define MAX_NUM_GROUPS 256

/* On-disk identity: the toxcore save file. It holds the bot's private key, so
 * this file *is* the bot's address on the network.
 *
 * The name is deliberately still the pre-rename "toxbot.tox". Renaming the
 * project must not rename the account: a relayer started without this file
 * generates a fresh identity, orphaning every contact, every master key entry
 * and every stored serial number. Existing deployments keep using this file.
 * Migrating the name would need an explicit, failure-safe rename step (see
 * legacy_data_file_rename()) and is left as separate work. */
#define DATA_FILE        "toxbot.tox"
#define MASTERLIST_FILE  "masterkeys"
#define BLOCKLIST_FILE   "blockedkeys"

/* Durable mirror of the delivery queue, written by mq_persist.c. Keeping the
 * name here next to the other on-disk artefacts makes the files a deployment
 * has to preserve obvious and central. */
#define QUEUE_FILE       "mq_queue.dat"

typedef unsigned long size_t;

struct Tox_Bot {
    time_t     start_time;          // time toxrelayer was started
    time_t     last_connected;      // time we last connected to the network
    time_t     last_bootstrap;      // last time we tried to bootstrap
    
    uint64_t   inactive_limit;      // how often we purge inactive contacts
    int        default_groupnum;    // the group that invite commands with no ID default to
    int        num_online_friends;
    int        chats_idx;

    struct Group_Chat *g_chats;
};

/* Upper bound for every array that is indexed by a toxcore friend number.
 *
 * toxcore reuses freed friend numbers, so this is a generous ceiling for the
 * number of contacts a relay ever holds at once, not a "maximum concurrent
 * friends" promise. Rules that all code must follow:
 *   - any array indexed by a friend number is sized with this constant;
 *   - every index is validated with friend_number_valid() before use.
 */
#define MAX_Friend_NUM      4096

/* True when `friendnumber` is a safe index into every MAX_Friend_NUM sized
 * array. Call this before indexing relay_name_list[], chat_tx_Control[] or any
 * other friend-number keyed storage. */
static inline bool friend_number_valid(uint32_t friendnumber)
{
    return friendnumber < MAX_Friend_NUM;
}

//  send_number = 1..999
//  VIP:    tele_send_number
struct Chat_TX_Control {
    time_t  last_msg_tx;        // last time msg send&receive, idele 20s send test signal
    int     hello_counter;       // hello command number
    int     send_number;        // sending msg serial number
    int     receive_number;     // receive msg serial number
    int     tele_send_number;   // VIP: sending msg tele-serial number
};

/* One entry per friend number, defined once in toxrelayer.c.
 *
 * This used to be a definition in the header. That is a tentative definition, so
 * every translation unit that included the header emitted its own copy; older
 * toolchains merged them silently, but GCC 10+ and LLVM 11+ compile with
 * -fno-common and fail at link time with "duplicate symbol". */
extern struct Chat_TX_Control chat_tx_Control[MAX_Friend_NUM];



int load_Masters(const char *path);
int save_data(Tox *m, const char *path);
bool friend_is_master(Tox *m, uint32_t friendnumber);
int get_conference_nick_truncate(Tox *m, char *buf, uint32_t peernum, uint32_t conferencenum);

#endif /* TOXRELAYER_H */

