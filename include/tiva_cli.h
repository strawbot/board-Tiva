#ifndef TIVA_CLI_H_
#define TIVA_CLI_H_

// tiva_cli.h — TIVA (TM4C123GH6PM) board CLI command declarations.
// All functions are void(void) — bound to CLI text commands via wordlist.c.

// system
void show_sys(void);      // SYSCLK frequency, uptime ticks, timer state
void show_timers(void);   // TM4C TIMER0–TIMER5 survey: clock gate, mode, PSC, ILR, TV
void do_reboot(void);     // NVIC system reset
void show_cli(void);      // CLI status: dropped character count
void show_stack(void);    // stack high-water mark and overflow status

// gpio
void gpio_dump_all(void); // all GPIO pins: mode, PMC, OD, pull, data level, function

#endif /* TIVA_CLI_H_ */
