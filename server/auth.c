#include "auth.h"

UserRole authenticate(const char *username, const char *password, int *zone_out) {
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) { ERR("Cannot open %s", USERS_FILE); return ROLE_UNKNOWN; }
    char u[MAX_USER_LEN], p[MAX_PASS_LEN], r[32];
    int zone = -1;
    while (fscanf(f, "%49s %49s %31s %d", u, p, r, &zone) >= 3) {
        if (strcmp(u, username) == 0 && strcmp(p, password) == 0) {
            fclose(f);
            if (zone_out) *zone_out = zone;
            if (strcmp(r, "control_room") == 0) return ROLE_CONTROL_ROOM;
            if (strcmp(r, "officer")      == 0) return ROLE_OFFICER;
            if (strcmp(r, "commuter")     == 0) return ROLE_COMMUTER;
        }
        zone = -1;
    }
    fclose(f);
    return ROLE_UNKNOWN;
}

int has_permission(const Session *session, const char *command) {
    if (!session || session->role == ROLE_UNKNOWN) return 0;
    if (strcmp(command, CMD_LIST)  == 0) return 1;
    if (strcmp(command, CMD_QUIT)  == 0) return 1;
    if (strcmp(command, CMD_STATS) == 0) return 1;
    if (strcmp(command, CMD_REPORT) == 0) return 1;
    if (strcmp(command, CMD_ALERTS) == 0) return session->role == ROLE_CONTROL_ROOM;
    if (strcmp(command, CMD_CLAIM)  == 0) return session->role == ROLE_OFFICER || session->role == ROLE_CONTROL_ROOM;
    if (strcmp(command, CMD_UPDATE) == 0) return session->role == ROLE_OFFICER || session->role == ROLE_CONTROL_ROOM;
    if (strcmp(command, CMD_CLOSE)   == 0) return session->role == ROLE_CONTROL_ROOM;
    if (strcmp(command, CMD_LISTALL) == 0) return session->role == ROLE_CONTROL_ROOM;
    return 0;
}

