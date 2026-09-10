/*  groupchats.h
 *
 */

#ifndef GROUPCHATS_H
#define GROUPCHATS_H

#define SECONDS_IN_DAY 86400UL
#define MAX_PASSWORD_SIZE 64

struct Group_Chat {
    uint32_t groupnum;
    bool active;
    bool has_pass;
    uint8_t type;
    char title[TOX_MAX_NAME_LENGTH];
    int title_len;
    char password[MAX_PASSWORD_SIZE];
};

int group_add(uint32_t groupnum, uint8_t type, const char *password);
void group_leave(uint32_t groupnum);
int group_index(uint32_t groupnum);
void realloc_groupchats(int n);

#endif  /* GROUPCHATS_H */

