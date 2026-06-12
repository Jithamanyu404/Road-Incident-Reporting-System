#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H
#include "../common.h"
#include "incident.h"
void file_write_incident(const Incident *inc);
void file_read_zone(int zone, char *buf, size_t buf_size);
void file_manager_init(void);
#endif
