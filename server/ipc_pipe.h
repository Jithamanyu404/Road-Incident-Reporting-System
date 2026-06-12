#ifndef IPC_PIPE_H
#define IPC_PIPE_H
void pipe_init(void);
void pipe_push_event(const char *event_type, int zone, int incident_id);
void pipe_close(void);
#endif
