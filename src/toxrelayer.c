/*  toxrelayer.c
    --------
 *   auto-invite friends to the specified group chat 
 * 
 *  password protect invites, and send messages to groups.
 * 
 *  Although current functionality is barebones, it will be easy to expand the bot to act in more 
 *  comprehensive ways once Tox group chats are fully implemented (e.g. admin duties); 
 *  this was the main motivation behind creating a proper Tox bot.
 * 
 *  In order to control the bot you must add your Tox ID to the masterkeys 
 * 
 *  large file tranfer:  file_size <= 128K, in chat trans; nible_pic
 *                       file_size > 128K , send file_store_link: ipfs.......;
 * 
 *  image/file quick_server: ipfs...; ??? private_network
 * 
 *  dot_mark = (publickey, serial_number)
 *  when friend online: 
 *      1. check dot_mark list ....... chat_room_msg_sync
 *      2. p2p msg_sync
 */

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <signal.h>
#include <getopt.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <sys/select.h>
#include <ctype.h>
#include <sqlite3.h>

#include <tox/tox.h>
#include <tox/toxav.h>
#include <tox/toxencryptsave.h>
#include "minIni.h"

#include "misc.h"
#include "commands.h"
#include "toxrelayer.h"
#include "groupchats.h"
#include "msg_database.h"
#include "msg_queue.h"
#include "mq_persist.h"
#include "telegram.h"
#include "relay.h"
#include "log.h"

#define VERSION "0.1.2"

/* How often we attempt to purge inactive friends */
#define FRIEND_PURGE_INTERVAL       (60ULL * 60ULL)

/* How often we attempt to purge inactive groups */
#define GROUP_PURGE_INTERVAL        (60ULL * 10ULL)

/* How long we need to have had a stable connection before purging inactive groups */
#define GROUP_PURGE_CONNECT_TIMEOUT (60ULL * 60ULL)

/* Bootstrap frequncy when "not presently connected" to the network */
#define BOOTSTRAP_INTERVAL      20       
#define HELL_SIGNAL_INTERVAL    16      //  OK == 15       

/* How often a changed delivery queue is written to disk. The flush is also
 * forced on exit, so this only bounds how much work a crash can cost. */
#define QUEUE_SAVE_INTERVAL     5

#define MAX_PORT_RANGE          65535
//#define TOXIC_MAX_NAME_LENGTH   128

#define sizearray(a)    (sizeof(a) / sizeof((a)[0]))
/* Name of data file prior to version 0.1.1 */
#define DATA_FILE_PRE_0_1_1 "arkmeta_save"

int   ini_puts(const mTCHAR *Section,
               const mTCHAR *Key,
               const mTCHAR *Value,
               const mTCHAR *Filename);
int   ini_gets(const mTCHAR *Section,
               const mTCHAR *Key,
               const mTCHAR *DefValue,
               mTCHAR *Buffer,
               int BufferSize,
               const mTCHAR *Filename);
//           

char config_file[64]   = "metaCom.config";



char Bot_Name[] = "ToxRelayer@元宇宙";
char hello_selfname_msg[64] = "/HELLO  BCEDDDDD  METACOM";
uint32_t pre_friendnumber = 0 ;

char tx_name[128] = "";
int friend_name_size;
char token[128];
int lineindex = 0;

int max_friend_num = 0;
// time_t  last_msg_tx;        // last time msg send&receive, idele 20s send test signal

volatile sig_atomic_t FLAG_EXIT = false;    /* set on SIGINT */

struct Tox_Bot Tox_Bot;

static struct Options {
    TOX_PROXY_TYPE    proxy_type;
    char      proxy_host[256];
    uint16_t  proxy_port;
    bool      disable_udp;
    bool      disable_lan;
    bool      force_ipv4;
} Options;

#define TIMESTAMP_SIZE                  64
#define MAX_MESSAGE_SIZE                512

/* TOX_MAX_MESSAGE_LENGTH comes from tox.h; do not redefine it here. */
#define MAX_MSG_QUEUE_NUM              2048         // 4k messages in sending queue
#define MAX_STR_SIZE                   TOX_MAX_MESSAGE_LENGTH 
#define MIN_PASSWORD_LEN                6
#define MAX_PASSWORD_LEN                64      
// 
//
//

/* MAX_Friend_NUM is defined once, in toxrelayer.h. Do not redefine it here. */
//
struct RELAY_NAME_LIST {
    int  friend_number;
    char public_key_str[128];
    char nick_name[128];
    bool line_connect;          // true = onlone, false = offline
    bool meta_chat_flag;        // true = yes, false = no
    int  send_sn;               // send telegram message serial_number, 1..999
    int  receive_sn;            // receive telegram serial_number, 1..999
} relay_name_list[MAX_Friend_NUM];

/* Receive-side serial-number tracking, one entry per friend.
 *
 * This is what turns "best effort" relaying into reliable store-and-forward:
 * an arriving serial that is not the expected one tells us exactly which
 * telegrams never made it, and a control telegram asks the peer to replay them.
 *
 * TELEGRAM_MAX_GAP bounds the replay request; beyond it the tracker
 * resynchronises instead of asking for a very large replay.
 * TELEGRAM_REPLAY_MAX bounds how many telegrams one request can answer. */
#define TELEGRAM_MAX_GAP      64
#define TELEGRAM_REPLAY_MAX   64

static sn_tracker recv_tracker[MAX_Friend_NUM];

char self_public_key_str[128];   // me_pk

// relay_server_list
char relay_public_key[128] = "6803B6738011AF6CEB44BC5AF0F62BFD2C99BEA32F19D8C1663164A46272465C67C517BA4E3E";
int  relay_number = 0;

struct RELAYER_LIST {
    char relay_public_key[128];
    int  relay_number;
};
//
struct MSG_DISPLAY {
    uint32_t receipt;      /* toxcore read receipt; 0 means "none" */
    char msg[1024];
    char selfname[256];
} msg_keeper;

////////////////////////////////////////////////////////////////
//
#define MAX_JOIN_ROOM_NUM       256             // room_num each client can join.
#define MAX_ROOM_PEER_NUMBER    500

int self_room_send_msg_sn[MAX_JOIN_ROOM_NUM];   // room_msg sending: 1..999

// each room have 500 peers, MAX_JOIN_ROOM_NUM*500 msg_sn iterms.
int room_send_msg_sn[MAX_JOIN_ROOM_NUM][MAX_ROOM_PEER_NUMBER];

/*
    ZCZC  TEXT
    sender_public_key;
    room_id room_send_msg_sn
    text ................
    NNNN
*/

struct arg_opts {
    bool use_ipv4;
    bool force_tcp;
    bool disable_local_discovery;
    bool debug;
    bool default_locale;
    bool use_custom_data;
    bool no_connect;
    bool encrypt_data;
    bool unencrypt_data;

    char nameserver_path[MAX_STR_SIZE];
    char config_path[MAX_STR_SIZE];
    char nodes_path[MAX_STR_SIZE];

    bool logging;
    FILE *log_fp;

    char proxy_address[256];
    uint8_t proxy_type;
    uint16_t proxy_port;

    uint16_t tcp_port;
};

struct arg_opts arg_opts;

static struct user_password {
    bool data_is_encrypted;
    char pass[MAX_PASSWORD_LEN + 1];
    int len;
} user_password;

void get_relay_name_list( Tox *m );
void local_info_display(Tox *m);
void conference_basic_info_display( Tox *m );
void evaluate_input( Tox *tox,char *user_input );
static void cb_friend_read_receipt( Tox *m, uint32_t friendnumber, uint32_t receipt, void *userdata);
void receipt_display( Tox *m, uint32_t friendnumber, uint32_t receipt);
int store_data(Tox *m, const char *path);

static void
cb_conference_message(Tox *m, uint32_t cnum, uint32_t pnum, TOX_MESSAGE_TYPE type, const uint8_t *data, size_t len, void *udata);
void on_conference_peer_list_changed(Tox *m, uint32_t conferencenumber, void *userdata);
void on_friend_name(Tox *m, uint32_t friendnumber, const uint8_t *string, size_t length, void *userdata);

void get_token( const char *mess );
int lookup_ch_num( char *public_key);
void get_relay_name_list( Tox *m );
time_t get_unix_time(void);
void set_relayer_public_key( char *user_input_msg);
int lookup_ch_num( char *public_key);

void welcom_logo_metaCom();

////
time_t get_time(void);
void get_time_str(char *buf, size_t bufsize);
void local_node_info_display( Tox *m );

void conference_peer_list_initial( Tox *m);
void conference_peer_join_left( Tox *m, int groupnum );


////////////////////////////////////////////////////////////////////////////
//
static void init_toxrelayer_state(void)
{
    Tox_Bot.start_time      = get_time();
    Tox_Bot.last_connected  = get_time();
    Tox_Bot.default_groupnum = 0;
    Tox_Bot.chats_idx = 0;
    Tox_Bot.num_online_friends = 0;

    //* 1 year default; 
    // anything lower should be explicitly set until we have a config file 
    Tox_Bot.inactive_limit = 31536000ULL * 5ULL;

    return;
}
//----------------------------------------------------------------
// 
static void catch_SIGINT(int sig)
{
    /* Required by the callback signature; unused here. */
    (void) sig;
    FLAG_EXIT = true;
}
//----------------------------------------------------------------
//
static void exit_toxrelayer(Tox *m)
{
    save_data(m, DATA_FILE);

    /* The delivery queue is the only in-memory state that cannot be rebuilt
     * from toxcore, so it is flushed on the way out and its failure reported. */
    if (mq_persist_save(QUEUE_FILE) != MQ_PERSIST_OK) {
        log_error_timestamp(-1, "Cannot persist the delivery queue to %s", QUEUE_FILE);
    }

    tox_kill(m);
    exit(EXIT_SUCCESS);
}
/* 
    Returns true if friendnumber's Tox ID is in the masterkeys list. 
*/
bool friend_is_master(Tox *m, uint32_t friendnumber) 
{
    char public_key[TOX_PUBLIC_KEY_SIZE];

    if (tox_friend_get_public_key(m, friendnumber, (uint8_t *) public_key, NULL) == 0) {
        return false;
    }

    return file_contains_key(public_key, MASTERLIST_FILE) == 1;
}
/* 
    Returns true if public_key is in the blockedkeys list. 
*/
static bool public_key_is_blocked(const char *public_key)
{
    return file_contains_key(public_key, BLOCKLIST_FILE) == 1;
}
//----------------------------------------------------------------
//  initial:  relay_name_list
//  self_public_key_str
//
void get_relay_name_list( Tox *m )
{    
    char PublicKey[128], node_id[128] = "";
    char key_buffer[128];
    uint32_t numfriends = tox_self_get_friend_list_size(m);
    // tox_self_get_friend_list(m, list);

    if( numfriends < 0 || numfriends > MAX_Friend_NUM ) {
        console_out("number of friends outside : %d\n", numfriends);
        return;
    }

    console_out("\n>> metaCom Contacts Data List\n"); 

    for(uint32_t i = 0; i < numfriends; i++) {
        node_id[0] = 0;
        tox_friend_get_public_key(m, i, (uint8_t*) PublicKey, NULL);

        char nick_name[128];
        int friend_size = tox_friend_get_name_size(m, i, NULL);
        tox_friend_get_name(m, i, (uint8_t*)nick_name,NULL);
        nick_name[friend_size] = 0;

        for (size_t i = 0; i < TOX_PUBLIC_KEY_SIZE; ++i) {
            char a[3];
            snprintf(a, sizeof(a), "%02X", PublicKey[i] & 0xff);
            strcat(node_id, a);
        };
        // console_out(" %02d public_key : %s\n" , i, node_id);

        relay_name_list[i].friend_number = i;
        strcpy( relay_name_list[i].public_key_str, node_id);
        strcpy( relay_name_list[i].nick_name, nick_name);

        relay_name_list[i].meta_chat_flag   = false;
        relay_name_list[i].line_connect     = false;;

        char sn_buffer[128] = "";
        long sn_value = TG_SN_MIN;

        snprintf(key_buffer, sizeof(key_buffer), "send%03d", i);
        int ret = ini_gets( "outgoing_sn", key_buffer, "1", sn_buffer, sizearray(sn_buffer), config_file);
        if( ret < 1 ) {
            console_out("ini_gets open failure.");
            continue;
        };
        /* A serial outside 1..TG_SN_MAX is not a serial. Falling back to the
         * INI default is safer than storing an out-of-range value that the
         * telegram layer would later reject. */
        if (!parse_int_range(sn_buffer, TG_SN_MIN, TG_SN_MAX, &sn_value)) {
            sn_value = TG_SN_MIN;
        }
        relay_name_list[i].send_sn = (int) sn_value;

        snprintf(key_buffer, sizeof(key_buffer), "recev%03d", i);
        int ret2 = ini_gets( "incoming_sn", key_buffer, "1", sn_buffer, sizearray(sn_buffer), config_file);
        if( ret2 < 1 ) {
            console_out("ini_gets open failure.");
            continue;
        };
        if (!parse_int_range(sn_buffer, TG_SN_MIN, TG_SN_MAX, &sn_value)) {
            sn_value = TG_SN_MIN;
        }
        relay_name_list[i].receive_sn = (int) sn_value;

        snprintf(key_buffer, sizeof(key_buffer), "meta%03d", i);
        int ret3 = ini_gets( "meta_chat_flag", key_buffer, "0", sn_buffer, sizearray(sn_buffer), config_file);
        if( ret3 < 1 ) {
            console_out("ini_gets open failure.");
            continue;
        };
        /* A malformed flag disables metaCom framing for the channel. Failing
         * safe matters here: assuming a peer speaks the protocol would send it
         * a record it cannot parse. */
        long meta_flag = 0;

        if (!parse_int_range(sn_buffer, 0, 1, &meta_flag)) {
            meta_flag = 0;
        }

        relay_name_list[i].meta_chat_flag = (meta_flag != 0);
    };
    
    console_out("\n");

    char self_id_string[TOX_ADDRESS_SIZE * 2 + 1] = "";
    char self_bin_id[TOX_ADDRESS_SIZE+1];

    tox_self_get_address( m, (uint8_t *) self_bin_id);

    for (size_t i = 0; i < TOX_ADDRESS_SIZE; ++i) {
        char d[3];
        snprintf(d, sizeof(d), "%02X", self_bin_id[i] & 0xff);
        memcpy( self_id_string + i * 2, d, 2);
    };

    strncpy( hello_selfname_msg, self_id_string, 8);
    hello_selfname_msg[8] = 0;

    strcat( hello_selfname_msg, "  METACOM");
    strcpy( self_public_key_str, self_id_string);

    // metaCom-relayer channel process
    int ret = ini_gets( "metaCom_Relayer","relay_public_key",
                        "6803B6738011AF6CEB44BC5AF0F62BFD2C99BEA32F19D8C1663164A46272465C67C517BA4E3E",
                        PublicKey, sizearray(PublicKey), config_file);
    if( ret < 1 ) {
        console_out("ini_gets relay_public_key open failure.\n");
        relay_number = 0;
    } else
        relay_number = lookup_ch_num( PublicKey );

    console_out("relay_public_key = %d\n", relay_number);

   return;
}
//
///////////////////////////////////////////////////////////////////////////
//  
/*
int lookup_ch_num( char *public_key)
{  
    for (int i = 0; i < MAX_Friend_NUM ; i++ ) {
        // console_out("lookup : %d   %s\n", i, relay_name_list[i].public_key);
        if( strstr( public_key, relay_name_list[i].public_key) != NULL ) {
            return i;
        }
    }
}
*/
////////////////////////////////////////////////////////////////
//    
//      START CALLBACKS 
//
static void cb_self_connection_change(Tox *m, TOX_CONNECTION connection_status, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) m;
    (void) userdata;
    switch (connection_status) {
        case TOX_CONNECTION_NONE:
            log_timestamp("Connection lost");
            Tox_Bot.last_bootstrap = get_time(); // usually we don't need to manually bootstrap if connection lost
            break;

        case TOX_CONNECTION_TCP:
            Tox_Bot.last_connected = get_time();
            log_timestamp("Connection established (TCP)");
            for( int i = 0; i < max_friend_num ; i++ ) {
                chat_tx_Control[i].last_msg_tx = get_time();
            };
            break;

        case TOX_CONNECTION_UDP:
            Tox_Bot.last_connected = get_time();
            log_timestamp("Connection established (UDP)");
            for( int i = 0; i < max_friend_num ; i++ ) {
                chat_tx_Control[i].last_msg_tx = get_time();
            };
            break;
    }
    return;
}
//
//      send Message ................
//
static void cb_friend_connection_change(Tox *m, uint32_t friendnumber, TOX_CONNECTION connection_status, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) connection_status;
    (void) userdata;
    char out_buffer[256];

    Tox_Bot.num_online_friends = 0;     // online friends number

    size_t i, size = tox_self_get_friend_list_size(m);

    if (size == 0) {
        return;
    };

    /* relay_name_list[] is indexed by friend number; toxrelayer.h requires that
     * every such index be validated before use. */
    if (!friend_number_valid(friendnumber)) {
        log_error_timestamp(-1, "Friend connection change for out-of-range friend number %u",
                            friendnumber);
        return;
    }

    if( tox_friend_get_connection_status(m, friendnumber, NULL) != TOX_CONNECTION_NONE )
        snprintf( out_buffer, sizeof(out_buffer), "\033[36m %03d :   online  %s\033[0m", friendnumber, relay_name_list[friendnumber].nick_name);
    else
        snprintf( out_buffer, sizeof(out_buffer), "\033[36m %03d :  offline  %s\033[0m", friendnumber, relay_name_list[friendnumber].nick_name);

    log_timestamp( "%s", out_buffer); 

    uint32_t list[size];
    tox_self_get_friend_list(m, list);

    for (i = 0; i < size; ++i) {
        if (tox_friend_get_connection_status(m, list[i], NULL) != TOX_CONNECTION_NONE) 
        {
            ++Tox_Bot.num_online_friends;
        }
    }

    return;
}
//
//
/* ---------------------------------------------------------------------------
 * Delivery transport adapter.
 *
 * msg_queue.h is deliberately free of toxcore dependencies; this adapter is the
 * single place where the store-and-forward queue meets the network. It maps the
 * toxcore error enumeration onto the queue's retry policy.
 * ------------------------------------------------------------------------- */
_Static_assert(TOX_MAX_MESSAGE_LENGTH <= MQ_PAYLOAD_MAX,
               "MQ_PAYLOAD_MAX must be at least TOX_MAX_MESSAGE_LENGTH");

static int tox_transport(void *ctx, int friendnum, const char *payload, size_t len)
{
    Tox *m = (Tox *) ctx;

    if (m == NULL || payload == NULL || friendnum < 0) {
        return MQ_SEND_FATAL;
    }

    if (!friend_number_valid((uint32_t) friendnum)) {
        return MQ_SEND_FATAL;
    }

    if (tox_friend_get_connection_status(m, (uint32_t) friendnum, NULL) == TOX_CONNECTION_NONE) {
        return MQ_SEND_RETRYABLE;   /* peer offline: keep the telegram queued */
    }

    Tox_Err_Friend_Send_Message err = TOX_ERR_FRIEND_SEND_MESSAGE_OK;

    const uint32_t receipt = tox_friend_send_message(m, (uint32_t) friendnum,
                                                    TOX_MESSAGE_TYPE_NORMAL,
                                                    (const uint8_t *) payload, len, &err);

    if (err != TOX_ERR_FRIEND_SEND_MESSAGE_OK) {
        return MQ_SEND_RETRYABLE;
    }

    return (int) receipt;
}

/* Reports retired telegrams so an operator can see that a message was lost. */
static void on_telegram_dropped(void *ctx, int friendnum, uint32_t attempts)
{
    (void) ctx;
    log_error_timestamp(-1, "Telegram to friend %d retired after %u attempts (undeliverable)",
                        friendnum, attempts);
}

/* Set by the queue whenever a message is added, confirmed or retired, so the
 * event loop knows a flush to QUEUE_FILE is worth doing. */
static volatile sig_atomic_t g_queue_dirty = false;

static void on_queue_changed(void *ctx)
{
    (void) ctx;
    g_queue_dirty = true;
}

static void cb_friend_read_receipt( Tox *m, uint32_t friendnumber, uint32_t receipt, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) userdata;
    // console_out("cb_callback: \033[32m ch_num: %d  receipt: %d\033[0m\n", friendnumber, receipt);
    mq_confirm( (int) friendnumber, (int) receipt);
    receipt_display( m, friendnumber, receipt);
    return;
}
//
static void cb_friend_request(Tox *m, const uint8_t *public_key, const uint8_t *data, size_t length,
                              void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) data;
    (void) length;
    (void) userdata;
    if (public_key_is_blocked((char *) public_key)) {
        return;
    }

    TOX_ERR_FRIEND_ADD err;
    tox_friend_add_norequest(m, public_key, &err);

    if (err != TOX_ERR_FRIEND_ADD_OK) {
        log_error_timestamp(err, "tox_friend_add_norequest failed");
    } else {
        log_timestamp("Accepted friend request");
    }
    
    save_data(m, DATA_FILE);

    // add friend message to to data list: relay_name_list[MAX_Friend_NUM];
    get_relay_name_list( m );

    return;
}
//
////////////////////////////////
//  
//
int lookup_ch_num( char *public_key)
{  
    int retval = -1;
    for (int i = 0; i < MAX_Friend_NUM ; i++ ) {
        if( strstr( public_key, relay_name_list[i].public_key_str) != NULL ) 
        {
            retval = i;
            break;
        }
    };

    return retval;
}
//
//  /relay ERETRT#%#%#%%$$$$H
void set_relayer_public_key( char *user_input_msg)
{
    lineindex = 0;
    get_token(user_input_msg);
    get_token(user_input_msg);

    ini_puts( "metaCom_Relayer","relay_public_key", token, config_file);

    relay_number = lookup_ch_num( token);
    console_out(" >> set relay_public_key...ok\n");

    return;
}
//----------------------------------------------------------------
/* ---------------------------------------------------------------------------
 * Store-and-forward reliability layer
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Relay environment
 *
 * The routing and store-and-forward policy lives in relay.c, which has no
 * toxcore dependency. This file supplies the toxcore facing half through the
 * callbacks below: the peer directory, the message database, the delivery
 * queue and the INI mirror.
 * ------------------------------------------------------------------------- */

static relay_env g_relay;

static int relay_resolve_peer(void *ctx, const char *public_key_hex)
{
    (void) ctx;
    return lookup_ch_num((char *) public_key_hex);
}

static const char *relay_peer_key(void *ctx, int channel)
{
    (void) ctx;

    if (!friend_number_valid((uint32_t) channel)) {
        return NULL;
    }

    return relay_name_list[channel].public_key_str;
}

static bool relay_peer_is_metacom(void *ctx, int channel)
{
    (void) ctx;

    return friend_number_valid((uint32_t) channel)
               && relay_name_list[channel].meta_chat_flag;
}

static sn_tracker *relay_tracker(void *ctx, int channel)
{
    (void) ctx;

    return friend_number_valid((uint32_t) channel) ? &recv_tracker[channel] : NULL;
}

/* A channel exists once get_relay_name_list() has recorded a public key for it.
 * Without this filter the start-up recovery would run one database query for
 * every one of the MAX_Friend_NUM channels, the great majority of which have no
 * contact and never will. */
static bool relay_channel_known(void *ctx, int channel)
{
    (void) ctx;

    return friend_number_valid((uint32_t) channel)
               && relay_name_list[channel].public_key_str[0] != '\0';
}

static int relay_store(void *ctx, int channel, int sn, long timestamp,
                       const char *sender, const char *body)
{
    (void) ctx;
    return msg_database_add(channel, sn, (time_t) timestamp, sender, "incoming", body);
}

static int relay_enqueue(void *ctx, int channel, const char *payload,
                         size_t len, time_t now)
{
    (void) ctx;
    return (mq_enqueue(channel, payload, len, now) == MQ_OK) ? 0 : -1;
}

static int relay_fetch_range(void *ctx, int channel, int from, int to,
                             relay_record *out, int max)
{
    (void) ctx;

    struct CHAT_MSG_RECORD records[RELAY_MAX_REPLAY];
    const int n = msg_database_range(channel, "outgoing", from, to, records,
                                     (max < RELAY_MAX_REPLAY) ? max : RELAY_MAX_REPLAY);

    if (n <= 0) {
        return n;
    }

    for (int i = 0; i < n; ++i) {
        out[i].sn = records[i].msg_sn;
        out[i].timestamp = (long) records[i].datetime;
        copy_tox_str(out[i].body, sizeof(out[i].body), records[i].message,
                     (uint16_t) strlen(records[i].message));
    }

    return n;
}

static void relay_remember_serial(void *ctx, int channel, int sn)
{
    (void) ctx;

    /* The database is authoritative; this mirrors the value into the INI file
     * for compatibility with builds that read it from there. */
    char key_buffer[64];
    char sn_buffer[16];
    snprintf(key_buffer, sizeof(key_buffer), "recev%03d", channel);
    snprintf(sn_buffer, sizeof(sn_buffer), "%04d", sn);
    ini_puts("incoming_sn", key_buffer, sn_buffer, config_file);
}

static void relay_init(void)
{
    memset(&g_relay, 0, sizeof(g_relay));

    g_relay.self_public_key = self_public_key_str;
    g_relay.resolve_peer = relay_resolve_peer;
    g_relay.peer_key = relay_peer_key;
    g_relay.peer_is_metacom = relay_peer_is_metacom;
    g_relay.tracker = relay_tracker;
    g_relay.channel_known = relay_channel_known;
    g_relay.store = relay_store;
    g_relay.enqueue = relay_enqueue;
    g_relay.fetch_range = relay_fetch_range;
    g_relay.remember_serial = relay_remember_serial;
    g_relay.ctx = NULL;
}

/* Adapter for relay_recover_trackers(): highest stored incoming serial. */
static int relay_last_stored_sn(void *ctx, int channel, int *out)
{
    (void) ctx;
    return (msg_database_last_sn(channel, "incoming", out) == MSG_DB_OK) ? 0 : -1;
}

/* Report what the relay did with a record. */
static void relay_log_outcome(relay_outcome outcome, uint32_t channel)
{
    switch (outcome) {
    case RELAY_DUPLICATE:
        log_timestamp("Duplicate telegram from channel %u ignored", channel);
        break;

    case RELAY_GAP_REQUESTED:
        log_timestamp("Requested a replay from channel %u", channel);
        break;

    case RELAY_RECOVERED:
        log_timestamp("Recovered a previously missing telegram on channel %u", channel);
        break;

    case RELAY_RESYNCED:
        log_timestamp("Sequence gap on channel %u exceeded %d; resynchronised",
                      channel, RELAY_MAX_GAP);
        break;

    case RELAY_RELAYED:
        log_timestamp("Relayed a telegram from channel %u", channel);
        break;

    case RELAY_NO_ROUTE:
        log_timestamp("No relay route for a telegram from channel %u", channel);
        break;

    case RELAY_SERVED:
        log_timestamp("Answered a replay request from channel %u", channel);
        break;

    case RELAY_NOTHING_STORED:
        log_timestamp("Replay request from channel %u had nothing stored", channel);
        break;

    case RELAY_DISCARDED:
        log_error_timestamp(-1, "Discarded a malformed record from channel %u", channel);
        break;

    case RELAY_STORED:
    case RELAY_NO_PEER:
    default:
        break;
    }
}
//
//  handle incoming message from outside
//
extern char token[];
extern int lineindex;
//
static void cb_friend_message(Tox *m, uint32_t friendnumber, TOX_MESSAGE_TYPE type, 
                    const uint8_t *string,size_t length, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) userdata;

    char public_key[TOX_PUBLIC_KEY_SIZE];
    char outmsg[256];
    char key_buffer[128];

    if (type != TOX_MESSAGE_TYPE_NORMAL) {
        return;
    }

    /* Every per-friend array below is indexed by friendnumber. Reject values
     * outside the valid range instead of indexing out of bounds. */
    if (!friend_number_valid(friendnumber)) {
        log_error_timestamp(-1, "cb_friend_message: friend number %u out of range", friendnumber);
        return;
    }

    //  char public_key[TOX_PUBLIC_KEY_SIZE];
    //  get the incoming_msg sender public_key, if it exists, we are friends.
    //
    if (tox_friend_get_public_key(m, friendnumber, (uint8_t *) public_key, NULL) == 0) {
        return;
    }

    // delete block name-list, delete the id we dislike.
    //
    if (public_key_is_blocked(public_key)) {
        tox_friend_delete(m, friendnumber, NULL);
        return;
    }

    // switch chat channal we are talking about......
    pre_friendnumber = friendnumber;    // for message reply ...... 

    // copy message from outside world!
    

    char message[TOX_MAX_MESSAGE_LENGTH];
    length = copy_tox_str(message, TOX_MAX_MESSAGE_LENGTH, (const char *) string, length);
    message[length] = '\0';

    // relash last tx-msg ........
    chat_tx_Control[pre_friendnumber].last_msg_tx = get_time();

    //
    // process incoming message  ..... 
    if( message[0] == '/' )     // command mode case
    {
        // internal command case process: only for network connect
        if( strstr(message, "/HELLO") != NULL && strstr(message, "METACOM") != NULL) {
            // we are all meta_man........set signe_flag.
            relay_name_list[friendnumber].meta_chat_flag = true;
            // keep in config file
            snprintf(key_buffer, sizeof(key_buffer), "meta%03d", friendnumber);
            ini_puts( "meta_chat_flag", key_buffer, "1", config_file);
            
            return;
        };

        if (length && execute(m, friendnumber, message, length) == -1) 
        {
            snprintf(outmsg, sizeof(outmsg), "%s",
                     ">> Invalid command to server. Type help for a list of commands");
            tox_friend_send_message(m, friendnumber, TOX_MESSAGE_TYPE_NORMAL, (uint8_t *) outmsg, strlen(outmsg), NULL);
            //console_out("+++ %s\n", message);
        }
    }
    else // non-command mode case
    {
        friend_name_size = tox_friend_get_name_size(m, pre_friendnumber, NULL);
        tox_friend_get_name(m, pre_friendnumber, (uint8_t*)tx_name,NULL);
        tx_name[friend_name_size] = 0;

        //console_out("$from [%s] msg :\n %s\n",tx_name,message);    // display msg to screen
        console_out(	"\033[36m>$ msg incoming from [ %s ]: %s\n\033[0m\n", tx_name, message);

        //  message text body to be transfer ................
        //  
        //      ::normal text format
        //      ZCZC TEXT
        //      receive_pubkey 
        //      sender_pubkey 
        //      send_sn time_stamp
        //      text ... ... ... ... ...
        //      NNNN
        //
        //  >> control command
        //      ZCZC CMD
        //      S:xxxx  
        //      R:xxxx
        //      NNNN
        //
        //      beep();  

        // maybe normal tox friend mess, not METACOM class!
        if( strstr( message, "ZCZC" ) == NULL ) {
            console_out( "mess-parser: direct post msg, no metacom class: ZCZC\n");

            // normal text message keep in database ...
            time_t datetime = get_unix_time();
            char sender[128];
            strcpy( sender, relay_name_list[friendnumber].public_key_str);
            msg_database_add(friendnumber, 0, datetime, sender, "incoming", message);  
                                        
            return;
        }

        /* Decode and act through the relay policy module. It is the verified
         * implementation of "is this ours, is it a duplicate, is there a gap,
         * must it be forwarded", and it reports what it did. */
        const relay_outcome outcome = relay_receive(&g_relay, (int) friendnumber,
                                                    message, strlen(message),
                                                    get_unix_time());
        relay_log_outcome(outcome, friendnumber);
    };
    
    return;
}
//
//
void receipt_display( Tox *m, uint32_t friendnumber, uint32_t receipt)
{
    /* Required by the callback signature; unused here. */
    (void) friendnumber;
    (void) m;
    char out_buffer[256] = " +++ ";

/*
    int friend_size = tox_friend_get_name_size(m,friendnumber,NULL);
    tox_friend_get_name(m,friendnumber,(uint8_t*)friend_name,NULL);
    friend_name[friend_size] = 0;
	*/

    //strftime( buffer, 64, "%H:%M", get_time()); ???

    snprintf(out_buffer, sizeof(out_buffer), "%s", "  ---  ~ read your message.");
    if( msg_keeper.receipt == receipt ) {
        console_out("%s", out_buffer);
        msg_keeper.receipt = 0;
    }

    return;
}
//----------------------------------------------------------------
//
//
static void cb_group_invite(Tox *m, uint32_t friendnumber, TOX_CONFERENCE_TYPE type,
                            const uint8_t *cookie, size_t length, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) userdata;
    /*
    if (!friend_is_master(m, friendnumber)) {
        return;
    }
    */

    char name[TOX_MAX_NAME_LENGTH];
    tox_friend_get_name(m, friendnumber, (uint8_t *) name, NULL);
    size_t len = tox_friend_get_name_size(m, friendnumber, NULL);
    name[len] = '\0';

    int groupnum = -1;

    if (type == TOX_CONFERENCE_TYPE_TEXT) {
        TOX_ERR_CONFERENCE_JOIN err;
        groupnum = tox_conference_join(m, friendnumber, cookie, length, &err);

        if (err != TOX_ERR_CONFERENCE_JOIN_OK) {
            console_out("Conference  join error: %d\n", err);
            goto on_error;
        };
        console_out("Conference join room number : %d\n", groupnum );
    } 
    else 
    if (type == TOX_CONFERENCE_TYPE_AV) {
        groupnum = toxav_join_av_groupchat(m, friendnumber, cookie, length, NULL, NULL);

        if (groupnum == -1) {
            console_out("Conference  join error: %d\n", groupnum);
            goto on_error;
        };
         console_out("Conference join : %d\n", groupnum );
    }

    if (group_add(groupnum, type, NULL) == -1) {
        log_error_timestamp(-1, "Invite from %s failed (group_add failed)", name);
        tox_conference_delete(m, groupnum, NULL);
        return;
    }

    /* 

    console_out(	"\033[32m $from smaple+++++++ \033[0m\n");    // GREEN
    console_out(	"\033[36m $from 36 smaple+++++++ \033[0m\n");  // CYAN
    */
    log_timestamp("\033[32m Accepted groupchat invite from %s Confernce Room Num[%d]\033[0m", name, groupnum);
    return;

on_error:
    log_error_timestamp(-1, "Invite from %s failed (core failure)", name);

    return;
}
//----------------------------------------------------------------
//
static void cb_group_titlechange(Tox *m, uint32_t groupnumber, uint32_t peernumber, const uint8_t *title,
                                 size_t length, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) m;
    (void) peernumber;
    (void) userdata;
    char message[TOX_MAX_MESSAGE_LENGTH];
    length = copy_tox_str(message, sizeof(message), (const char *) title, length);

    int idx = group_index(groupnumber);

    if (idx == -1) {
        return;
    }

    /* g_chats[].title is TOX_MAX_NAME_LENGTH bytes, so clamp even though the
     * library is expected to have bounded the title it delivered. */
    const size_t copy_len = MIN(length, sizeof(Tox_Bot.g_chats[idx].title) - 1);

    memcpy(Tox_Bot.g_chats[idx].title, message, copy_len);
    Tox_Bot.g_chats[idx].title[copy_len] = '\0';
    Tox_Bot.g_chats[idx].title_len = (int) copy_len;

    return ;
}
/* END CALLBACKS */
//----------------------------------------------------------------
//
//  Keep mess data.......
int save_data(Tox *m, const char *path)
{
    if (path == NULL) {
        log_error_timestamp(-1, "Warning: save_data failed (no path)");
        return -1;
    }

    FILE *fp = fopen(path, "wb");

    if (fp == NULL) {
        console_err("Couldn't open file %s\n", path);
        return -1;
    }

    const size_t data_len = tox_get_savedata_size(m);
    uint8_t *data = malloc(data_len);

    if (data == NULL) {
        /* The stream must be released on every path; the previous version
         * jumped to its error label here and leaked the descriptor. */
        (void) fclose(fp);
        log_error_timestamp(-1, "Warning: save_data failed (out of memory)");
        return -1;
    }

    tox_get_savedata(m, data);

    /* Count bytes rather than records so a partial write is detected too. */
    const size_t written = fwrite(data, 1, data_len, fp);

    free(data);

    const int close_rc = fclose(fp);

    if (written != data_len || close_rc != 0) {
        log_error_timestamp(-1, "Warning: save_data failed (short write)");
        return -1;
    }

    return 0;
}
//----------------------------------------------------------------
//
static Tox *load_tox(struct Tox_Options *options, char *path)
{
    FILE *fp = fopen(path, "rb");
    Tox *m = NULL;

    if (fp == NULL) {
        TOX_ERR_NEW err;
        m = tox_new(options, &err);

        if (err != TOX_ERR_NEW_OK) {
            console_err("tox_new failed with error %d\n", err);
            return NULL;
        }

        save_data(m, path);
        return m;
    }

    const off_t data_len = file_size(path);

    if (data_len <= 0) {
        console_err("tox_new failed: toxrelayer save file is empty\n");
        (void) fclose(fp);
        return NULL;
    }

    /* Heap allocated rather than a variable length array: the size comes from a
     * file on disk and must not be trusted to fit on the stack. */
    uint8_t *data = malloc((size_t) data_len);

    if (data == NULL) {
        (void) fclose(fp);
        return NULL;
    }

    if (fread(data, (size_t) data_len, 1, fp) != 1) {
        free(data);
        (void) fclose(fp);
        return NULL;
    }

    TOX_ERR_NEW err;
    options->savedata_type = TOX_SAVEDATA_TYPE_TOX_SAVE;
    options->savedata_data = data;
    options->savedata_length = (size_t) data_len;

    m = tox_new(options, &err);

    /* tox_new takes its own copy of the savedata, so the buffer is released and
     * the stream is closed on every path. The error path previously returned
     * without closing the file, leaking the stream descriptor. */
    free(data);
    (void) fclose(fp);

    if (err != TOX_ERR_NEW_OK) {
        console_err("tox_new failed with error %d\n", err);
        return NULL;
    }

    return m;
}
//
static void load_conferences(Tox *m)
{
    size_t num_chats = tox_conference_get_chatlist_size(m);

    if (num_chats == 0) {
        return;
    }

    uint32_t *chatlist = malloc(num_chats * sizeof(uint32_t));

    if (chatlist == NULL) {
        console_err("malloc() failed in load_conferences()\n");
        return;
    }

    tox_conference_get_chatlist(m, chatlist);

    for (size_t i = 0; i < num_chats; ++i) {
        uint32_t groupnumber = chatlist[i];

        Tox_Err_Conference_Get_Type type_err;
        Tox_Conference_Type type = tox_conference_get_type(m, groupnumber, &type_err);

        if (type_err != TOX_ERR_CONFERENCE_GET_TYPE_OK) {
            tox_conference_delete(m, groupnumber, NULL);
            continue;
        }

        if (group_add(groupnumber, type, NULL) != 0) {
            console_err("Failed to autoload group %d\n", groupnumber);
            tox_conference_delete(m, groupnumber, NULL);
            continue;
        }
    }

    free(chatlist);
}
//
// arkMeta 4 …… usage
static void print_usage(void) 
{
    console_out("usage: toxrelayer [OPTION] ...\n");
    console_out("    -4, --ipv4              Force IPv4\n");
    console_out("    -h, --help              Show this message and exit\n");
    console_out("    -L, --no-lan            Disable LAN\n");
    console_out("    -P, --HTTP-proxy        Use HTTP proxy. Requires: [IP] [port]\n");
    console_out("    -p, --SOCKS5-proxy      Use SOCKS proxy. Requires: [IP] [port]\n");
    console_out("    -t, --force-tcp         Force connections through TCP relays (DHT disabled)\n");
}
//
static void set_default_options(void) {
    Options = (struct Options) {
        0
    };

    /* set any non-zero defaults here*/
    Options.proxy_type = TOX_PROXY_TYPE_NONE;
}
//
//  node start check
static void parse_args(int argc, char *argv[]) 
{
    set_default_options();

    static struct option long_opts[] = {
        {"ipv4", no_argument, 0, '4'},
        {"help", no_argument, 0, 'h'},
        {"no-lan", no_argument, 0, 'L'},
        {"SOCKS5-proxy", required_argument, 0, 'p'},
        {"HTTP-proxy", required_argument, 0, 'P'},
        {"force-tcp", no_argument, 0, 't'},
        {NULL, no_argument, NULL, 0},
    };

    const char *options_string = "4hLtp:P:";
    int opt = 0;
    int indexptr = 0;

    while ((opt = getopt_long(argc, argv, options_string, long_opts, &indexptr)) != -1) {
        switch (opt) {
            case '4': {
                Options.force_ipv4 = true;
                console_out("Option set: Forcing IPV4\n");
                break;
            }

            case 'L': {
                Options.disable_lan = true;
                console_out("Option set: LAN disabled\n");
                break;
            }

            case 'p': {
                Options.proxy_type = TOX_PROXY_TYPE_SOCKS5;
            }

            // Intentional fallthrough
            case 'P': {
                if (optarg == NULL) {
                    console_err("Invalid argument for option: %d", opt);
                    Options.proxy_type = TOX_PROXY_TYPE_NONE;
                    break;
                }

                if (Options.proxy_type != TOX_PROXY_TYPE_SOCKS5) {
                    Options.proxy_type = TOX_PROXY_TYPE_HTTP;
                }

                snprintf(Options.proxy_host, sizeof(Options.proxy_host), "%s", optarg);

                ++optind;

                if (optind > argc || argv[optind - 1][0] == '-') {
                    console_err("Error setting proxy\n");
                    exit(EXIT_FAILURE);
                }

                long parsed_port = 0;

                if (!parse_int_range(argv[optind - 1], 1, MAX_PORT_RANGE, &parsed_port)) {
                    console_err("Invalid port given for proxy\n");
                    exit(EXIT_FAILURE);
                }

                const long int port = parsed_port;

                Options.proxy_port = (uint16_t) port;

                const char *proxy_str = Options.proxy_type == TOX_PROXY_TYPE_SOCKS5 ? "SOCKS5" : "HTTP";

                console_out("Option set: %s proxy %s:%ld\n", proxy_str, optarg, port);
            }

            // Intentional fallthrough
            // we always want UDP disabled if proxy is set
            // don't change order, as -t must come after -P or -p

            case 't': {
                Options.disable_udp = true;
                console_out("Option set: UDP/DHT disabled\n");
                break;
            }

            case 'h':

            // Intentional fallthrough

            default: {
                print_usage();
                exit(EXIT_SUCCESS);
            }
        }
    }
}
//----------------------------------------------------------------
//
static void init_tox_options(struct Tox_Options *tox_opts) 
{
    tox_options_default(tox_opts);

    tox_options_set_ipv6_enabled(tox_opts, !Options.force_ipv4);
    tox_options_set_udp_enabled(tox_opts, !Options.disable_udp);
    tox_options_set_proxy_type(tox_opts, Options.proxy_type);
    tox_options_set_local_discovery_enabled(tox_opts, !Options.disable_lan);

    if (Options.proxy_type != TOX_PROXY_TYPE_NONE) {
        tox_options_set_proxy_port(tox_opts, Options.proxy_port);
        tox_options_set_proxy_host(tox_opts, Options.proxy_host);
    }

    return;
}
//----------------------------------------------------------------
//
static Tox *init_tox(void) 
{
    Tox_Err_Options_New err;
    struct Tox_Options *tox_opts = tox_options_new(&err);
    
    if (!tox_opts || err != TOX_ERR_OPTIONS_NEW_OK) {
        console_err("Failed to initialize tox options: error %d\n", err);
        exit(EXIT_FAILURE);
    }

    init_tox_options(tox_opts);

    Tox *m = load_tox(tox_opts, DATA_FILE);

    tox_options_free(tox_opts);

    if (!m) {
        return NULL;
    }

    // command function biding ......
    tox_callback_self_connection_status(m, cb_self_connection_change);
    tox_callback_friend_connection_status(m, cb_friend_connection_change);
    tox_callback_friend_request(m, cb_friend_request);
    tox_callback_friend_name(m, on_friend_name);
    tox_callback_friend_read_receipt(m, cb_friend_read_receipt);

    // incoming message......
    tox_callback_friend_message(m, cb_friend_message);  
    //----------------------------------------------------------------        
    tox_callback_conference_invite(m, cb_group_invite);
    tox_callback_conference_title(m, cb_group_titlechange);
    tox_callback_conference_message(m, cb_conference_message);
    tox_callback_conference_peer_list_changed(m, on_conference_peer_list_changed);
    
    //----------------------------------------------------------------
    /*
    tox_callback_conference_peer_list_changed(m, on_conference_peer_list_changed);
    tox_callback_conference_peer_name(m, on_conference_peer_name);

    tox_callback_file_recv(m, on_file_recv);
    tox_callback_file_chunk_request(m, on_file_chunk_request);
    tox_callback_file_recv_control(m, on_file_recv_control);
    tox_callback_file_recv_chunk(m, on_file_recv_chunk);
    tox_callback_friend_lossless_packet(m, on_lossless_custom_packet);
    */
    //-------------------------------

    size_t s_len = tox_self_get_status_message_size(m);

    /*
    tox_callback_self_connection_status(m, on_self_connection_status);
    tox_callback_friend_connection_status(m, on_friend_connection_status);
    tox_callback_friend_typing(m, on_friend_typing);
    tox_callback_friend_request(m, on_friend_request);
    tox_callback_friend_message(m, on_friend_message);
    tox_callback_friend_name(m, on_friend_name);
    tox_callback_friend_status(m, on_friend_status);
    tox_callback_friend_status_message(m, on_friend_status_message);
    tox_callback_friend_read_receipt(m, on_friend_read_receipt);
    tox_callback_conference_invite(m, on_conference_invite);
    tox_callback_conference_message(m, on_conference_message);
    tox_callback_conference_peer_list_changed(m, on_conference_peer_list_changed);
    tox_callback_conference_peer_name(m, on_conference_peer_name);
    tox_callback_conference_title(m, on_conference_title);
    tox_callback_file_recv(m, on_file_recv);
    tox_callback_file_chunk_request(m, on_file_chunk_request);
    tox_callback_file_recv_control(m, on_file_recv_control);
    tox_callback_file_recv_chunk(m, on_file_recv_chunk);
    tox_callback_friend_lossless_packet(m, on_lossless_custom_packet);
    */

    // Node Status Message Display ......
    if (s_len == 0) {
        const char *statusmsg = "arkMeta World: Command '/help' for more info";
        tox_self_set_status_message(m, (uint8_t *) statusmsg, strlen(statusmsg), NULL);
    }

    size_t n_len = tox_self_get_name_size(m);
    
    if (n_len == 0) {
        tox_self_set_name(m, (uint8_t *)Bot_Name, strlen(Bot_Name), NULL);
    }

    return m;
}

/* 
    Store Tox profile data to path.
 *
 *  Return 0 if stored successfully.
 *  Return -1 on error.
 */
#define TEMP_PROFILE_EXT ".tmp"
int store_data(Tox *m, const char *path)
{
    if (path == NULL) {
        return -1;
    }

    size_t temp_buf_size = strlen(path) + strlen(TEMP_PROFILE_EXT) + 1;
    char *temp_path = malloc(temp_buf_size);

    if (temp_path == NULL) {
        return -1;
    }

    snprintf(temp_path, temp_buf_size, "%s%s", path, TEMP_PROFILE_EXT);

    FILE *fp = fopen(temp_path, "wb");

    if (fp == NULL) {
        free(temp_path);
        return -1;
    }

    size_t data_len = tox_get_savedata_size(m);
    char *data = malloc(data_len * sizeof(char));

    if (data == NULL) {
        free(temp_path);
        (void) fclose(fp);
        return -1;
    }

    tox_get_savedata(m, (uint8_t *) data);

    if (user_password.data_is_encrypted && !arg_opts.unencrypt_data) {
        size_t enc_len = data_len + TOX_PASS_ENCRYPTION_EXTRA_LENGTH;
        char *enc_data = malloc(enc_len * sizeof(char));

        if (enc_data == NULL) {
            (void) fclose(fp);
            free(temp_path);
            free(data);
            return -1;
        }

        Tox_Err_Encryption err;
        tox_pass_encrypt((uint8_t *) data, data_len, (uint8_t *) user_password.pass, user_password.len,
                         (uint8_t *) enc_data, &err);

        if (err != TOX_ERR_ENCRYPTION_OK) {
            console_err("tox_pass_encrypt() failed with error %d\n", err);
            (void) fclose(fp);
            free(temp_path);
            free(data);
            free(enc_data);
            return -1;
        }

        if (fwrite(enc_data, enc_len, 1, fp) != 1) {
            console_err("Failed to write profile data.\n");
            (void) fclose(fp);
            free(temp_path);
            free(data);
            free(enc_data);
            return -1;
        }

        free(enc_data);
    } else {  /* data will not be encrypted */
        if (fwrite(data, data_len, 1, fp) != 1) {
            console_err("Failed to write profile data.\n");
            (void) fclose(fp);
            free(temp_path);
            free(data);
            return -1;
        }
    }

    (void) fclose(fp);
    free(data);

    if (rename(temp_path, path) != 0) {
        free(temp_path);
        return -1;
    }

    free(temp_path);

    return 0;
}
//-------------------------------------------------------------------------------------
//  when a peer changes their name.
//
void on_conference_peer_name(Tox *m, uint32_t conferencenumber, uint32_t peernumber, const uint8_t *name,
                             size_t length, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) peernumber;
    (void) userdata;
    /* Required by the callback signature; unused here. */
    (void) conferencenumber;
    (void) m;
    // UNUSED_VAR(userdata);

    char nick[TOXIC_MAX_NAME_LENGTH + 1];
    length = copy_tox_str(nick, sizeof(nick), (const char *) name, length);
    filter_str(nick, length);

    console_out("conference_peer_name: %s\n", nick);

}
//-----------------------------------------------------------------------------
//  conference_peer join or left, then the status is changed.
//
void on_conference_peer_list_changed(Tox *m, uint32_t conferencenumber, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) userdata;
    // UNUSED_VAR(userdata);
    // console_out("conference_peer join or left\n");
    conference_peer_join_left( m, conferencenumber );

    return;
}
//-----------------------------------------------------------------------------
//  when friend name change
//
void on_friend_name(Tox *m, uint32_t friendnumber, const uint8_t *string, size_t length, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) friendnumber;
    (void) userdata;
    // UNUSED_VAR(userdata);
    char nick[TOXIC_MAX_NAME_LENGTH + 1];

    length = copy_tox_str(nick, sizeof(nick), (const char *) string, length);
    filter_str(nick, length);
    nick[length] = '\0';

    // console_out("new friend_name: %s\n", nick);

    store_data(m, DATA_FILE);
}
//----------------------------------------------------------------
//
//  conferencenumber + peernumber + message +
//  type = TOX_MESSAGE_TYPE_NORMAL ? ACTION_TYPE
//
//  > chat_msg keep in database: 
//       conferencenumber + peernumber + message + serial_number_publickey
//  each peer keep the same database and sync_data when join chat_room again.
//
//  > sync_data:
//      msg_serial_number(1..999) checking with relayer_server or room_peer.
//
//  > sync_tele_command_type:
//      ZCZC METACOM ................NNNN
//
//  > each client can join a max_room_num = 256, send_msg with serial_num/timestamp.
//    chat_room with many peer friends, and the send_msg serial_num/timestamp must be remembered.
//
//  
static void cb_conference_message(Tox *m, uint32_t conferencenumber, uint32_t peernumber, Tox_Message_Type type,
                           const uint8_t *message, size_t length, void *userdata)
{
    /* Required by the callback signature; unused here. */
    (void) type;
    (void) userdata;
    // UNUSED_VAR(userdata);

    char nick_name[128];

    /* The message text, bounded by the sender. A separate variable holds its
     * length: reusing `length` for the title size previously truncated the
     * displayed message to however long the room title happened to be. */
    char msg[MAX_STR_SIZE + 1];
    const uint16_t msg_len = copy_tox_str(msg, sizeof(msg), (const char *) message, (uint16_t) length);
    msg[msg_len] = '\0';

    Tox_Err_Conference_Title t_err;
    uint8_t title[MAX_STR_SIZE];
    size_t title_len = tox_conference_get_title_size(m, conferencenumber, &t_err);

    if (t_err != TOX_ERR_CONFERENCE_TITLE_OK || title_len >= sizeof(title)) {
        title_len = 0;
    }

    tox_conference_get_title(m, conferencenumber, title, &t_err);
    title[title_len] = '\0';

    log_timestamp("\033[36m %s\033[0m", title);

    get_conference_nick_truncate(m, nick_name, peernumber, conferencenumber);

    // room msg display .......
    console_out( "\033[36m >> Room%02d.%s: %s\033[0m\n", conferencenumber, nick_name, msg);

    return;
}

/* 
    TODO: hardcoding is bad stop being lazy      
*/
static struct toxNodes {
    const char *ip;
    uint16_t    port;
    const char *key;
} nodes[] = {
    { "95.79.50.56",    33445, "8E7D0B859922EF569298B4D261A8CCB5FEA14FB91ED412A7603A585A25698832" },
    { "85.143.221.42",  33445, "DA4E4ED4B697F2E9B000EEFE3A34B554ACD3F45F5C96EAEA2516DD7FF9AF7B43" },
    { "46.229.52.198",  33445, "813C8F4187833EF0655B10F7752141A352248462A567529A38B6BBF73E979307" },
    { "144.217.167.73", 33445, "7E5668E0EE09E19F320AD47902419331FFEE147BB3606769CFBE921A2A2FD34C" },
    { "198.199.98.108", 33445, "BEF0CFB37AF874BD17B9A8F9FE64C75521DB95A37D33C5BDB00E9CF58659C04F" },
    { "81.169.136.229", 33445, "E0DB78116AC6500398DDBA2AEEF3220BB116384CAB714C5D1FCD61EA2B69D75E" },
    { "205.185.115.131", 53,   "3091C6BEB2A993F1C6300C16549FABA67098FF3D62C6D253828B531470B53D68" },
    { "46.101.197.175", 33445, "CD133B521159541FB1D326DE9850F5E56A6C724B5B8E5EB5CD8D950408E95707" },
    { "195.201.7.101",  33445, "B84E865125B4EC4C368CD047C72BCE447644A2DC31EF75BD2CDA345BFD310107" },
    { "168.138.203.178",33445, "6D04D8248E553F6F0BFDDB66FBFB03977E3EE54C432D416BC2444986EF02CC17" },
    { "5.19.249.240",   38296, "DA98A4C0CD7473A133E115FEA2EBDAEEA2EF4F79FD69325FC070DA4DE4BA3238" },
    { "209.59.144.175", 33445, "214B7FEA63227CAEC5BCBA87F7ABEEDB1A2FF6D18377DD86BF551B8E094D5F1E" },
    { "188.225.9.167",  33445, "1911341A83E02503AB1FD6561BD64AF3A9D6C3F12B5FBB656976B2E678644A67" },
    { "122.116.39.151", 33445, "5716530A10D362867C8E87EE1CD5362A233BAFBBA4CF47FA73B7CAD368BD5E6E" },
    { "195.123.208.139",33445, "534A589BA7427C631773D13083570F529238211893640C99D1507300F055FE73" },
    { "104.225.141.59", 43334, "933BA20B2E258B4C0D475B6DECE90C7E827FE83EFA9655414E7841251B19A72C" },
    { "137.74.42.224",  33445, "A95177FA018066CF044E811178D26B844CBF7E1E76F140095B3A1807E081A204" },
    { "172.105.109.31", 33445, "D46E97CF995DC1820B92B7D899E152A217D36ABE22730FEA4B6BF1BFC06C617C" },
    { "91.146.66.26",   33445, "B5E7DAC610DBDE55F359C7F8690B294C8E4FCEC4385DE9525DBFA5523EAD9D53" },
    { NULL, 0, NULL },
};
//
//
static void bootstrap_DHT(Tox *m) 
{
    TOX_ERR_BOOTSTRAP err;

    for (int i = 0; nodes[i].ip; ++i) {
        char *key = hex_string_to_bin(nodes[i].key);
        
        tox_bootstrap(m, nodes[i].ip, nodes[i].port, (uint8_t *) key, &err);

        if (err != TOX_ERR_BOOTSTRAP_OK) {
            console_err("Failed to bootstrap DHT: %s %d (error %d)\n", nodes[i].ip, nodes[i].port, err);
        }

        tox_add_tcp_relay(m, nodes[i].ip, nodes[i].port, (uint8_t *) key, &err);

        if (err != TOX_ERR_BOOTSTRAP_OK) {
            console_err("Failed to add TCP relay: %s %d (error %d)\n", nodes[i].ip, nodes[i].port, err);
        }

        free(key);
    }
}
//
//  Dump out the system mess
static void print_profile_info(Tox *m) 
{
    char buffer[128];
    char d[3];
    char address[TOX_ADDRESS_SIZE];
    char name[TOX_MAX_NAME_LENGTH];

    snprintf(buffer, sizeof(buffer), "%s version %s", Bot_Name, VERSION);
    console_out("%s\n", buffer);

    console_out("SoftwareCore version %d.%d.%d\n", tox_version_major(), tox_version_minor(), tox_version_patch());
    console_out("arkMeta ID:\n");
    
    tox_self_get_address(m, (uint8_t *) address);   // TOX_ADDRESS_SIZE = 76 chars display
    for (size_t i = 0; i < TOX_ADDRESS_SIZE; ++i) {
        snprintf(d, sizeof(d), "%02X", address[i] & 0xff);
        console_out("%s", d);
    }
    console_out("\n");

    size_t len = tox_self_get_name_size(m);
    tox_self_get_name(m, (uint8_t *) name);     // Node Name
    name[len] = '\0';

    size_t numfriends = tox_self_get_friend_list_size(m);
    size_t num_chats = tox_conference_get_chatlist_size(m);

    max_friend_num = numfriends;    // number of friend.

    console_out("Name: %s\n", name);
    console_out("Contacts: %lu\n", numfriends);
    console_out("Active groups: %lu\n", num_chats);

    return;
}
//
static void purge_inactive_friends(Tox *m)
{
    size_t numfriends = tox_self_get_friend_list_size(m);

    if (numfriends == 0) {
        return;
    }

    uint32_t friend_list[numfriends];
    tox_self_get_friend_list(m, friend_list);

    for (size_t i = 0; i < numfriends; ++i) {
        uint32_t friendnum = friend_list[i];

        if (!tox_friend_exists(m, friendnum)) {
            continue;
        }

        TOX_ERR_FRIEND_GET_LAST_ONLINE err;
        uint64_t last_online = tox_friend_get_last_online(m, friendnum, &err);

        if (err != TOX_ERR_FRIEND_GET_LAST_ONLINE_OK) {
            continue;
        }

        if (get_time() - last_online > Tox_Bot.inactive_limit) {
            tox_friend_delete(m, friendnum, NULL);
        }
    }
}
//
static void purge_empty_groups(Tox *m)
{
    /* chats_idx is an int, so the loop counter must be signed too. */
    for (int i = 0; i < Tox_Bot.chats_idx; ++i) {
        if (!Tox_Bot.g_chats[i].active) {
            continue;
        }

        const uint32_t groupnum = Tox_Bot.g_chats[i].groupnum;
        TOX_ERR_CONFERENCE_PEER_QUERY err;
        const uint32_t num_peers = tox_conference_peer_count(m, groupnum, &err);

        if (err != TOX_ERR_CONFERENCE_PEER_QUERY_OK || num_peers <= 1) {
            log_timestamp("Deleting empty group %d", groupnum);
            tox_conference_delete(m, groupnum, NULL);

            /* group_leave() takes a group number, not a slot index. Passing the
             * index here would clear the wrong entry. */
            group_leave(groupnum);

            if (i >= Tox_Bot.chats_idx) {   // group_leave modifies chats_idx
                return;
            }
        }
    }
}

/* 
    Return true if we should attempt to purge empty groups.
 *
 *  Empty groups are purged on an interval, but only if we have a stable connection
 *  to the Tox network.
 */
static bool check_group_purge(time_t last_group_purge, time_t cur_time, TOX_CONNECTION connection_status)
{
    if (!timed_out(last_group_purge, cur_time, GROUP_PURGE_INTERVAL)) {
        return false;
    }

    if (connection_status == TOX_CONNECTION_NONE) {
        return false;
    }

    if (!timed_out(Tox_Bot.last_connected, cur_time, GROUP_PURGE_CONNECT_TIMEOUT)) {
        return false;
    }

    return true;
}

/* Attempts to rename legacy toxrelayer save file to new name
 *
 * Return 0 on successful rename, or if legacy file does not exist.
 * Return -1 if both legacy file and new file exist. If this occurrs the user needs to manually sort
 *   the situation out.
 * Return -2 if file rename operation is unsuccessful.
 */
static int legacy_data_file_rename(void)
{
    if (!file_exists(DATA_FILE_PRE_0_1_1)) {
        return 0;
    }

    if (file_exists(DATA_FILE)) {
        return -1;
    }

    if (rename(DATA_FILE_PRE_0_1_1, DATA_FILE) != 0) {
        return -2;
    }

    console_out("Renaming legacy toxrelayer save file to '%s'\n", DATA_FILE);

    return 0;
}

//
int console_key_available(void) 
{
    static const int STDIN = 0;
    static bool initialized = false;

    if (! initialized) {
        // Use termios to turn off line buffering
        struct termios term;

        tcgetattr(STDIN, &term);
        term.c_lflag &= ~ICANON;
        tcsetattr(STDIN, TCSANOW, &term);
        if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
            /* Not fatal: stdin simply stays line buffered. */
        }
        initialized = true;
    }

    int bytesWaiting;
    ioctl(STDIN, FIONREAD, &bytesWaiting);
    return bytesWaiting;
}
/**
 *       get_token      
 
    char token[128];
    int lineindex = 0;
*/
void get_token( const char *mess )
{
    int16_t i = 0;

    while( lineindex < 127 ){
        if ( mess[lineindex] != ' ' &&
            mess[lineindex] != '\r' &&
            mess[lineindex] != '\n' 
            // &&isascii(mess[lineindex])
            ){
                token[i++] = mess[lineindex];
                lineindex++;
        };

        if ( mess[lineindex] == ' ' ){
            lineindex++;
            if( i > 0  ){
                token[i] = 0;
                return;
            };
        }
        if ( mess[lineindex] == 0 ||  mess[lineindex] == '\r' || mess[lineindex] == '\n'){
            token[i] = 0;
            return;
        }
        // console_out("lineindex = %d ",lineindex );
    }
    
    return;
}
//
//
void local_info_display( Tox *m)
{
    char timestr[64];
    char name[128] = "";
    char status_msg[256] = "";
    size_t i, size = tox_self_get_friend_list_size(m);
    size_t friend_size;

    if (size == 0) {
        return;
    }

    // "\033[36m$from 36 smaple+++++++ \033[0m\n"
    size_t len = tox_self_get_name_size(m);
    tox_self_get_name(m, (uint8_t *) name);     // Node Name
    name[len] = '\0';
    console_out( "\033[36m\n NickName: %s\033[0m\n", name);

    // status message display
    // tox_friend_get_status_message
    len = tox_self_get_status_message_size(m);
    tox_self_get_status_message(m,(uint8_t *) status_msg);
    status_msg[len] = '\0';
    console_out( " Status Message :\n     %s\n", status_msg);

    time_t curtime = get_unix_time();
    get_elapsed_time_str(timestr, sizeof(timestr), curtime - Tox_Bot.start_time);
    console_out( " Uptime: %s\n", timestr);

    uint32_t numfriends = tox_self_get_friend_list_size(m);
    console_out("\033[36m Friends: %d (%d online)\033[0m\n", numfriends, Tox_Bot.num_online_friends);

    uint32_t list[size];
    tox_self_get_friend_list(m, list);

    for (i = 0; i < size; ++i) {
        friend_size = tox_friend_get_name_size(m,list[i],NULL);
        tox_friend_get_name(m,list[i],(uint8_t*)name,NULL);
        name[friend_size] = 0;
        console_out("\033[36m    %03lu :  %s\033[0m\n", i+1, name);
    }

    console_out(" Inactive friends are purged after %"PRIu64" days\n",
             Tox_Bot.inactive_limit / SECONDS_IN_DAY);

    /* List active group chats and number of peers in each */
    size_t num_chats = tox_conference_get_chatlist_size(m);

    if (num_chats == 0) {
        console_out(" No active groupchats\n");
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

            if (idx != -1 && Tox_Bot.g_chats[idx].title_len) {
                title = Tox_Bot.g_chats[idx].title;
            }
            const char *type = 
                tox_conference_get_type(m, groupnum, NULL) == TOX_CONFERENCE_TYPE_AV ? "Audio" : "Text";
            console_out(" Group %d | %s | peers: %d | Title: %s\n", groupnum, type,num_peers, title);
        }
    }
    
    conference_basic_info_display(m);

    return;
}
/*
ToxRelayer Master Commands
leave <n>              : Leaves groupchat n
master <id>            : Adds Tox ID to the masterkeys file
passwd <n> <pass>      : Sets password for groupchat n (leave pass blank for no password)
purge <n>              : Sets the number of days before an inactive friend is deleted
status <s>             : Sets status (online, busy or away)
statusmessage <msg>    : Sets status message
title <n> <msg>        : Sets title for groupchat n
*/
//
//  /help  local user command 
//
void evaluate_input( Tox *tox,char *user_input ) 
{
    if( strstr(user_input,"/help") != NULL ) {
        console_out("\n IOT-Web3 DID Node Commands \n");
        console_out(" -------------------------------------------------------------- \n");
        console_out(" /help          - Prints this message\n");
        console_out(" /id            - Print the node ID\n");
        console_out(" /info          - Display node status\n");
        console_out(" /cur           - Display current user info\n");
        console_out(" /set new_name  - Set a new node name\n");
        console_out(" /#num          - Chat with new name_num");
        console_out(" /add <web3-id> - Adds a new friend\n");
        console_out(" /default <n>   - Sets default groupchat room to n\n");
        console_out(" /gmess <n> <msg>  - Sends msg to groupchat n\n");
        console_out(" /title <n> <msg>  - Sets title for groupchat n\n");
        console_out(" /invite           - Request invite to default group chat\n");
        console_out(" /invite <n> <p>   - Invite to group chat n (with password p)\n");
        console_out(" /relay <pub_key>  - Set new mataRelayer public_key.\n");
        console_out(" Enter your command: ?\n\n");

    }else
    if( strstr(user_input,"/id") != NULL ) {
            char* id = (char*) malloc(TOX_ADDRESS_SIZE * 2 * sizeof(char) + 1);
            char address[TOX_ADDRESS_SIZE];
            tox_self_get_address(tox, (uint8_t*) address);

            for (size_t i = 0; i < TOX_ADDRESS_SIZE; ++i) {
                char a[3];
                snprintf(a, sizeof(a), "%02X", address[i] & 0xff);
                strcat(id, a);
            }
            console_out(" web3-id: \n %s\n\n",id);
            local_node_info_display( tox );
        ;
    }else
    // switch channel ...
    if(strstr(user_input,"/#") != NULL ) {
        char temp[16] = "";
        int i = 0;
        int len = strlen(user_input);

        for( i = 0; i < len-2; i++) {
            if (i >= (int) sizeof(temp) - 1) {
                break;      /* never write past the buffer */
            }
            temp[i] = user_input[i+2];
        }
        temp[i] = 0;

            long parsed_index = 0;

            /* A channel is a friend number, so it must be one. atoi() turned
             * "/#abc" into channel 0, silently starting a chat with the wrong
             * peer. */
            if (!parse_int_range(temp, 0, 1023, &parsed_index)) {
                console_out( " > Invalid command! /#2");
                return;
            }

            const int index = (int) parsed_index;

            char name[TOX_MAX_NAME_LENGTH];
            

            if( index < 1024 ){
                pre_friendnumber = index;
                
                tox_friend_get_name(tox,pre_friendnumber,(uint8_t *) name,NULL);
                size_t len = tox_friend_get_name_size(tox,pre_friendnumber,NULL);
                name[len] = '\0';
                console_out( " >> Start chating with: %s\n", name);

                //----------------------------------------------------------------
                //  list chat_msg of friend display, for p2p chatroom
                //
                struct CHAT_MSG recent[MAX_MSG_TOP];
                int recent_count = msg_database_top(recent, MAX_MSG_TOP);

                if (recent_count > 0) {
                    for (int i = 0; i < recent_count; ++i) {
                        console_out(" >> %s : %s\n", recent[i].sender, recent[i].message);
                    }
                } else if (recent_count == 0) {
                    console_out("No stored messages.\n");
                } else {
                    console_out("top_msg error: database unavailable (%d).\n", recent_count);
                }

                return;
            }
        //local_info_display(tox);
    }
    else
    if( strstr(user_input,"/info") != NULL ) {
        local_info_display(tox);
    } else 
    // set relayer public_key
    if( strstr(user_input, "/relay") != NULL ) {
        set_relayer_public_key( user_input);
    }
    else
    if(strstr(user_input,"/set") != NULL ) {   // set a new name
            lineindex = 0;
            get_token( user_input );
            get_token( user_input );    // token = web3_id

            if( strlen(token) < 3 ) {
                console_out(">> Invalid new name. Try again.\n");
                return;
            }
          
            TOX_ERR_SET_INFO name_error;
            tox_self_set_name(tox, (const uint8_t*) token, strlen(token), &name_error);

            if (name_error == TOX_ERR_SET_INFO_OK) {
                console_out("\n>> Successfully set a new name.\n");
            }

    }else
    if( strstr(user_input,"/cur") != NULL)    // /cur channal info
    {
        char name[256];
        char* id = (char*) malloc(((size_t) TOX_PUBLIC_KEY_SIZE * 2) + 1);

        int friend_size = tox_friend_get_name_size(tox,pre_friendnumber,NULL);
        tox_friend_get_name(tox,pre_friendnumber,(uint8_t*)name,NULL);
        name[friend_size] = 0;
        console_out("\n>> Current Friend: %s\n",name);

        tox_friend_get_public_key(tox,pre_friendnumber,(uint8_t*)name,NULL);
        //name[76] = 0;
        for (size_t i = 0; i < TOX_PUBLIC_KEY_SIZE; ++i) {
                char a[3];
                snprintf(a, sizeof(a), "%02X", name[i] & 0xff);
                strcat(id, a);
        }
        console_out(">> PublicKey :\n   %s\n",id);

    }
    else
    if( strstr(user_input,"/add") != NULL && strlen(user_input) > 5 ) {
        int res = 0;
        lineindex = 1;
        get_token( user_input );
        get_token( user_input );    // token = web3_id

        int len = strlen(token);
        if( len != TOX_ADDRESS_SIZE*2 ) {
            console_out("\nInvald address,please try again! %d %lu\n",len, TOX_ADDRESS_SIZE);
            return;
        }

        TOX_ERR_FRIEND_ADD add_error;
        char add_mess[] = "arkMeta ask for Cyber future...";
        unsigned char *address = (unsigned char *) hex_string_to_bin(token);
        // token -> address
        res = tox_friend_add(tox, address, (uint8_t*) add_mess, strlen(add_mess), &add_error);
        
        /*TOX_ERR_FRIEND_ADD err;
        tox_friend_add_norequest(tox, public_key, &err);

    if (err != TOX_ERR_FRIEND_ADD_OK) {
        log_error_timestamp(err, "tox_friend_add_norequest failed");
    } else {
        log_timestamp("Accepted friend request");
    }
*/
        if (res >= 0)   // res: friend num
            console_out(">> Add new friend sucess!\n");
        else
            console_out(">> Add new friend error，maybe old fridend.\n");
    }else
        console_out(">> Invalid command input,try again.\n");
}
//
//  /id--  command
//
void local_node_info_display( Tox *m )
{
    char name[128] = "";
    char status_msg[256] = "";
    size_t i, size = tox_self_get_friend_list_size(m);
    size_t friend_size;

    if (size == 0) {
        return;
    }
    //chat_friend_statusmessage( wm, m, glob_friendnumber);

    size_t len = tox_self_get_name_size(m);
    tox_self_get_name(m, (uint8_t *) name);     // Node Name
    name[len] = '\0';
    //line_info_add(wm, false, NULL, NULL, SYS_MSG, 1, CYAN, " ");
    console_out( " Local Node NickName: %s\n", name);
    //line_info_add(wm, false, NULL, NULL, SYS_MSG, 1, CYAN, line_buffer);

    // status message display
    // tox_friend_get_status_message
    len = tox_self_get_status_message_size(m);
    tox_self_get_status_message(m,(uint8_t *) status_msg);
    status_msg[len] = '\0';
    console_out( "\n Status Message :\n   %s\n", status_msg);
    //line_info_add(wm, false, NULL, NULL, SYS_MSG, 1, CYAN, line_buffer);

    //time_t curtime = get_unix_time();
    //get_elapsed_time_str(timestr, sizeof(timestr), curtime - Tox_Bot.start_time);
    //console_out( " Uptime: %s\n", timestr);

    uint32_t numfriends = tox_self_get_friend_list_size(m);
    // console_out(" Friends: %d (%d online)\n", numfriends, Tox_Bot.num_online_friends);
    console_out( " Friends: %02d\n", numfriends );

    //line_info_add(wm, false, NULL, NULL, SYS_MSG, 1, CYAN, line_buffer);

    uint32_t list[size];
    tox_self_get_friend_list(m, list);

    // friendlist display ......
    for (i = 0; i < size; ++i) {
        friend_size = tox_friend_get_name_size(m,list[i],NULL);
        tox_friend_get_name(m,list[i],(uint8_t*)name,NULL);
        name[friend_size] = 0;

        if( tox_friend_get_connection_status( m, list[i], NULL) == TOX_CONNECTION_NONE ) {
            strcpy( status_msg, "offline");
        } else
        {
            strcpy( status_msg, " online");
        }

        console_out( "    %03zu :  %s  %s\n", i, status_msg, name);
    }

    //console_out(" Inactive friends are purged after %"PRIu64" days\n",
      //       Tox_Bot.inactive_limit / SECONDS_IN_DAY);

    /* List active group chats and number of peers in each */
    size_t num_chats = tox_conference_get_chatlist_size(m);

    if (num_chats == 0) {
        //line_info_add(wm, false, NULL, NULL, SYS_MSG, 1, CYAN, " No active groupchats");
        console_out(" No active groupchats\n");
    }

    /* The per-room detail loop below was commented out and unreachable, so it
     * has been dropped along with the storage it needed. That also removes a
     * dead assignment and an unused buffer. */
    if (num_chats > 0) {
        console_out(" Active groupchats: %zu\n", num_chats);
    }


    return;
}

/*
    main loop
*/
char inKey;
static char cm_buffer[512];
int line_index = 0;
char hello_msg[128] = "ZCZC HELLO-%04d NNNN";

int main(int argc, char **argv)
{
    const char *statusmsg = "metaCom@relayer-swarm: Command '/help' for more info";
    const char *ark_node_name = "arkMeta@元宇宙";
    
    (void) signal(SIGINT, catch_SIGINT);
    umask(S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    welcom_logo_metaCom();

    int ret = legacy_data_file_rename() ;

    if (ret != 0) {
        console_err("Failed to rename legacy data file. Error: %d\n", ret);
        exit(EXIT_FAILURE);
    }

    parse_args(argc, argv);

    Tox *m = init_tox();

    if (m == NULL) {
        console_out("System initial failure.\n");
        exit(EXIT_FAILURE);
    };

    mq_init();
    mq_set_drop_hook(on_telegram_dropped, NULL);
    mq_set_change_hook(on_queue_changed, NULL);
    conference_peer_list_initial(m);

    /* Restore telegrams that were still waiting for their peer when the process
     * last stopped. A missing file is the normal first-run state. */
    const int queue_restored = mq_persist_load(QUEUE_FILE, get_time());

    if (queue_restored > 0) {
        log_timestamp("Restored %d queued telegram(s) from %s", queue_restored, QUEUE_FILE);
    } else if (queue_restored < 0 && queue_restored != MQ_PERSIST_ERR_NOFILE) {
        log_error_timestamp(queue_restored, "Cannot restore the delivery queue from %s",
                            QUEUE_FILE);
    }

    if (msg_database_open() != MSG_DB_OK) {
        console_err("Warning: message database is unavailable; store-and-forward "
                    "history will not be persisted for this session.\n");
    }

    init_toxrelayer_state();
    load_conferences(m);
    
    tox_self_set_status_message(m, (uint8_t *) statusmsg, strlen(statusmsg), NULL);
    tox_self_set_name(m, (uint8_t *) ark_node_name, strlen(ark_node_name), NULL);


    print_profile_info(m);  // Node Message
    get_relay_name_list(m);

    /* The database is authoritative for the receive serial numbers, and the
     * relay environment must be in place before any record is handled. */
    relay_init();

    if (relay_recover_trackers(&g_relay, relay_last_stored_sn, MAX_Friend_NUM) > 0) {
        log_timestamp("Recovered receive serial numbers from stored history");
    }
    
    time_t cur_time = get_time();
    time_t last_queue_service = cur_time;
    time_t last_queue_save = cur_time;

    uint64_t last_friend_purge = cur_time;
    uint64_t last_group_purge  = cur_time;
    msg_keeper.receipt = 0;

    for( int i = 0; i < max_friend_num ; i++ ) {
        chat_tx_Control[i].last_msg_tx = cur_time;
        chat_tx_Control[i].hello_counter = 1;
    };

    while (!FLAG_EXIT) {
        
        TOX_CONNECTION connection_status = tox_self_get_connection_status(m);

        // start to connect.......
        if (connection_status == TOX_CONNECTION_NONE
                && timed_out(Tox_Bot.last_bootstrap, cur_time, BOOTSTRAP_INTERVAL)) {
            log_timestamp("Bootstrapping to network...");
            bootstrap_DHT(m);
            Tox_Bot.last_bootstrap = cur_time;
        }

        if (connection_status != TOX_CONNECTION_NONE 
                                && timed_out(last_friend_purge, cur_time, FRIEND_PURGE_INTERVAL)) {
            purge_inactive_friends(m);
            save_data(m, DATA_FILE);
            last_friend_purge = cur_time;
        }

        if (check_group_purge(last_group_purge, cur_time, connection_status)) {
            purge_empty_groups(m);
            last_group_purge = cur_time;
        }

        tox_iterate(m, NULL);

        usleep(tox_iteration_interval(m) * 1000);

        cur_time = get_time();

        // Hello process ...... ???? 
        for( int i = 0; i < max_friend_num; i++ )
        {
            if( (cur_time - chat_tx_Control[i].last_msg_tx) > HELL_SIGNAL_INTERVAL
            && connection_status != TOX_CONNECTION_NONE  )
            {
                // /HELLO-0018 WEAXSDRT METACOM -->
                snprintf(hello_msg, sizeof(hello_msg), "/HELLO-%04d  %s",
                                 chat_tx_Control[i].hello_counter++, hello_selfname_msg);
                if(chat_tx_Control[i].hello_counter >= 1000 ) 
                        chat_tx_Control[i].hello_counter = 1;
                tox_friend_send_message(m, i, TOX_MESSAGE_TYPE_NORMAL, 
                                    (uint8_t *) hello_msg, strlen(hello_msg), NULL);
                chat_tx_Control[i].last_msg_tx = get_time();
            }
        };

        // message relay modual .......FIFO Quqe
        if ((cur_time - last_queue_service) > 1) {
            mq_service( tox_transport, m, cur_time );
            last_queue_service = cur_time;
        }

        /* Mirror the queue to disk once it has changed, but no more often than
         * QUEUE_SAVE_INTERVAL, so a busy relay does not rewrite the file on
         * every tick. exit_toxrelayer() flushes whatever is left. */
        if (g_queue_dirty && timed_out(last_queue_save, cur_time, QUEUE_SAVE_INTERVAL)) {
            if (mq_persist_save(QUEUE_FILE) == MQ_PERSIST_OK) {
                g_queue_dirty = false;
            } else {
                log_error_timestamp(-1, "Cannot persist the delivery queue to %s", QUEUE_FILE);
            }

            last_queue_save = cur_time;
        }

        // local command processing functions
        if( console_key_available() ) {
            inKey = getchar();
            switch( inKey ) {
            case '\n':
            case '\r':
                cm_buffer[line_index++]  = inKey;
                cm_buffer[line_index]    = 0;
              
                /* If first char is a '/' then user is entering a command */
                if( cm_buffer[0] == '/') {
                    evaluate_input( m, cm_buffer );    // local command process ........
                }
                else 
                {
                    // outgoing direction: pre_friendnumber
                    //      ::normal text format
                    //      ZCZC TEXT
                    //      receive_pubkey 
                    //      sender_pubkey 
                    //      send_sn time_stamp
                    //      text ... ... ... ... ...
                    //      NNNN
                    friend_name_size = tox_friend_get_name_size(m, pre_friendnumber,NULL);
                    tox_friend_get_name(m,pre_friendnumber,(uint8_t*)tx_name,NULL);
                    tx_name[friend_name_size] = 0;
                    
                    //----------------------------------------------------------------
                    //  for message sending ......
                    char sn_buffer[16];
                    char key_buffer[64];
                    char wire[TOX_MAX_MESSAGE_LENGTH + 1];

                    const int send_sn = tg_sn_next(relay_name_list[pre_friendnumber].send_sn);
                    relay_name_list[pre_friendnumber].send_sn = send_sn;

                    // keep in config file
                    snprintf(key_buffer, sizeof(key_buffer), "send%03d", pre_friendnumber);
                    snprintf(sn_buffer, sizeof(sn_buffer), "%04d", send_sn);
                    ini_puts("outgoing_sn", key_buffer, sn_buffer, config_file);

                    const time_t cur_time = get_unix_time();

                    /* The outbound record is produced by the verified encoder, so
                     * the framing and the field limits are the same ones the tests
                     * exercise. */
                    const int wire_len = tg_encode_text(wire, sizeof(wire),
                                                        relay_name_list[pre_friendnumber].public_key_str,
                                                        self_public_key_str,
                                                        send_sn, (long) cur_time, cm_buffer);

                    if (wire_len < 0 && relay_name_list[pre_friendnumber].meta_chat_flag) {
                        log_error_timestamp(wire_len, "Cannot encode telegram %d for channel %d",
                                            send_sn, pre_friendnumber);
                    } else if (relay_name_list[pre_friendnumber].meta_chat_flag) {
                        (void) mq_enqueue(pre_friendnumber, wire, (size_t) wire_len, cur_time);
                    } else {
                        (void) mq_enqueue(pre_friendnumber, cm_buffer, strlen(cm_buffer), cur_time);
                    }

                    console_out(">> Outgoing Msg~ %s: %s\n", tx_name, cm_buffer);

                    /* Only the body is persisted. When a peer later asks for a
                     * replay the wire record is rebuilt by the encoder above, so a
                     * stored blob can never be replayed in a malformed state. */
                    if (msg_database_add(pre_friendnumber, send_sn, cur_time,
                                         self_public_key_str, "outgoing", cm_buffer) != MSG_DB_OK) {
                        log_error_timestamp(-1, "Cannot store outgoing telegram %d for channel %d",
                                            send_sn, pre_friendnumber);
                    }

                    chat_tx_Control[pre_friendnumber].last_msg_tx = get_time();
                }
                
                line_index = 0;
                break;
            case '\b':  // backspace
                        // deleter ????
                if( line_index > 0 ) {
                    line_index--;
                    cm_buffer[line_index]    = 0;
                }

                break;
            default:
                if( line_index < 255 )
                    cm_buffer[line_index++] = inKey;
            };
        };
    }

    exit_toxrelayer(m);

    return 0;
}
//----------------------------------------------------------------
//
void welcom_logo_metaCom()
{
     console_out( "\n");
    console_out( "    _____ _____  _____ ____ \n");
    console_out( "   |_   _/ _ \\ \\/ /_ _/ ___|\n");
    console_out( "     | || | | \\  / | | |    \n");
    console_out( "     | || |_| /  \\ | | |___ \n");
    console_out( "     |_| \\___/_/\\_\\___\\____| ...Second Life in Metaverse.\n\n");

    console_out( " > Welcome to metaCom freedom_world!\n"); 
    console_out( "     Start with blockchain-based web3 instant messaging.\n\n");
}