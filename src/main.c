// main.c — TIVA (TM4C123GH6PM) TimbreOS entry point
//
// Startup sequence (mirrors Nano/src/hal_entry.c):
//
//   1. SysCtlClockSet()      — 80 MHz from PLL, 16 MHz crystal
//   2. init_tea()            — TimbreOS scheduler; calls init_clocks()
//                              internally, which starts Timer0A (1 ms tick),
//                              Timer1A (delta alarm), heartbeat LED, and
//                              enables interrupts.
//   3. uart_transport_init() — UART2 on PD6/PD7 @ 115 200, uDMA TX
//   4. print_build_banner()  — safe to print now that the transport is up
//   5. init_cli()            — TimbreOS Forth-like CLI
//   6. run() loop            — cooperative scheduler; never returns
//
// The heartbeat LED (PC4) is kicked off inside init_clocks() via later()
// and thereafter self-reschedules through in() — no polling required here.

#include <stdint.h>
#include <stdbool.h>

#include "inc/hw_memmap.h"
#include "driverlib/sysctl.h"

#include "tea.h"
#include "cli.h"
#include "clocks.h"
#include "cli_transport_uart.h"

// init_cli is defined in TimbreOS/cli.c but not declared in cli.h.
void init_cli(void);

int main(void) {
    // 80 MHz from PLL, 16 MHz external crystal.
    SysCtlClockSet(SYSCTL_SYSDIV_2_5 | SYSCTL_USE_PLL |
                   SYSCTL_OSC_MAIN   | SYSCTL_XTAL_16MHZ);

    // TimbreOS: start scheduler, timers, heartbeat, interrupts.
    init_tea();

    // UART2 transport for CLI (init_tea / init_clocks run before this,
    // so do NOT print before uart_transport_init — emitq has no drain yet).
    uart_transport_init();

    // Safe to print now.
    print_build_banner();

    // Forth-like CLI.
    init_cli();

    // Cooperative scheduler — never returns.
    while (1) {
        run();
    }
}
