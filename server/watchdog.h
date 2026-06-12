#ifndef WATCHDOG_H
#define WATCHDOG_H
#include "../common.h"

// Starts the watchdog thread — runs inside the server process
void watchdog_start(void);
void watchdog_stop(void);

#endif
