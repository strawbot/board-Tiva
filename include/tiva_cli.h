#ifndef TIVA_CLI_H_
#define TIVA_CLI_H_

// tiva_cli.h — TIVA (TM4C123GH6PM) board CLI command declarations.
// All functions are void(void) — bound to CLI text commands via wordlist.c.

// system
void show_sys(void);      // SYSCLK frequency, uptime ticks, timer state
void show_timers(void);   // TM4C TIMER0–TIMER5 survey: clock gate, mode, PSC, ILR, TV
void do_reboot(void);     // NVIC system reset
void do_dfu(void);        // enter ROM USB DFU bootloader — never returns; power-cycle to recover
void test_usb_isr(void);  // software-pend USB0 IRQ to verify vector table and IntEnable are correct
void show_cli(void);      // CLI status: dropped character count
void show_stack(void);    // stack high-water mark and overflow status

// gpio
void gpio_dump_all(void); // all GPIO pins: mode, PMC, OD, pull, data level, function

// network
void show_usb(void);        // USB CDC-ECM device state: enumeration, link, RX/TX ring
void debug_usb(void);       // raw hardware registers + ISR counters — use when show-usb is stuck
void usb_reconnect(void);   // drop D+ for 50 ms then reconnect — forces re-enumeration
void probe_usb_pins(void);  // read D+/D- digitally to detect open traces or swapped wires
void probe_vbus(void);      // test if VBUS comparator input is connected to PB1 (AMSEL toggle)
void vbus_poll(void);       // live VBUS + ISR monitor for 10 s — use during hot-plug test

#endif /* TIVA_CLI_H_ */
