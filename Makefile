CC      = gcc
CFLAGS  = -Wall -Wextra -g -pthread -I.
LDFLAGS = -pthread -lrt 

SERVER_SRCS = server/main.c server/auth.c server/incident.c \
              server/file_manager.c server/ipc_pipe.c \
              server/shm_stats.c server/mq_alerts.c server/watchdog.c

all: server/server client/commuter_client client/officer_client \
     client/control_client analytics/analytics
	@echo ""
	@echo "=== Build Complete — v2 ==="
	@echo "New features: Priority | Auto-Escalation | Shared Memory | Crash Recovery"
	@echo ""
	@echo "Accounts: admin/admin123 | officer1/pass1 | ravi/ravi123"
	@echo "New REPORT format: REPORT <zone> LOW|MEDIUM|HIGH|CRITICAL <description>"
	@echo "New commands: STATS | ALERTS"

server/server: $(SERVER_SRCS) server/auth.h server/incident.h \
               server/file_manager.h server/ipc_pipe.h \
               server/shm_stats.h server/mq_alerts.h server/watchdog.h common.h
	$(CC) $(CFLAGS) $(SERVER_SRCS) -o server/server $(LDFLAGS)
	@echo "Built: server/server"

client/commuter_client: client/commuter_client.c common.h
	$(CC) $(CFLAGS) client/commuter_client.c -o client/commuter_client
	@echo "Built: client/commuter_client"

client/officer_client: client/officer_client.c common.h
	$(CC) $(CFLAGS) client/officer_client.c -o client/officer_client
	@echo "Built: client/officer_client"

client/control_client: client/control_client.c server/mq_alerts.c common.h
	$(CC) $(CFLAGS) client/control_client.c server/mq_alerts.c \
	      -o client/control_client $(LDFLAGS)
	@echo "Built: client/control_client"

analytics/analytics: analytics/analytics.c common.h
	$(CC) $(CFLAGS) analytics/analytics.c -o analytics/analytics $(LDFLAGS)
	@echo "Built: analytics/analytics"

clean:
	rm -f server/server client/commuter_client client/officer_client \
	      client/control_client analytics/analytics
	rm -f /tmp/traffic_analytics_pipe
	@echo "Cleaned"
