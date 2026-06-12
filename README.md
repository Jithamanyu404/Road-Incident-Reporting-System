# Real-Time Collaborative Traffic & Road Incident Reporting System
## EGC 301P Operating Systems Lab — Mini Project

---

## Project Overview

A multi-user, multi-process networked application where traffic police
officers, commuters, and a city control room coordinate road incident
reporting and response in real time.

---

## OS Concepts Implemented

| Concept | Where | Mechanism |
|---------|-------|-----------|
| 4.1 Role-Based Auth | server/auth.c | 3 roles: control_room, officer, commuter |
| 4.2 File Locking | server/file_manager.c | fcntl F_WRLCK / F_RDLCK |
| 4.3 Concurrency Control | server/incident.c | pthread_mutex + POSIX semaphore |
| 4.4 Data Consistency | server/incident.c | Atomic check+write, state machine |
| 4.5 Socket Programming | server/main.c + clients | TCP client-server, one thread/client |
| 4.6 IPC | server/ipc_pipe.c + analytics/ | Named pipe (FIFO) |

---

## File Structure

```
traffic_system/
├── common.h                  # Shared types, constants, protocol
├── Makefile
│
├── server/
│   ├── main.c                # TCP server, thread spawning
│   ├── auth.c / auth.h       # Role-based authentication
│   ├── incident.c / .h       # Registry with mutex + semaphore
│   ├── file_manager.c / .h   # fcntl file locking
│   └── ipc_pipe.c / .h       # Named pipe writer
│
├── client/
│   ├── commuter_client.c     # Commuter terminal
│   ├── officer_client.c      # Officer terminal
│   └── control_client.c      # Control room terminal
│
├── analytics/
│   └── analytics.c           # Separate process — named pipe reader
│
└── data/                     # Auto-created on first run
    ├── users.txt
    └── zone_N_incidents.log
```

---

## Build

```bash
cd traffic_system
make all
```

Requirements: gcc, Linux (fcntl + POSIX threads + named pipes)

---

## Running the System

Open **5 terminals** in the traffic_system directory:

**Terminal 1 — Analytics (start first!)**
```bash
./analytics/analytics
```

**Terminal 2 — Server**
```bash
./server/server
```

**Terminal 3 — Commuter**
```bash
./client/commuter_client
# Login: ravi / ravi123
```

**Terminal 4 — Officer**
```bash
./client/officer_client
# Login: officer1 / pass1  (Zone 1)
```

**Terminal 5 — Control Room**
```bash
./client/control_client
# Login: admin / admin123
```

---

## Default User Accounts

| Username | Password | Role | Zone |
|----------|----------|------|------|
| admin | admin123 | control_room | all |
| officer1 | pass1 | officer | 1 |
| officer2 | pass2 | officer | 2 |
| officer3 | pass3 | officer | 3 |
| ravi | ravi123 | commuter | any |
| priya | priya123 | commuter | any |
| arjun | arjun123 | commuter | any |

---

## Demo Script (For Evaluation)

```
1. [Commuter Terminal]
   REPORT 1 Accident near MG Road signal
   → Server: OK Incident #1 created in zone 1

2. [Officer1 Terminal — zone 1]
   LIST
   → Shows Incident #1 as PENDING

   CLAIM 1
   → Server: OK Incident claimed successfully

3. [Commuter Terminal — try to claim same incident]
   (Cannot — DENIED: commuter role has no CLAIM permission)

4. [Officer1 Terminal]
   UPDATE 1 INVESTIGATING
   UPDATE 1 RESOLVED

5. [Control Room Terminal]
   LISTALL
   → Shows all zones

   REPORT 2 Pothole on Hosur Road
   CLOSE 2
   → Force-closes from control room

6. [Analytics Terminal]
   → Live dashboard updates with each event
   → Shows per-zone statistics in real time

7. RACE CONDITION DEMO:
   Open two officer terminals (officer1 and officer2 — same zone workaround)
   Both try: CLAIM 1 simultaneously
   → Only one succeeds. Other gets: "FAIL Incident already claimed"
   → Demonstrates mutex protecting against double-claim race condition
```

---