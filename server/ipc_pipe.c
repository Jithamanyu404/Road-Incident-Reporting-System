#include "ipc_pipe.h"
#include "../common.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

static int pipe_fd = -1;
static pthread_mutex_t pipe_mutex = PTHREAD_MUTEX_INITIALIZER;

void pipe_init(void) {
    if (mkfifo(PIPE_PATH, 0666) == -1 && errno != EEXIST)
        ERR("mkfifo: %s", strerror(errno));
    pipe_fd = open(PIPE_PATH, O_WRONLY | O_NONBLOCK);
    if (pipe_fd < 0) ERR("Analytics not connected — start analytics first");
    else LOG("IPC pipe connected: %s", PIPE_PATH);
}

void pipe_push_event(const char *event_type, int zone, int incident_id) {
    if (pipe_fd < 0) {
        pipe_fd = open(PIPE_PATH, O_WRONLY | O_NONBLOCK);
        if (pipe_fd < 0) return;
    }
    char msg[128];
    int len = snprintf(msg, sizeof(msg), "%s,%d,%d,%ld\n",
                       event_type, zone, incident_id, (long)time(NULL));
    pthread_mutex_lock(&pipe_mutex);
    if (write(pipe_fd, msg, len) < 0) { close(pipe_fd); pipe_fd = -1; }
    pthread_mutex_unlock(&pipe_mutex);
}

void pipe_close(void) {
    if (pipe_fd >= 0) { close(pipe_fd); pipe_fd = -1; }
    pthread_mutex_destroy(&pipe_mutex);
}
