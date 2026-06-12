#include "watchdog.h"
#include "incident.h"
#include "mq_alerts.h"
#include "shm_stats.h"
#include <pthread.h>
#include <signal.h>
#include <errno.h>

/*
 * watchdog.c — Auto-Escalation Watchdog (Signals + New Feature)
 *
 * Runs as a dedicated pthread inside the server process.
 * Every WATCHDOG_INTERVAL_SEC seconds it:
 *
 *   1. Scans all PENDING incidents
 *   2. Checks if CRITICAL incidents have been unclaimed too long
 *   3. If so — marks them ESCALATED, sends SIGUSR1 to the server's
 *      main thread (signal demonstration), and pushes an EscalationAlert
 *      to the POSIX message queue (consumed by control room client)
 *   4. Updates shared memory escalation counter
 *
 * Signals used:
 *   SIGUSR1 — sent from watchdog thread to main thread when escalation
 *             occurs. In a real system this would page an on-call operator.
 *             Here it demonstrates signal-based inter-thread notification.
 *
 * This adds THREE OS concepts in one module:
 *   - Dedicated thread (concurrency)
 *   - POSIX signals (IPC mechanism)
 *   - Message queue (IPC mechanism)
 */

static pthread_t watchdog_tid;
static volatile int watchdog_running = 0;
static pid_t main_pid;
static volatile sig_atomic_t escalation_occurred = 0;

// ─── Signal handler registered in main ────────────────────────────────────
void escalation_signal_handler(int sig){
    (void)sig;
    escalation_occurred = 1;
    // No write() here - it's not fully async-safe
}

// ─── Watchdog thread function ──────────────────────────────────────────────
static void *watchdog_thread(void *arg) {
    (void)arg;
    LOG("Watchdog thread started (interval=%ds)", WATCHDOG_INTERVAL_SEC);

    while (watchdog_running) {
        sleep(WATCHDOG_INTERVAL_SEC);
        if (!watchdog_running) break;

        time_t now = time(NULL);
        int escalated_count = 0;

        // Lock the registry for scanning
        pthread_mutex_lock(&registry_mutex);

        for (int i = 0; i < incident_count; i++) {
            Incident *inc = &registry[i];

            // Only check active, unclaimed, pending incidents
            if (!inc->active) continue;
            if (inc->status != STATUS_PENDING) continue;
            if (strlen(inc->claimed_by) > 0) continue;
            if (inc->escalated) continue;

            static time_t server_start_time = 0;
            if (server_start_time == 0)
            {
                server_start_time = time(NULL);
            }

            double age_sec = difftime(now, inc->timestamp);

            if (server_start_time > 0 && inc->timestamp < server_start_time - 300)
            {
                continue; // Skip old incidents from previous sessions
            }

            
            int should_escalate = 0;

            if (inc->priority == PRIORITY_CRITICAL && age_sec > CRITICAL_TIMEOUT_SEC)
                should_escalate = 1;
            else if (inc->priority == PRIORITY_HIGH && age_sec > HIGH_TIMEOUT_SEC)
                should_escalate = 1;

            if (should_escalate) {
                inc->status    = STATUS_ESCALATED;
                inc->escalated = 1;
                escalated_count++;

                LOG("WATCHDOG: Escalating incident #%d (zone %d, %s, age=%.0fs)",
                    inc->id, inc->zone, priority_to_str(inc->priority), age_sec);

                // Build escalation alert for message queue
                EscalationAlert alert;
                memset(&alert, 0, sizeof(alert));
                alert.incident_id   = inc->id;
                alert.zone          = inc->zone;
                alert.priority      = inc->priority;
                alert.pending_since = inc->timestamp;
                strncpy(alert.description, inc->description, MAX_STR - 1);

                // Must unlock before calling external functions
                // (to avoid holding mutex during mq_send which could block)
                pthread_mutex_unlock(&registry_mutex);

                // Push to message queue → control room clients will see it
                mq_alerts_send(&alert);

                // Update shared memory counter
                shm_record_escalated();

                // Send SIGUSR1 to main process — signals the operator
                kill(main_pid, SIGUSR1);

                // Re-lock for next iteration
                pthread_mutex_lock(&registry_mutex);
            }
        }

        pthread_mutex_unlock(&registry_mutex);

        if (escalated_count > 0) {
            LOG("Watchdog scan complete: %d incident(s) escalated", escalated_count);
        } else {
            LOG("Watchdog scan complete: no escalations needed");
        }
    }

    LOG("Watchdog thread stopped");
    return NULL;
}

// ─── Public API ────────────────────────────────────────────────────────────
void watchdog_start(void) {
    main_pid = getpid();

    // Register SIGUSR1 handler in main process
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = escalation_signal_handler;
    sigaction(SIGUSR1, &sa, NULL);

    watchdog_running = 1;
    if (pthread_create(&watchdog_tid, NULL, watchdog_thread, NULL) != 0) {
        ERR("Failed to start watchdog thread");
        watchdog_running = 0;
    }
}

void watchdog_stop(void) {
    watchdog_running = 0;
    pthread_join(watchdog_tid, NULL);
}
