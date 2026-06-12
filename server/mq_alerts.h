#ifndef MQ_ALERTS_H
#define MQ_ALERTS_H
#include "../common.h"
#include <mqueue.h>

void mq_alerts_init_server(void);
void mq_alerts_send(const EscalationAlert *alert);
void mq_alerts_close_server(void);

// Client side — called by control room client
void mq_alerts_init_client(void);
int  mq_alerts_receive(EscalationAlert *out);  // non-blocking, returns 1 if got one
void mq_alerts_close_client(void);

#endif
