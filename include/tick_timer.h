// tick_timer.h — RETIRED
//
// Timer0A and the 1 ms tick have moved to src/clocks.c (TimbreOS port).
// Replacements:
//   tick_timer_init() → called internally by init_tea() via init_clocks()
//   tick_ms()         → getTime()   (TimbreOS/timestamp.h)
//   msec(n)           → msec(n)     (tea.h, scaled by ONE_SECOND)
//
// This file is intentionally empty so existing #includes do not break.
