#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include "../common.h"

/*
 * analytics.c v2 — Dual IPC: Named Pipe + Shared Memory
 *
 * Now reads from TWO sources simultaneously:
 *   1. Named pipe  — event stream (CREATED, CLAIMED, RESOLVED, ESCALATED)
 *   2. Shared memory — live counters written by server threads
 *
 * The dashboard shows both: pipe-counted events AND shared memory stats.
 * This demonstrates that two completely different IPC mechanisms can
 * coexist and complement each other in one system.
 */

#define MAX_ZONES 10
typedef struct { int created,claimed,resolved,escalated; } ZoneStats;
static ZoneStats   zone_stats[MAX_ZONES+1];
static int         total_events = 0;
static volatile int running = 1;

// Shared memory — mapped read-only in analytics
typedef struct {
    // Must match ShmSegment layout in shm_stats.h
    // We only read stats, never write — no lock needed for read if careful
    char     _mutex_pad[48]; // pthread_mutex_t is typically 40 bytes, pad safely
    SharedStats stats;
} ShmView;

static ShmView *shm_view = NULL;

static void handle_sig(int sig)
{
    (void)sig;
    printf("\n\nStopping analytics...\n");
    running = 0;
}



static void map_shared_memory(void) {
    int fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (fd < 0) return;  // Not created yet
    shm_view = mmap(NULL, sizeof(ShmView), PROT_READ, MAP_SHARED, fd, 0);
    if (shm_view == MAP_FAILED) shm_view = NULL;
    close(fd);
}

static void print_dashboard(int last_zone, const char *last_event, int last_id)
{
    printf("\033[2J\033[H");
    printf("========================================================================\n");
    printf("                         ANALYTICS DASHBOARD                            \n");
    printf("========================================================================\n");

    printf("[PIPE] Last: %-10s Zone: %-2d ID: %-4d Total Events: %-5d\n",
           last_event, last_zone, last_id, total_events);

    if (shm_view)
    {
        SharedStats *s = &shm_view->stats;
        printf("[SHM]  Created: %-5d Resolved: %-5d Avg Response: %-5.1fs\n",
               s->total_created, s->total_resolved, s->avg_response_sec);
        printf("[SHM]  Priority: CRIT:%-3d HIGH:%-3d MED:%-3d LOW:%-3d\n",
               s->by_priority[3], s->by_priority[2], s->by_priority[1], s->by_priority[0]);
    }
    else
    {
        printf("[SHM]  Shared memory not yet available\n");
        map_shared_memory();
    }

    printf("\n------------------------------------------------------------------------\n");
    printf("%-8s %-10s %-10s %-10s %-10s\n", "ZONE", "CREATED", "CLAIMED", "RESOLVED", "PENDING");
    printf("------------------------------------------------------------------------\n");

    for (int z = 1; z <= MAX_ZONES; z++)
    {
        if (zone_stats[z].created == 0 && zone_stats[z].claimed == 0 &&
            zone_stats[z].resolved == 0)
            continue;
        int pending = zone_stats[z].created - zone_stats[z].resolved;
        if (pending < 0)
            pending = 0;

        printf("%-8d %-10d %-10d %-10d %-10d\n",
               z,
               zone_stats[z].created,
               zone_stats[z].claimed,
               zone_stats[z].resolved,
               pending);
    }
    printf("------------------------------------------------------------------------\n");
    printf("Pipe: %s | SHM: %s\n", PIPE_PATH, shm_view ? SHM_NAME : "not mapped");
    fflush(stdout);
}

int main(void) {
    signal(SIGINT, handle_sig); signal(SIGTERM, handle_sig);
    printf("=== Traffic Analytics Process v2 ===\n");
    printf("IPC channels: Named Pipe + Shared Memory\n");

    if (mkfifo(PIPE_PATH, 0666)==-1 && errno!=EEXIST){ perror("mkfifo"); return 1; }
    memset(zone_stats, 0, sizeof(zone_stats));

    printf("Waiting for server...\n");
    int fd = open(PIPE_PATH, O_RDONLY);
    if (fd < 0){ perror("open pipe"); return 1; }
    printf("Connected! Receiving events...\n");

    map_shared_memory();

    char buf[256], leftover[256]={0};
    size_t leftover_len = 0;

    while (running) {
        fd_set readfds;
        struct timeval tv = {0, 100000}; // 100ms timeout

        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);

        int ret = select(fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ret == 0)
            continue;

        ssize_t n = read(fd, buf+leftover_len, sizeof(buf)-leftover_len-1);
        if (n <= 0) {
            if (n==0){ close(fd); printf("\nReconnecting...\n"); fd=open(PIPE_PATH,O_RDONLY); if(fd<0)break; continue; }
            if (errno==EINTR) continue;
            break;
        }
        memcpy(buf, leftover, leftover_len);
        buf[leftover_len+n]='\0';
        char *ls=buf, *nl;
        while((nl=strchr(ls,'\n'))!=NULL){
            *nl='\0';
            char event[32]; int zone=0,id=0; long ts=0;
            if(sscanf(ls,"%31[^,],%d,%d,%ld",event,&zone,&id,&ts)==4 && zone>=1 && zone<=MAX_ZONES){
                if(strcmp(event,"CREATED") ==0) zone_stats[zone].created++;
                if(strcmp(event,"CLAIMED") ==0) zone_stats[zone].claimed++;
                if(strcmp(event,"RESOLVED")==0) zone_stats[zone].resolved++;
                if(strcmp(event,"CLOSED")  ==0) zone_stats[zone].resolved++;
                if(strcmp(event,"ESCALATED")==0)zone_stats[zone].escalated++;
                total_events++;
                if (!shm_view) map_shared_memory();
                print_dashboard(zone, event, id);
            }
            ls=nl+1;
        }
        leftover_len=strlen(ls);
        if(leftover_len>0) memcpy(leftover,ls,leftover_len);
        else leftover_len=0;
    }
    close(fd);
    if (shm_view) munmap(shm_view, sizeof(ShmView));
    printf("\nAnalytics stopped. Total events: %d\n", total_events);
    return 0;
}

