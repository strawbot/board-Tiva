// clocks.h — TIVA clock and timer API for TimbreOS
//
// Mirrors the interface expected by tea.h (which #includes "clocks.h")
// and the Nano Board/clocks.h pattern.

#ifndef CLOCKS_H_
#define CLOCKS_H_

#include "ttypes.h"
#include "project_defs.h"

// Convert scheduler ticks to real time (ONE_SECOND = 1000 → 1 tick = 1 ms)
#define TO_US(ticks)  ((Octet)(ticks) * 1000000uLL / ONE_SECOND)
#define TO_MS(ticks)  ((Octet)(ticks) * 1000uLL    / ONE_SECOND)

// Called once by init_tea() before the scheduler starts.
void init_clocks(void);

// Print the firmware build timestamp. Call AFTER uart_transport_init()
// so the emit queue has a drain path.
void print_build_banner(void);

// Diagnostic: print uptime and tick info to CLI.
void show_timer(void);

// get_ticks(), set_delta_alarm(), over_due() declared in project_defs.h.

#endif /* CLOCKS_H_ */
