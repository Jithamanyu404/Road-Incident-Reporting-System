#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#include "../common.h"
#include "auth.h"
#include "incident.h"
#include "file_manager.h"
#include "ipc_pipe.h"
#include "shm_stats.h"
#include "mq_alerts.h"
#include "watchdog.h"

/*
 * main.c v2 — New commands: STATS, ALERTS
 * New startup: crash recovery + watchdog thread + shared memory + message queue
 */

typedef struct
{
    int fd;
    Session session;
} ClientState;
static volatile sig_atomic_t running = 1;
static int server_fd = -1;

static void handle_sigint(int sig)
{
    (void)sig;
    LOG("Shutdown signal received");
    running = 0;
    // Close server socket to unblock accept()
    if (server_fd >= 0)
    {
        shutdown(server_fd, SHUT_RD);
    }
}

static void send_line(int fd, const char *msg)
{
    char buf[BUFFER_SIZE + 2];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    send(fd, buf, strlen(buf), MSG_NOSIGNAL);
}

static int recv_line(int fd, char *buf, size_t size)
{
    size_t i = 0;
    char c;
    while (i < size - 1)
    {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0)
            return -1;
        if (c == '\n')
            break;
        if (c != '\r')
            buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

static void process_command(ClientState *cs, const char *line)
{
    int fd = cs->fd;
    Session *sess = &cs->session;
    char cmd[32], arg1[MAX_STR], arg2[32];
    cmd[0] = arg1[0] = arg2[0] = '\0';
    sscanf(line, "%31s %255s %31s", cmd, arg1, arg2);

    if (!has_permission(sess, cmd))
    {
        send_line(fd, RESP_DENIED);
        LOG("[%s] DENIED: %s", sess->username, cmd);
        return;
    }

    // ── REPORT ────────────────────────────────────────────────────────────
    if (strcmp(cmd, CMD_REPORT) == 0)
    {
        // Format: REPORT <zone> <priority> <description>
        int zone = 0;
        char prio_s[16] = "LOW";
        char desc[MAX_STR] = {0};
        if (sscanf(line, "%*s %d %15s %[^\n]", &zone, prio_s, desc) < 3 || zone < 1 || zone > MAX_ZONES)
        {
            send_line(fd, "FAIL Usage: REPORT <zone> LOW|MEDIUM|HIGH|CRITICAL <description>");
            return;
        }
        Priority p = str_to_priority(prio_s);
        int id = incident_add(desc, zone, sess->username, p);
        if (id < 0)
        {
            send_line(fd, "FAIL Could not create incident");
            return;
        }
        char resp[128];
        snprintf(resp, sizeof(resp), "OK Incident #%d [%s] created in zone %d", id, priority_to_str(p), zone);
        send_line(fd, resp);
        return;
    }

    // ── LIST ──────────────────────────────────────────────────────────────
    if (strcmp(cmd, CMD_LIST) == 0)
    {
        int zone = (sess->role == ROLE_OFFICER) ? sess->zone : atoi(arg1);
        if (zone < 1 || zone > MAX_ZONES)
        {
            send_line(fd, "FAIL Invalid zone");
            return;
        }
        char buf[BUFFER_SIZE * 6];
        buf[0] = '\0';
        int count = incident_list_zone(zone, buf, sizeof(buf));
        if (count == 0)
            send_line(fd, "(no active incidents in this zone)");
        else
        {
            char *p = buf, *nl;
            while ((nl = strchr(p, '\n')) != NULL)
            {
                *nl = '\0';
                send_line(fd, p);
                p = nl + 1;
            }
            if (strlen(p) > 0)
                send_line(fd, p);
        }
        send_line(fd, RESP_END);
        return;
    }

    // ── LISTALL ───────────────────────────────────────────────────────────
    if (strcmp(cmd, CMD_LISTALL) == 0)
    {
        char buf[BUFFER_SIZE * 10];
        buf[0] = '\0';
        int count = incident_list_all(buf, sizeof(buf));
        if (count == 0)
            send_line(fd, "(no active incidents)");
        else
        {
            char *p = buf, *nl;
            while ((nl = strchr(p, '\n')) != NULL)
            {
                *nl = '\0';
                send_line(fd, p);
                p = nl + 1;
            }
            if (strlen(p) > 0)
                send_line(fd, p);
        }
        send_line(fd, RESP_END);
        return;
    }

    // ── CLAIM ─────────────────────────────────────────────────────────────
    if (strcmp(cmd, CMD_CLAIM) == 0)
    {
        int id = atoi(arg1);
        if (id <= 0)
        {
            send_line(fd, "FAIL Usage: CLAIM <id>");
            return;
        }
        int zone = (sess->role == ROLE_CONTROL_ROOM) ? -1 : sess->zone;
        int r = incident_claim(id, sess->username, zone);
        switch (r)
        {
        case 0:
            send_line(fd, "OK Incident claimed");
            break;
        case -1:
            send_line(fd, "FAIL Already claimed");
            break;
        case -2:
            send_line(fd, "FAIL Not found");
            break;
        case -3:
            send_line(fd, "FAIL Not in your zone");
            break;
        }
        return;
    }

    // ── UPDATE ────────────────────────────────────────────────────────────
    if (strcmp(cmd, CMD_UPDATE) == 0)
    {
        int id = atoi(arg1);
        IncidentStatus ns;
        if (strcmp(arg2, "INVESTIGATING") == 0)
            ns = STATUS_INVESTIGATING;
        else if (strcmp(arg2, "RESOLVED") == 0)
            ns = STATUS_RESOLVED;
        else
        {
            send_line(fd, "FAIL Usage: UPDATE <id> INVESTIGATING|RESOLVED");
            return;
        }
        int r = incident_update_status(id, ns, sess->username, sess->role);
        switch (r)
        {
        case 0:
            send_line(fd, "OK Status updated");
            break;
        case -1:
            send_line(fd, "FAIL Invalid transition");
            break;
        case -2:
            send_line(fd, "FAIL Not found");
            break;
        case -3:
            send_line(fd, "FAIL Not your incident");
            break;
        }
        return;
    }

    // ── CLOSE ─────────────────────────────────────────────────────────────
    if (strcmp(cmd, CMD_CLOSE) == 0)
    {
        int id = atoi(arg1);
        int r = incident_close(id, sess->username);
        send_line(fd, r == 0 ? "OK Incident closed" : "FAIL Not found");
        return;
    }

    // ── STATS (NEW) — reads from shared memory ────────────────────────────
    if (strcmp(cmd, CMD_STATS) == 0)
    {
        SharedStats snap;
        shm_get_snapshot(&snap);
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "=== LIVE STATS (Shared Memory) ===\n"
                 "Total Created  : %d\n"
                 "Total Resolved : %d\n"
                 "Total Escalated: %d\n"
                 "Avg Response   : %.1f seconds\n"
                 "By Priority    : CRITICAL=%d HIGH=%d MEDIUM=%d LOW=%d",
                 snap.total_created, snap.total_resolved, snap.total_escalated,
                 snap.avg_response_sec,
                 snap.by_priority[PRIORITY_CRITICAL], snap.by_priority[PRIORITY_HIGH],
                 snap.by_priority[PRIORITY_MEDIUM], snap.by_priority[PRIORITY_LOW]);
        char *p = buf, *nl;
        while ((nl = strchr(p, '\n')) != NULL)
        {
            *nl = '\0';
            send_line(fd, p);
            p = nl + 1;
        }
        if (strlen(p))
            send_line(fd, p);
        send_line(fd, RESP_END);
        return;
    }

    // ── ALERTS (NEW — control room only) — drain message queue ───────────
    if (strcmp(cmd, CMD_ALERTS) == 0)
    {
        EscalationAlert alert;
        int count = 0;
        while (mq_alerts_receive(&alert) == 1)
        {
            char msg[512];
            char ts[32];
            struct tm *tm_info = localtime(&alert.pending_since);
            strftime(ts, sizeof(ts), "%H:%M:%S", tm_info);
            snprintf(msg, sizeof(msg),
                     "!! ESCALATION: Incident #%d | Zone:%d | %s | \"%s\" | Pending since %s",
                     alert.incident_id, alert.zone,
                     priority_to_str(alert.priority),
                     alert.description, ts);
            send_line(fd, msg);
            count++;
        }
        if (count == 0)
            send_line(fd, "(no pending alerts)");
        send_line(fd, RESP_END);
        return;
    }

    send_line(fd, "FAIL Unknown command");
}

static void *handle_client(void *arg)
{
    ClientState *cs = (ClientState *)arg;
    int fd = cs->fd;
    LOG("Client connected fd=%d", fd);

    send_line(fd, "WELCOME Traffic Incident System v2. LOGIN <user> <pass>");
    char line[BUFFER_SIZE];
    if (recv_line(fd, line, sizeof(line)) < 0)
        goto cleanup;

    char cmd_buf[16], username[MAX_USER_LEN] = {0}, password[MAX_PASS_LEN] = {0};
    sscanf(line, "%15s %49s %49s", cmd_buf, username, password);
    if (strcmp(cmd_buf, "LOGIN") != 0)
    {
        send_line(fd, RESP_AUTH_FAIL);
        goto cleanup;
    }

    int zone = -1;
    UserRole role = authenticate(username, password, &zone);
    if (role == ROLE_UNKNOWN)
    {
        send_line(fd, RESP_AUTH_FAIL);
        goto cleanup;
    }

    strncpy(cs->session.username, username, MAX_USER_LEN - 1);
    cs->session.role = role;
    cs->session.zone = zone;

    char welcome[192];
    snprintf(welcome, sizeof(welcome), "%s Welcome %s! Role:%s Zone:%d",
             RESP_AUTH_OK, username, role_to_str(role), zone);
    send_line(fd, welcome);
    LOG("Auth OK: %s as %s", username, role_to_str(role));

    while (recv_line(fd, line, sizeof(line)) >= 0)
    {
        if (!strlen(line))
            continue;
        if (strcmp(line, CMD_QUIT) == 0)
        {
            send_line(fd, "OK Goodbye");
            break;
        }
        process_command(cs, line);
    }

cleanup:
    LOG("Client disconnected fd=%d user=%s", fd, cs->session.username);
    close(fd);
    free(cs);
    return NULL;
}

int main(void)
{
    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, SIG_IGN); // Don't let SIGUSR1 break accept

    LOG("=== Traffic Incident Reporting Server v2 ===");

    file_manager_init();
    incidents_init();

    // CRASH RECOVERY - Load existing incidents from logs
    incidents_load_from_logs();
    shm_init();
    incidents_rebuild_shm_stats();

    mq_alerts_init_server();
    pipe_init();
    watchdog_start();

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }
    if (listen(server_fd, MAX_CLIENTS) < 0)
    {
        perror("listen");
        return 1;
    }

    LOG("Listening on port %d", SERVER_PORT);
    LOG("Commands: REPORT <zone> <priority> <desc> | CLAIM | UPDATE | CLOSE | STATS | ALERTS | LISTALL");
    LOG("Watchdog: CRITICAL escalates in %ds, HIGH in %ds", CRITICAL_TIMEOUT_SEC, HIGH_TIMEOUT_SEC);
    LOG("CRASH RECOVERY: Loaded %d incidents from log files", incident_count);

    while (running)
    {
        struct sockaddr_in ca;
        socklen_t al = sizeof(ca);
        int cfd = accept(server_fd, (struct sockaddr *)&ca, &al);
        if (cfd < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
            {
                continue; // Signal interrupted - just continue
            }
            if (running)
            {
                ERR("accept: %s", strerror(errno));
            }
            break;
        }
        ClientState *cs = calloc(1, sizeof(ClientState));
        if (!cs)
        {
            close(cfd);
            continue;
        }
        cs->fd = cfd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cs) != 0)
        {
            free(cs);
            close(cfd);
            continue;
        }
        pthread_detach(tid);
    }

    LOG("Server shutting down...");
    watchdog_stop();
    if (server_fd >= 0)
        close(server_fd);
    pipe_close();
    mq_alerts_close_server();
    shm_destroy();
    incidents_destroy();
    LOG("Server stopped. Total incidents processed: %d", incident_count);
    return 0;
}