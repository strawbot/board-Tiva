#ifndef TICK_TIMER_H
#define TICK_TIMER_H

#include <stdint.h>
#include <stdbool.h>

/* Initialise Timer0A as a 1 ms free-running tick source.
   Call once after SysCtlClockSet(), before enabling interrupts globally. */
void tick_timer_init(void);

/* Returns the current millisecond tick count.
   The counter is 32-bit and rolls over every ~49 days. */
uint32_t tick_ms(void);

/* Convert milliseconds to tick units (1:1, but documents intent).
   Always use unsigned subtraction for rollover-safe elapsed checks:
       uint32_t start = tick_ms();
       if (tick_ms() - start >= msec(500)) { ... }   // safe across rollover */
#define msec(ms)  ((uint32_t)(ms))

#endif /* TICK_TIMER_H */
