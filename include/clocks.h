// clocks.h — TIVA clock and timer API for TimbreOS
//
// Mirrors the interface from TimbreOS/clocks.h. The board's project_defs.h
// defines CLOCK_HAS_* guards that determine which functions come from
// TimbreOS/clocks.c versus TIVA's own src/clocks.c.

#ifndef CLOCKS_H_
#define CLOCKS_H_

#include "ttypes.h"
#include "project_defs.h"
#include <time.h>

// Tick/real-time conversions: ticks are raw 80 MHz cycles (ONE_SECOND = 80 000 000).
// Simple integer divides — no multiply needed at this clock rate.
#define TO_US(ticks)  ((ticks) / 80UL)
#define TO_MS(ticks)  ((ticks) / 80000UL)

// Libc-free UTC date/time math (provided by TimbreOS/clocks.c)
Long timestamp_to_utc(const char *ts);
void epoch_to_tm(Long utc, struct tm *t);
Long tm_to_epoch(const struct tm *t);

// Clock API
Long get_ticks(void);
void set_delta_alarm(Long t);
void delta_alarm(void);
void over_due(void);
void micro_sleep(void);
void init_clocks(void);
void show_timer(void);
void print_build_banner(void);

#endif /* CLOCKS_H_ */
