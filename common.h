#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define SERVER_PORT     8080
#define SERVER_IP       "127.0.0.1"
#define MAX_CLIENTS     50
#define BUFFER_SIZE     1024
#define PIPE_PATH       "/tmp/traffic_analytics_pipe"
#define SHM_NAME        "/traffic_shm_stats"
#define MQ_NAME         "/traffic_alert_queue"
#define MAX_INCIDENTS   500
#define MAX_ZONES       10
#define MAX_STR         256
#define MAX_USER_LEN    50
#define MAX_PASS_LEN    50
#define WATCHDOG_INTERVAL_SEC   15
#define CRITICAL_TIMEOUT_SEC    60
#define HIGH_TIMEOUT_SEC       120
#define MQ_MAX_MSG      10
#define MQ_MSG_SIZE     256

typedef enum { ROLE_UNKNOWN=0, ROLE_COMMUTER=1, ROLE_OFFICER=2, ROLE_CONTROL_ROOM=3 } UserRole;
typedef enum { PRIORITY_LOW=0, PRIORITY_MEDIUM=1, PRIORITY_HIGH=2, PRIORITY_CRITICAL=3 } Priority;
typedef enum { STATUS_PENDING=0, STATUS_INVESTIGATING=1, STATUS_RESOLVED=2, STATUS_ESCALATED=3 } IncidentStatus;

typedef struct {
    int            id;
    char           description[MAX_STR];
    IncidentStatus status;
    Priority       priority;
    char           reported_by[MAX_USER_LEN];
    char           claimed_by[MAX_USER_LEN];
    int            zone;
    time_t         timestamp;
    time_t         claimed_at;
    time_t         resolved_at;
    int            escalated;
    int            active;
} Incident;

typedef struct {
    int    total_created;
    int    total_resolved;
    int    total_escalated;
    int    by_priority[4];
    int    by_zone[MAX_ZONES + 1];
    double avg_response_sec;
    int    response_samples;
    time_t last_updated;
} SharedStats;

typedef struct {
    int      incident_id;
    int      zone;
    Priority priority;
    char     description[MAX_STR];
    time_t   pending_since;
} EscalationAlert;

#define CMD_LOGIN    "LOGIN"
#define CMD_REPORT   "REPORT"
#define CMD_LIST     "LIST"
#define CMD_CLAIM    "CLAIM"
#define CMD_UPDATE   "UPDATE"
#define CMD_CLOSE    "CLOSE"
#define CMD_LISTALL  "LISTALL"
#define CMD_STATS    "STATS"
#define CMD_ALERTS   "ALERTS"
#define CMD_QUIT     "QUIT"

#define RESP_OK        "OK"
#define RESP_FAIL      "FAIL"
#define RESP_AUTH_OK   "AUTH_OK"
#define RESP_AUTH_FAIL "AUTH_FAIL"
#define RESP_DENIED    "DENIED"
#define RESP_END       "END_OF_LIST"

#define LOG(fmt, ...) do { \
    time_t _t=time(NULL); struct tm *_tm=localtime(&_t); char _ts[32]; \
    strftime(_ts,sizeof(_ts),"%H:%M:%S",_tm); \
    fprintf(stdout,"[%s] "fmt"\n",_ts,##__VA_ARGS__); fflush(stdout); } while(0)

#define ERR(fmt,...) do { \
    fprintf(stderr,"[ERROR] "fmt"\n",##__VA_ARGS__); fflush(stderr); } while(0)

static inline const char *role_to_str(UserRole r){
    switch(r){case ROLE_COMMUTER:return"COMMUTER";case ROLE_OFFICER:return"OFFICER";
    case ROLE_CONTROL_ROOM:return"CONTROL_ROOM";default:return"UNKNOWN";}}

static inline const char *status_to_str(IncidentStatus s){
    switch(s){case STATUS_PENDING:return"PENDING";case STATUS_INVESTIGATING:return"INVESTIGATING";
    case STATUS_RESOLVED:return"RESOLVED";case STATUS_ESCALATED:return"ESCALATED";default:return"UNKNOWN";}}

static inline const char *priority_to_str(Priority p){
    switch(p){case PRIORITY_LOW:return"LOW";case PRIORITY_MEDIUM:return"MEDIUM";
    case PRIORITY_HIGH:return"HIGH";case PRIORITY_CRITICAL:return"CRITICAL";default:return"UNKNOWN";}}

static inline Priority str_to_priority(const char *s){
    if(strcmp(s,"CRITICAL")==0)return PRIORITY_CRITICAL;
    if(strcmp(s,"HIGH")==0)return PRIORITY_HIGH;
    if(strcmp(s,"MEDIUM")==0)return PRIORITY_MEDIUM;
    return PRIORITY_LOW;}

#endif
