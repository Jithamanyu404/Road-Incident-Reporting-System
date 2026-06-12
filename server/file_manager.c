#include "file_manager.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

/*
 * file_manager.c — File Locking (Concept 4.2)
 * v2: log format extended to include priority field for crash recovery parsing
 */

static void acquire_lock(int fd, short lock_type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = lock_type; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 0;
    if (fcntl(fd, F_SETLKW, &fl) == -1) ERR("fcntl lock: %s", strerror(errno));
}

static void release_lock(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK; fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_len = 0;
    fcntl(fd, F_SETLK, &fl);
}

static void zone_filename(int zone, char *buf, size_t size) {
    snprintf(buf, size, "data/zone_%d_incidents.log", zone);
}

void file_manager_init(void) {
    mkdir("data", 0755);
    for (int z = 1; z <= MAX_ZONES; z++) {
        char fname[64]; zone_filename(z, fname, sizeof(fname));
        int fd = open(fname, O_CREAT | O_APPEND | O_WRONLY, 0644);
        if (fd >= 0) close(fd);
    }
    FILE *uf = fopen("data/users.txt", "r");
    if (!uf) {
        uf = fopen("data/users.txt", "w");
        if (uf) {
            fprintf(uf, "admin    admin123  control_room -1\n");
            fprintf(uf, "officer1 pass1     officer       1\n");
            fprintf(uf, "officer2 pass2     officer       2\n");
            fprintf(uf, "officer3 pass3     officer       3\n");
            fprintf(uf, "ravi     ravi123   commuter     -1\n");
            fprintf(uf, "priya    priya123  commuter     -1\n");
            fclose(uf);
            LOG("Created default users.txt");
        }
    } else { fclose(uf); }
    LOG("File manager initialized");
}

void file_write_incident(const Incident *inc) {
    char fname[64];
    zone_filename(inc->zone, fname, sizeof(fname));

    sem_wait(&file_write_sem);     // Semaphore: limit concurrent file writers

    int fd = open(fname, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) { ERR("open %s: %s", fname, strerror(errno)); sem_post(&file_write_sem); return; }

    acquire_lock(fd, F_WRLCK);    // Exclusive write lock

    char ts[32];
    struct tm *tm_info = localtime(&inc->timestamp);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);

    char line[768];
    // Extended format: includes priority for crash recovery
    snprintf(line, sizeof(line),
        "[%s] ID:%-4d | Zone:%-2d | %-13s | %-8s | %-20s | Reporter:%-15s | Officer:%-15s\n",
        ts, inc->id, inc->zone,
        status_to_str(inc->status),
        priority_to_str(inc->priority),
        inc->description,
        inc->reported_by,
        strlen(inc->claimed_by) ? inc->claimed_by : "none");
    write(fd, line, strlen(line));

    release_lock(fd);
    close(fd);
    sem_post(&file_write_sem);     // Semaphore: release slot
}

void file_read_zone(int zone, char *buf, size_t buf_size) {
    char fname[64];
    zone_filename(zone, fname, sizeof(fname));
    int fd = open(fname, O_RDONLY);
    if (fd < 0) { snprintf(buf, buf_size, "(no log for zone %d)\n", zone); return; }
    acquire_lock(fd, F_RDLCK);    // Shared read lock
    ssize_t n = read(fd, buf, buf_size - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';
    release_lock(fd);
    close(fd);
}
