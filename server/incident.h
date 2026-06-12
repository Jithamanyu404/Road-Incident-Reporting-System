#ifndef INCIDENT_H
#define INCIDENT_H
#include "../common.h"
#include <pthread.h>
#include <semaphore.h>

extern Incident        registry[MAX_INCIDENTS];
extern int             incident_count;
extern pthread_mutex_t registry_mutex;
extern sem_t           file_write_sem;

void incidents_init(void);
void incidents_destroy(void);
void incidents_load_from_logs(void);   // NEW: crash recovery

int  incident_add(const char *desc, int zone, const char *reporter, Priority priority);
int  incident_claim(int id, const char *officer, int officer_zone);
int  incident_update_status(int id, IncidentStatus new_status, const char *by_user, UserRole role);
int  incident_close(int id, const char *by_user);
int  incident_list_zone(int zone, char *buf, size_t buf_size);
int  incident_list_all(char *buf, size_t buf_size);
void incident_get_stats(char *buf, size_t buf_size);
void incidents_rebuild_shm_stats(void);

#endif
