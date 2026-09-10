/*  
    groupchats.c
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toxrelayer.h"
#include "groupchats.h"
#include "log.h"

void conference_peer_join_left( Tox *m, int groupnum );
void conference_peer_list_initial( Tox *m);
void log_timestamp(const char *message, ...);

extern struct Tox_Bot Tox_Bot;

/* Number of slots actually allocated. Tracked here so that growing the table
 * can initialise only the newly added region; without this the fresh slots hold
 * indeterminate bytes and group_add() reads them as a bool, which is undefined
 * behaviour (caught by UndefinedBehaviorSanitizer). */
static size_t g_allocated;

//
void realloc_groupchats(int n)
{
    if (n <= 0) {
        free(Tox_Bot.g_chats);
        Tox_Bot.g_chats = NULL;
        g_allocated = 0;
        return;
    }

    struct Group_Chat *g = realloc(Tox_Bot.g_chats, (size_t) n * sizeof(struct Group_Chat));

    if (g == NULL) {
        exit(EXIT_FAILURE);
    }

    /* Every slot must start inactive: the table is searched for a free entry by
     * inspecting `active`. */
    if ((size_t) n > g_allocated) {
        memset(&g[g_allocated], 0, ((size_t) n - g_allocated) * sizeof(struct Group_Chat));
    }

    Tox_Bot.g_chats = g;
    g_allocated = (size_t) n;
}
//----------------------------------------------------------------
//
//
int group_add(uint32_t groupnum, uint8_t type, const char *password)
{
    /* Bound the index before it is used as an array subscript or passed to
     * realloc_groupchats(). chats_idx is the number of slots in use. */
    if (Tox_Bot.chats_idx < 0 || Tox_Bot.chats_idx >= MAX_NUM_GROUPS) {
        return -1;
    }

    realloc_groupchats(Tox_Bot.chats_idx + 1);

    /* realloc_groupchats() may have emptied the storage; never memset through
     * a NULL pointer. */
    if (Tox_Bot.g_chats == NULL) {
        return -1;
    }

    memset(&Tox_Bot.g_chats[Tox_Bot.chats_idx], 0, sizeof(struct Group_Chat));

    for (int i = 0; i <= Tox_Bot.chats_idx && i < MAX_NUM_GROUPS; ++i) 
    {
        if (Tox_Bot.g_chats[i].active) {
            continue;
        }

        memset(&Tox_Bot.g_chats[i], 0, sizeof(struct Group_Chat));
        Tox_Bot.g_chats[i].groupnum = groupnum;
        Tox_Bot.g_chats[i].active = true;
        Tox_Bot.g_chats[i].type = type;

        if (password) {
            Tox_Bot.g_chats[i].has_pass = true;
            snprintf(Tox_Bot.g_chats[i].password, sizeof(Tox_Bot.g_chats[i].password), "%s", password);
        }

        if (Tox_Bot.chats_idx == i) {
            ++Tox_Bot.chats_idx;
        }

        return 0;
    }

    return -1;
}
//----------------------------------------------------------------
//
void group_leave(uint32_t groupnum)
{
    int i;

    for (i = 0; i < Tox_Bot.chats_idx; ++i) {
        if (Tox_Bot.g_chats[i].active && Tox_Bot.g_chats[i].groupnum == groupnum) {
            memset(&Tox_Bot.g_chats[i], 0, sizeof(struct Group_Chat));
            break;
        }
    }

    for (i = Tox_Bot.chats_idx; i > 0; --i) {
        if (Tox_Bot.g_chats[i - 1].active) {
            break;
        }
    }

    Tox_Bot.chats_idx = i;
    realloc_groupchats(i);
}
//----------------------------------------------------------------
//
int group_index(uint32_t groupnum)
{
    for (int i = 0; i < Tox_Bot.chats_idx; ++i) {
        if (Tox_Bot.g_chats[i].active && Tox_Bot.g_chats[i].groupnum == groupnum) {
            return i;
        }
    }

    return -1;
}
////////////////////////////////////////////////////////////////
//
//  chat_room process functions
//
#define MAX_ROOM_PEER_NUMBER        1024
#define MAX_ROOM_NUMBER             128
//
struct room_peer_list {
    char room_peer_nick_name[128];
    char public_key_str[100];       // 76 characters
    bool online_flag;               // true or false
};

struct room_peer_list  room_peer_list[MAX_ROOM_NUMBER][MAX_ROOM_PEER_NUMBER];
int room_peer_list_number[MAX_ROOM_NUMBER];
//
//  test room 00 first
//
void conference_peer_list_initial( Tox *m)
{
    uint32_t groupchat_list[256];
    uint8_t public_key[TOX_PUBLIC_KEY_SIZE+1];
    char public_key_str[128];
    unsigned char nick_name[64];

    for( int room = 0; room < MAX_ROOM_NUMBER; room++ )
    for( int i = 0; i < MAX_ROOM_PEER_NUMBER ; i++ ) {
        room_peer_list[room][i].public_key_str[0]      = 0;
        room_peer_list[room][i].room_peer_nick_name[0] = 0;
    };

    size_t num_chats = tox_conference_get_chatlist_size(m);     // room number
    if (num_chats == 0) {
        console_out(" No active groupchats.\n");
        return;
    };

    tox_conference_get_chatlist(m, groupchat_list);
    
    for( size_t room = 0; room < num_chats; room++ ) 
    {
    uint32_t groupnum = groupchat_list[room];         // room = 0;
    uint32_t online_num = tox_conference_peer_count(m, groupnum, NULL);
    uint32_t offline_num = tox_conference_offline_peer_count(m, groupnum, NULL);
    room_peer_list_number[room] = online_num + offline_num;

    // chat_room peer online case ................
    for (uint32_t i = 0; i < online_num; ++i) {
        tox_conference_peer_get_public_key(m, groupnum, i, public_key, NULL);

        char a[3] = "";
        public_key_str[0] = 0;
        for (size_t i = 0; i < TOX_PUBLIC_KEY_SIZE; ++i) {
            snprintf(a, sizeof(a), "%02X", public_key[i] & 0xff);
            strcat(public_key_str, a);
        };
        strcpy( room_peer_list[room][i].public_key_str, public_key_str);
        room_peer_list[room][i].online_flag = true;

        int length = tox_conference_peer_get_name_size( m, groupnum, i, NULL);
        tox_conference_peer_get_name(m, groupnum, i, (uint8_t *) nick_name, NULL);
        nick_name[length] = '\0';
        strcpy(room_peer_list[room][i].room_peer_nick_name, (const char *) nick_name);
        // line_info_add(wm, false, NULL, NULL, SYS_MSG, 0, CYAN, room_peer_list[room][i].room_peer_nick_name);
    };

    // chat_room peer offline case ................
    for(uint32_t i = 0; i < offline_num; i++) {
        tox_conference_offline_peer_get_public_key(m, groupnum, i, public_key, NULL);

        char a[3] = "";
        public_key_str[0] = 0;
        for (size_t i = 0; i < TOX_PUBLIC_KEY_SIZE; ++i) {
            snprintf(a, sizeof(a), "%02X", public_key[i] & 0xff);
            strcat(public_key_str, a);
        };
        strcpy( room_peer_list[room][online_num+i].public_key_str, public_key_str);
        room_peer_list[room][online_num+i].online_flag = false;

        int length = tox_conference_offline_peer_get_name_size( m, groupnum, i, NULL);
        tox_conference_offline_peer_get_name(m, groupnum, i, nick_name, NULL);
        nick_name[length] = '\0';
        strcpy(room_peer_list[room][online_num+i].room_peer_nick_name, (const char *) nick_name);
        // line_info_add(wm, false, NULL, NULL, SYS_MSG, 0, CYAN, room_peer_list[room][online_num+i].public_key_str);
    };
    };           
    return;
}
//----------------------------------------------------------------
//
//      who join? who left room?
//      room_peer_list[MAX_ROOM_PEER_NUMBER] contains all the room members 
//      
//
void conference_peer_join_left( Tox *m, int groupnum )
{
    uint8_t public_key[TOX_PUBLIC_KEY_SIZE+1];
    char public_key_str[128];
    char nick_name[64];
    struct room_peer_list room_online_peer_list[MAX_ROOM_PEER_NUMBER];

    //
    // List active group chats and number of peers in each 
    size_t num_chats = tox_conference_get_chatlist_size(m);
    if (num_chats == 0) {
        console_out(" No active groupchats\n");
        return;
    }


    // test room = 0 first
    // int groupnum = 0;
    uint32_t online_num = tox_conference_peer_count(m, groupnum, NULL);

    if( online_num == 0 ) { return; }

    for (uint32_t i = 0; i < online_num; ++i) {
        tox_conference_peer_get_public_key(m, groupnum, i, public_key, NULL);

        char a[3] = "";
        public_key_str[0] = 0;
        for (size_t i = 0; i < TOX_PUBLIC_KEY_SIZE; ++i) {
            snprintf(a, sizeof(a), "%02X", public_key[i] & 0xff);
            strcat(public_key_str, a);
        };
        strcpy( room_online_peer_list[i].public_key_str, public_key_str);

        int length = tox_conference_peer_get_name_size( m, groupnum, i, NULL);
        tox_conference_peer_get_name(m, groupnum, i, (uint8_t *) nick_name, NULL);
        nick_name[length] = '\0';
        strcpy(room_online_peer_list[i].room_peer_nick_name, nick_name);
    };

    //  room = 0
    //  seach_peer from start...end;
    //  peer_online_flag = true     in online_list      : normal
    //  peer_online_flag = false    not in online_list  : normal
    //  peer_online_flag = false    in online_list      : join the room, change peer_flag;
    //  peer_online_flag = true     not in online_list  : left the room, change peer_flag;
    //
    char search_key[128] = "";
    bool in_online_list_flag = false;

    for ( int i = 0; i < room_peer_list_number[groupnum]; i++) {
        strcpy( search_key, room_peer_list[groupnum][i].public_key_str);
        in_online_list_flag = false;

        // search_key in online_peer list ?
        for( uint32_t j = 0; j < online_num; j++) {
            
           // peer in online_list
           if( strstr( room_online_peer_list[j].public_key_str, search_key) != NULL ) {

               // room_peer_list[i]: join the room case ++ , new come peer!
              if( room_peer_list[groupnum][i].online_flag == false ) {
                  log_timestamp("\033[36m %s joint the room: %02d\033[0m", room_peer_list[groupnum][i].room_peer_nick_name, groupnum);
                  room_peer_list[groupnum][i].online_flag = true;
                  return;
              }
              in_online_list_flag = true;
           }
        };


        if( in_online_list_flag == false )  // not found, not in list
        {
            // not in list, but peer_online_flag = true  --> left case
         if( room_peer_list[groupnum][i].online_flag == true ) {
            log_timestamp("\033[36m %s left the room: %02d\033[0m", room_peer_list[groupnum][i].room_peer_nick_name, groupnum);

            room_peer_list[groupnum][i].online_flag = false;
            return;
         }
        }
    };

    return;
}