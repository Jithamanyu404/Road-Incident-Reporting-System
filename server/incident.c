#include "incident.h"
#include "file_manager.h"
#include "ipc_pipe.h"
#include "shm_stats.h"

/*
 * incident.c — Registry with priority sorting + crash recovery
 *
 * New in v2:
 *   - Priority field: CRITICAL incidents always listed first
 *   - crash recovery: incidents_load_from_logs() rebuilds registry on restart
 *   - Response time tracking: resolved_at - claimed_at
 *   - Shared memory updates on every state change
 */

Incident        registry[MAX_INCIDENTS];
int             incident_count = 0;
pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t           file_write_sem;
static int      next_id = 1;

void incidents_init(void) {
    memset(registry, 0, sizeof(registry));
    incident_count = 0;
    next_id = 1;
    sem_init(&file_write_sem, 0, 3);
    LOG("Incident registry initialized");
}

void incidents_destroy(void) {
    sem_destroy(&file_write_sem);
    pthread_mutex_destroy(&registry_mutex);
}

/*
 * incidents_load_from_logs — Crash Recovery (Feature 4)
 *
 * Reads all zone log files and rebuilds the in-memory registry.
 * On each restart the server picks up where it left off —
 * no incidents are lost. This is a simplified version of
 * Write-Ahead Log (WAL) recovery used in real databases.
 *
 * Format parsed: "[TIMESTAMP] ID:N | Zone:N | STATUS | DESC | Reporter:X | Officer:Y"
 * We only keep the LATEST entry per incident ID (last write wins).
 */
void incidents_load_from_logs(void) {
    int loaded = 0;
    for (int z = 1; z <= MAX_ZONES; z++) {
        char fname[64];
        snprintf(fname, sizeof(fname), "data/zone_%d_incidents.log", z);
        FILE *f = fopen(fname, "r");
        if (!f) continue;

        char line[512];
        while (fgets(line, sizeof(line), f)) {
            int id = 0, zone = 0;
            char status_s[32] = {0}, desc[MAX_STR] = {0};
            char reporter[MAX_USER_LEN] = {0}, officer[MAX_USER_LEN] = {0};
            char priority_s[16] = "LOW";

            // Parse log line using simple token extraction
            // Format: [TIMESTAMP] ID:N | Zone:N | STATUS | PRIORITY | DESC | Reporter:X | Officer:Y
            char *p = line;
            // Skip timestamp [...]
            if (*p == '[') { while (*p && *p != ']') p++; if (*p) p++; }
            while (*p == ' ') p++;

            // Extract ID:
            if (sscanf(p, "ID:%d", &id) != 1 || id <= 0) continue;

            // Extract Zone:
            char *zp = strstr(p, "Zone:");
            if (!zp || sscanf(zp, "Zone:%d", &zone) != 1) continue;

            // Extract status (first field after Zone)
            char *f1 = strchr(zp, '|'); if (!f1) continue; f1++;
            while (*f1 == ' ') f1++;
            sscanf(f1, "%31[^| \n]", status_s);

            // Extract priority (second field)
            char *f2 = strchr(f1, '|'); if (!f2) continue; f2++;
            while (*f2 == ' ') f2++;
            sscanf(f2, "%15[^| \n]", priority_s);

            // Extract description (third field)
            char *f3 = strchr(f2, '|'); if (!f3) continue; f3++;
            while (*f3 == ' ') f3++;
            sscanf(f3, "%255[^|\n]", desc);
            // Trim trailing spaces from desc
            { char *e=desc+strlen(desc)-1; while(e>desc && *e==' ') *e--='\0'; }

            // Extract Reporter:
            char *rp = strstr(p, "Reporter:"); 
            if (rp) sscanf(rp + 9, "%49[^| \n]", reporter);

            // Extract Officer:
            char *op = strstr(p, "Officer:");
            if (op) sscanf(op + 8, "%49[^ \n]", officer);

            int parsed = (id > 0 && zone > 0) ? 7 : 0;
            if (parsed < 2) continue;

            // Trim trailing spaces
            char *end;
            end = status_s   + strlen(status_s)   - 1; while(end>status_s   && *end==' ') *end--='\0';
            end = desc       + strlen(desc)       - 1; while(end>desc       && *end==' ') *end--='\0';
            end = reporter   + strlen(reporter)   - 1; while(end>reporter   && *end==' ') *end--='\0';
            end = officer    + strlen(officer)    - 1; while(end>officer    && *end==' ') *end--='\0';

            // Find or create registry slot for this ID
            Incident *slot = NULL;
            for (int i = 0; i < incident_count; i++) {
                if (registry[i].id == id) { slot = &registry[i]; break; }
            }
            if (!slot) {
                if (incident_count >= MAX_INCIDENTS) continue;
                slot = &registry[incident_count++];
                memset(slot, 0, sizeof(Incident));
                slot->id   = id;
                slot->zone = zone;
                if (id >= next_id) next_id = id + 1;
                loaded++;
            }

            slot->active = 1;
            strncpy(slot->description, desc,     MAX_STR - 1);
            strncpy(slot->reported_by, reporter, MAX_USER_LEN - 1);
            if (strcmp(officer, "none") != 0 && strlen(officer) > 0)
                strncpy(slot->claimed_by, officer, MAX_USER_LEN - 1);

            // Parse status
            if      (strstr(status_s, "RESOLVED")      ) slot->status = STATUS_RESOLVED;
            else if (strstr(status_s, "INVESTIGATING")  ) slot->status = STATUS_INVESTIGATING;
            else if (strstr(status_s, "ESCALATED")      ) slot->status = STATUS_ESCALATED;
            else                                           slot->status = STATUS_PENDING;

            slot->priority = str_to_priority(priority_s);
        }
        fclose(f);
    }
    if (loaded > 0)
        LOG("Crash recovery: loaded %d incidents from logs (next_id=%d)", loaded, next_id);
}

int incident_add(const char *desc, int zone, const char *reporter, Priority priority) {
    if (zone < 1 || zone > MAX_ZONES) return -1;

    pthread_mutex_lock(&registry_mutex);
    if (incident_count >= MAX_INCIDENTS) {
        pthread_mutex_unlock(&registry_mutex);
        return -1;
    }
    Incident *inc = &registry[incident_count];
    memset(inc, 0, sizeof(Incident));
    inc->id        = next_id++;
    inc->zone      = zone;
    inc->status    = STATUS_PENDING;
    inc->priority  = priority;
    inc->timestamp = time(NULL);
    inc->active    = 1;
    strncpy(inc->description, desc,     MAX_STR - 1);
    strncpy(inc->reported_by, reporter, MAX_USER_LEN - 1);
    int new_id = inc->id;
    incident_count++;
    pthread_mutex_unlock(&registry_mutex);

    file_write_incident(inc);
    pipe_push_event("CREATED", zone, new_id);
    shm_record_created(priority, zone);

    LOG("Incident #%d [%s] created zone=%d by %s", new_id, priority_to_str(priority), zone, reporter);
    return new_id;
}

int incident_claim(int id, const char *officer, int officer_zone) {
    pthread_mutex_lock(&registry_mutex);
    for (int i = 0; i < incident_count; i++) {
        if (registry[i].id == id && registry[i].active) {
            if (officer_zone != -1 && registry[i].zone != officer_zone) {
                pthread_mutex_unlock(&registry_mutex);
                return -3;
            }
            if (strlen(registry[i].claimed_by) > 0) {
                pthread_mutex_unlock(&registry_mutex);
                return -1;
            }
            strncpy(registry[i].claimed_by, officer, MAX_USER_LEN - 1);
            registry[i].status    = STATUS_INVESTIGATING;
            registry[i].claimed_at = time(NULL);
            registry[i].escalated  = 0;  // Reset if escalated
            int zone = registry[i].zone;
            Priority p = registry[i].priority;
            pthread_mutex_unlock(&registry_mutex);
            file_write_incident(&registry[i]);
            pipe_push_event("CLAIMED", zone, id);
            LOG("Incident #%d [%s] claimed by %s", id, priority_to_str(p), officer);
            return 0;
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return -2;
}

int incident_update_status(int id, IncidentStatus new_status, const char *by_user, UserRole role) {
    pthread_mutex_lock(&registry_mutex);
    for (int i = 0; i < incident_count; i++) {
        if (registry[i].id == id && registry[i].active) {
            IncidentStatus cur = registry[i].status;
            int valid = 0;
            if (cur == STATUS_PENDING        && new_status == STATUS_INVESTIGATING) valid = 1;
            if (cur == STATUS_INVESTIGATING  && new_status == STATUS_RESOLVED)      valid = 1;
            if (cur == STATUS_ESCALATED      && new_status == STATUS_INVESTIGATING) valid = 1;
            if (role == ROLE_CONTROL_ROOM) valid = 1;
            if (!valid) { pthread_mutex_unlock(&registry_mutex); return -1; }
            if (role == ROLE_OFFICER && strcmp(registry[i].claimed_by, by_user) != 0) {
                pthread_mutex_unlock(&registry_mutex); return -3;
            }
            registry[i].status = new_status;
            if (new_status == STATUS_RESOLVED) {
                registry[i].resolved_at = time(NULL);
            }
            Priority p  = registry[i].priority;
            int      zone = registry[i].zone;
            double   resp = 0;
            if (new_status == STATUS_RESOLVED && registry[i].claimed_at > 0)
                resp = difftime(registry[i].resolved_at, registry[i].claimed_at);
            pthread_mutex_unlock(&registry_mutex);
            file_write_incident(&registry[i]);
            if (new_status == STATUS_RESOLVED) {
                pipe_push_event("RESOLVED", zone, id);
                shm_record_resolved(p, zone, resp);
            }
            LOG("Incident #%d → %s by %s", id, status_to_str(new_status), by_user);
            return 0;
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return -2;
}

int incident_close(int id, const char *by_user) {
    pthread_mutex_lock(&registry_mutex);
    for (int i = 0; i < incident_count; i++) {
        if (registry[i].id == id && registry[i].active) {
            registry[i].status     = STATUS_RESOLVED;
            registry[i].active     = 0;
            registry[i].resolved_at = time(NULL);
            int zone = registry[i].zone;
            Priority p = registry[i].priority;
            double resp = 0;
            if (registry[i].claimed_at > 0)
                resp = difftime(registry[i].resolved_at, registry[i].claimed_at);
            pthread_mutex_unlock(&registry_mutex);
            file_write_incident(&registry[i]);
            pipe_push_event("CLOSED", zone, id);
            shm_record_resolved(p, zone, resp);
            LOG("Incident #%d force-closed by %s", id, by_user);
            return 0;
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return -1;
}

// Priority-sorted list: CRITICAL first, then HIGH, MEDIUM, LOW
static int priority_cmp(const void *a, const void *b) {
    const Incident *ia = (const Incident *)a;
    const Incident *ib = (const Incident *)b;
    return (int)ib->priority - (int)ia->priority;  // Descending
}

int incident_list_zone(int zone, char *buf, size_t buf_size) {
    pthread_mutex_lock(&registry_mutex);

    // Collect matching incidents into temp array for sorting
    Incident temp[MAX_INCIDENTS];
    int count = 0;
    for (int i = 0; i < incident_count; i++) {
        if (registry[i].active && registry[i].zone == zone)
            temp[count++] = registry[i];
    }
    pthread_mutex_unlock(&registry_mutex);

    // Sort by priority (CRITICAL first)
    qsort(temp, count, sizeof(Incident), priority_cmp);

    size_t offset = 0;
    buf[0] = '\0';
    for (int i = 0; i < count; i++) {
        char ts[32], line[600];
        struct tm *tm_info = localtime(&temp[i].timestamp);
        strftime(ts, sizeof(ts), "%H:%M", tm_info);
        const char *esc = temp[i].escalated ? " [ESCALATED]" : "";
        snprintf(line, sizeof(line),
            "[%s] #%-4d | %-8s | %-13s | %-20s | By:%-12s | Clm:%-12s%s",
            ts, temp[i].id,
            priority_to_str(temp[i].priority),
            status_to_str(temp[i].status),
            temp[i].description,
            temp[i].reported_by,
            strlen(temp[i].claimed_by) ? temp[i].claimed_by : "(none)",
            esc);
        size_t len = strlen(line);
        if (offset + len + 2 < buf_size) {
            memcpy(buf + offset, line, len);
            offset += len;
            buf[offset++] = '\n';
        }
    }
    buf[offset] = '\0';
    return count;
}

int incident_list_all(char *buf, size_t buf_size) {
    pthread_mutex_lock(&registry_mutex);
    Incident temp[MAX_INCIDENTS];
    int count = 0;
    for (int i = 0; i < incident_count; i++)
        if (registry[i].active) temp[count++] = registry[i];
    pthread_mutex_unlock(&registry_mutex);

    qsort(temp, count, sizeof(Incident), priority_cmp);

    size_t offset = 0;
    buf[0] = '\0';
    for (int i = 0; i < count; i++) {
        char ts[32], line[600];
        struct tm *tm_info = localtime(&temp[i].timestamp);
        strftime(ts, sizeof(ts), "%H:%M", tm_info);
        const char *esc = temp[i].escalated ? " [ESCALATED]" : "";
        snprintf(line, sizeof(line),
            "[%s] #%-4d | Zn:%-2d | %-8s | %-13s | %-20s | By:%-10s | Clm:%-10s%s",
            ts, temp[i].id, temp[i].zone,
            priority_to_str(temp[i].priority),
            status_to_str(temp[i].status),
            temp[i].description,
            temp[i].reported_by,
            strlen(temp[i].claimed_by) ? temp[i].claimed_by : "(none)",
            esc);
        size_t len = strlen(line);
        if (offset + len + 2 < buf_size) {
            memcpy(buf + offset, line, len);
            offset += len;
            buf[offset++] = '\n';
        }
    }
    buf[offset] = '\0';
    return count;
}

void incident_get_stats(char *buf, size_t buf_size) {
    pthread_mutex_lock(&registry_mutex);
    int total=0, pending=0, investigating=0, resolved=0, escalated=0;
    int by_p[4] = {0};
    for (int i = 0; i < incident_count; i++) {
        if (!registry[i].active && registry[i].status != STATUS_RESOLVED) continue;
        total++;
        switch (registry[i].status) {
            case STATUS_PENDING:       pending++;       break;
            case STATUS_INVESTIGATING: investigating++; break;
            case STATUS_RESOLVED:      resolved++;      break;
            case STATUS_ESCALATED:     escalated++;     break;
        }
        if (registry[i].priority <= PRIORITY_CRITICAL)
            by_p[registry[i].priority]++;
    }
    pthread_mutex_unlock(&registry_mutex);
    snprintf(buf, buf_size,
        "Total:%d | Pending:%d | Investigating:%d | Resolved:%d | Escalated:%d\n"
        "By Priority — CRITICAL:%d HIGH:%d MEDIUM:%d LOW:%d",
        total, pending, investigating, resolved, escalated,
        by_p[PRIORITY_CRITICAL], by_p[PRIORITY_HIGH],
        by_p[PRIORITY_MEDIUM],   by_p[PRIORITY_LOW]);
}


void incidents_rebuild_shm_stats(void)
{
    pthread_mutex_lock(&registry_mutex);

    // Reset shared memory stats
    SharedStats temp_stats = {0};

    for (int i = 0; i < incident_count; i++)
    {
        Incident *inc = &registry[i];
        if (!inc->active && inc->status != STATUS_RESOLVED)
            continue;

        temp_stats.total_created++;
        temp_stats.by_priority[inc->priority]++;

        if (inc->zone >= 1 && inc->zone <= MAX_ZONES)
            temp_stats.by_zone[inc->zone]++;

        if (inc->status == STATUS_RESOLVED)
        {
            temp_stats.total_resolved++;
            if (inc->claimed_at > 0 && inc->resolved_at > 0)
            {
                double resp = difftime(inc->resolved_at, inc->claimed_at);
                int n = temp_stats.response_samples;
                temp_stats.avg_response_sec =
                    (temp_stats.avg_response_sec * n + resp) / (n + 1);
                temp_stats.response_samples++;
            }
        }

        if (inc->status == STATUS_ESCALATED || inc->escalated)
        {
            temp_stats.total_escalated++;
        }
    }

    temp_stats.last_updated = time(NULL);

    // Write to shared memory
    if (shm)
    {
        pthread_mutex_lock(&shm->lock);
        memcpy(&shm->stats, &temp_stats, sizeof(SharedStats));
        pthread_mutex_unlock(&shm->lock);
    }

    pthread_mutex_unlock(&registry_mutex);
    LOG("Rebuilt shared memory stats: Created=%d, Resolved=%d, Escalated=%d",
        temp_stats.total_created, temp_stats.total_resolved, temp_stats.total_escalated);
}