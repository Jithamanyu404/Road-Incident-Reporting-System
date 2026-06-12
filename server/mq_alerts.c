#include "mq_alerts.h"
#include <errno.h>
#include <fcntl.h>

/*
 * mq_alerts.c — POSIX Message Queue (IPC Concept 4.6 — third mechanism)
 *
 * The watchdog thread sends escalation alerts to the control room
 * via a POSIX message queue. This is different from the named pipe:
 *
 *   Named pipe  → raw byte stream, order preserved, blocking reads
 *   Msg queue   → discrete messages with priority, non-blocking reads,
 *                 messages persist in the queue until consumed
 *
 * The control room client calls mq_alerts_receive() on every loop
 * iteration (non-blocking) and displays any pending alerts immediately.
 *
 * Why message queue here instead of pipe?
 *   Escalation alerts are discrete, structured messages. The queue
 *   guarantees each alert is delivered as a complete atomic unit —
 *   no partial reads, no framing needed. It also supports message
 *   priority (CRITICAL alerts could be prioritized over HIGH).
 */

static mqd_t server_mq = (mqd_t)-1;
static mqd_t client_mq = (mqd_t)-1;

static struct mq_attr mq_attributes = {
    .mq_flags   = 0,
    .mq_maxmsg  = MQ_MAX_MSG,
    .mq_msgsize = sizeof(EscalationAlert),
    .mq_curmsgs = 0
};

// ── Server side: create + write ───────────────────────────────────────────
void mq_alerts_init_server(void) {
    // Unlink first in case of stale queue from previous crash
    mq_unlink(MQ_NAME);

    server_mq = mq_open(MQ_NAME,
                        O_CREAT | O_WRONLY | O_NONBLOCK,
                        0666,
                        &mq_attributes);
    if (server_mq == (mqd_t)-1) {
        ERR("mq_open (server) failed: %s", strerror(errno));
    } else {
        LOG("Message queue created: %s", MQ_NAME);
    }
}

void mq_alerts_send(const EscalationAlert *alert) {
    if (server_mq == (mqd_t)-1) return;

    // Priority = incident priority (CRITICAL=3 delivered before HIGH=2)
    unsigned int prio = (unsigned int)alert->priority;

    int r = mq_send(server_mq, (const char *)alert,
                    sizeof(EscalationAlert), prio);
    if (r < 0 && errno != EAGAIN) {
        ERR("mq_send failed: %s", strerror(errno));
    }
}

void mq_alerts_close_server(void) {
    if (server_mq != (mqd_t)-1) mq_close(server_mq);
    mq_unlink(MQ_NAME);
}

// ── Client side: open + non-blocking read ─────────────────────────────────
void mq_alerts_init_client(void) {
    client_mq = mq_open(MQ_NAME, O_RDONLY | O_NONBLOCK);
    if (client_mq == (mqd_t)-1) {
        // Queue may not exist yet — not an error
    }
}

// Returns 1 if alert received, 0 if queue empty, -1 on error
int mq_alerts_receive(EscalationAlert *out) {
    if (client_mq == (mqd_t)-1) {
        // Try to connect if not yet open
        client_mq = mq_open(MQ_NAME, O_RDONLY | O_NONBLOCK);
        if (client_mq == (mqd_t)-1) return 0;
    }
    unsigned int prio;
    ssize_t n = mq_receive(client_mq, (char *)out,
                           sizeof(EscalationAlert), &prio);
    if (n == sizeof(EscalationAlert)) return 1;
    if (errno == EAGAIN) return 0;   // Queue empty — normal
    return -1;
}

void mq_alerts_close_client(void) {
    if (client_mq != (mqd_t)-1) mq_close(client_mq);
}
