#ifndef AUTH_H
#define AUTH_H
#include "../common.h"
#define USERS_FILE "data/users.txt"
typedef struct {
    char     username[MAX_USER_LEN];
    UserRole role;
    int      zone;
} Session;
UserRole authenticate(const char *username, const char *password, int *zone_out);
int has_permission(const Session *session, const char *command);
#endif
