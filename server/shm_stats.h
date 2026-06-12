#ifndef SHM_STATS_H
#define SHM_STATS_H
#include "../common.h"
#include <pthread.h>

// Shared memory segment — readable by analytics process too
typedef struct {
    pthread_mutex_t lock;   // Cross-process mutex (PTHREAD_PROCESS_SHARED)
    SharedStats     stats;
} ShmSegment;

extern ShmSegment *shm;

void shm_init(void);
void shm_destroy(void);
void shm_record_created(Priority p, int zone);
void shm_record_resolved(Priority p, int zone, double response_secs);
void shm_record_escalated(void);
void shm_get_snapshot(SharedStats *out);

#endif
