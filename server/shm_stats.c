#include "shm_stats.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

/*
 * shm_stats.c — Shared Memory (IPC Concept 4.6 — second mechanism)
 *
 * A POSIX shared memory segment holds live statistics that BOTH the
 * server process AND the analytics process can read simultaneously.
 *
 * Unlike the named pipe (which streams events one-way), shared memory
 * gives the analytics process direct read access to the latest counters
 * at any time — no message passing needed.
 *
 * Cross-process mutex:
 *   The ShmSegment contains a pthread_mutex_t initialized with
 *   PTHREAD_PROCESS_SHARED attribute. This means the mutex works
 *   across process boundaries — the server locks it to update stats,
 *   and the analytics process locks it to read a consistent snapshot.
 *   Without this, analytics could read a half-updated stats struct.
 *
 * This demonstrates TWO IPC mechanisms simultaneously:
 *   Pipe        → event streaming (server → analytics)
 *   Shared mem  → live stat counters (server writes, analytics reads)
 */

ShmSegment *shm = NULL;
static int shm_fd = -1;

void shm_init(void) {
    // Create or open the shared memory object
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        ERR("shm_open failed: %s", strerror(errno));
        return;
    }

    // Set the size of the shared memory segment
    if (ftruncate(shm_fd, sizeof(ShmSegment)) < 0) {
        ERR("ftruncate failed: %s", strerror(errno));
        return;
    }

    // Map it into our address space
    shm = mmap(NULL, sizeof(ShmSegment),
               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) {
        ERR("mmap failed: %s", strerror(errno));
        shm = NULL;
        return;
    }

    // Initialize the cross-process mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    memset(&shm->stats, 0, sizeof(SharedStats));
    shm->stats.last_updated = time(NULL);

    LOG("Shared memory initialized: %s (%zu bytes)", SHM_NAME, sizeof(ShmSegment));
}

void shm_destroy(void) {
    if (shm) {
        pthread_mutex_destroy(&shm->lock);
        munmap(shm, sizeof(ShmSegment));
        shm = NULL;
    }
    if (shm_fd >= 0) close(shm_fd);
    shm_unlink(SHM_NAME);
}

void shm_record_created(Priority p, int zone) {
    if (!shm) return;
    pthread_mutex_lock(&shm->lock);
    shm->stats.total_created++;
    shm->stats.by_priority[p]++;
    if (zone >= 1 && zone <= MAX_ZONES) shm->stats.by_zone[zone]++;
    shm->stats.last_updated = time(NULL);
    pthread_mutex_unlock(&shm->lock);
}

void shm_record_resolved(Priority p, int zone, double response_secs) {
    if (!shm) return;
    pthread_mutex_lock(&shm->lock);
    shm->stats.total_resolved++;
    // Rolling average: new_avg = (old_avg * n + new_val) / (n+1)
    int n = shm->stats.response_samples;
    shm->stats.avg_response_sec =
        (shm->stats.avg_response_sec * n + response_secs) / (n + 1);
    shm->stats.response_samples++;
    shm->stats.last_updated = time(NULL);
    (void)p; (void)zone;
    pthread_mutex_unlock(&shm->lock);
}

void shm_record_escalated(void) {
    if (!shm) return;
    pthread_mutex_lock(&shm->lock);
    shm->stats.total_escalated++;
    shm->stats.last_updated = time(NULL);
    pthread_mutex_unlock(&shm->lock);
}

void shm_get_snapshot(SharedStats *out) {
    if (!shm) { memset(out, 0, sizeof(SharedStats)); return; }
    pthread_mutex_lock(&shm->lock);
    memcpy(out, &shm->stats, sizeof(SharedStats));
    pthread_mutex_unlock(&shm->lock);
}
