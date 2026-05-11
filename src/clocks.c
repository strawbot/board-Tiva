// clocks.c — TIVA (TM4C123GH6PM) board-specific clock code
//
// TimbreOS/clocks.c provides: timestamp_to_utc, epoch_to_tm, tm_to_epoch,
// over_due, micro_sleep, print_build_banner.
// CLOCK_HAS_BLINK/INIT/TICKS/DELTA/SHOW are defined in project_defs.h so
// this file owns all hardware clock and heartbeat functions.
//
// ── CLOCK ARCHITECTURE ────────────────────────────────────────────────────
//
// Timer0A — 32-bit free-running counter at system clock (80 MHz), no ISR.
//   Configured as periodic with load=0xFFFFFFFF, counting down.
//   get_ticks() = ~TAR — raw 80 MHz cycles, ONE_SECOND=80000000.
//   tea.c signed-int delta: INT_MAX cycles ≈ 26.8 s scheduling horizon;
//   the scheduler re-arms automatically for longer durations.
//
// Timer1A — 32-bit one-shot at system clock (80 MHz).
//   set_delta_alarm(t): t is already in 80 MHz cycles; load = t − 1.
//   ISR calls (*alarmEvent)() to dispatch due time events.
//   Maximum one-shot: DELTA_MAX_TICKS = INT32_MAX cycles (≈26.8 s). Longer
//   alarms are clamped; the scheduler re-arms automatically.
//
// DWT CYCCNT — 80 MHz cycle counter.
//   Provides sysTicks() for namedAction() execution-time profiling.
//
// ── HEARTBEAT LED ─────────────────────────────────────────────────────────
//
// PC4 — double-blink pattern, 1 Hz:
//   on(2 ms) · off(198 ms) · on(2 ms) · off(798 ms)

#include <stdint.h>
#include <stdbool.h>

#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "inc/hw_types.h"
#include "inc/hw_timer.h"
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"
#include "driverlib/interrupt.h"
#include "driverlib/gpio.h"

#include "tea.h"
#include "clocks.h"
#include "printers.h"

// ── Tick counter — Timer0A free-running ───────────────────────────────────
// Timer0A counts down from 0xFFFFFFFF at 80 MHz; inverting gives a raw
// up-counter in cycles.  No ISR, no division — ONE_SECOND=80000000.
// Guard against pre-init reads (init_tea calls get_ticks before init_clocks).

static bool timer0_ready = false;

Long get_ticks(void) {
    if (!timer0_ready) return 0;
    return (Long)(~HWREG(TIMER0_BASE + TIMER_O_TAR));
}

static Long uptime = 0;
uint32_t getUptime(void) { return (uint32_t)uptime; }

// ── Heartbeat LED (PC4) ───────────────────────────────────────────────────

#define HB_LED_PERIPH   SYSCTL_PERIPH_GPIOC
#define HB_LED_PORT     GPIO_PORTC_BASE
#define HB_LED_PIN      GPIO_PIN_4

static inline void hb_on (void) { GPIOPinWrite(HB_LED_PORT, HB_LED_PIN, HB_LED_PIN); }
static inline void hb_off(void) { GPIOPinWrite(HB_LED_PORT, HB_LED_PIN, 0); }

static void blink_leds(void) {
    Long t;
    static enum { HB1, GAP1, HB2, GAP2 } phase = GAP2;
    switch (phase) {
    case HB1:   hb_on();  phase = GAP1; t = msec(3);             break;
    case GAP1:  hb_off(); phase = HB2;  t = msec(197) - msec(3); break;
    case HB2:   hb_on();  phase = GAP2; t = msec(3);             break;
    default:
    case GAP2:  hb_off(); phase = HB1;  t = msec(797) - msec(3); uptime++; break;
    }
    in(t, blink_leds);
}

// ── Delta alarm — Timer1A one-shot ────────────────────────────────────────
// t is in raw 80 MHz cycles (ONE_SECOND = 80000000); load = t − 1.
// Clamped to INT32_MAX to match tea.c's signed-delta scheduling horizon.

#define DELTA_MAX_TICKS  2147483647UL

void set_delta_alarm(Long t) {
    if (t > DELTA_MAX_TICKS) t = DELTA_MAX_TICKS;
    if (t < 1)               t = 1;
    uint32_t load = (uint32_t)t - 1UL;
    TimerDisable(TIMER1_BASE, TIMER_A);
    TimerLoadSet(TIMER1_BASE, TIMER_A, load);
    TimerEnable (TIMER1_BASE, TIMER_A);
}

void Timer1AIntHandler(void) {
    TimerIntClear(TIMER1_BASE, TIMER_TIMA_TIMEOUT);
    now(*alarmEvent);
}

// ── DWT cycle counter ─────────────────────────────────────────────────────

#define DEM_CR_REG     (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL_REG   (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT_REG (*(volatile uint32_t *)0xE0001004u)

static void dwt_enable(void) {
    DEM_CR_REG    |= (1u << 24);
    DWT_CYCCNT_REG = 0;
    DWT_CTRL_REG  |= 1u;
}

// ── init_clocks ───────────────────────────────────────────────────────────

void init_clocks(void) {
    never(alarmEvent);
    dwt_enable();

    // Timer0A: free-running 80 MHz counter, no interrupt
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_TIMER0)) {}
    TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC);
    TimerLoadSet(TIMER0_BASE, TIMER_A, 0xFFFFFFFFUL);
    TimerEnable(TIMER0_BASE, TIMER_A);
    timer0_ready = true;

    // Timer1A: one-shot delta alarm (started on demand by set_delta_alarm)
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER1);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_TIMER1)) {}
    TimerConfigure(TIMER1_BASE, TIMER_CFG_ONE_SHOT);
    IntEnable(INT_TIMER1A);
    TimerIntEnable(TIMER1_BASE, TIMER_TIMA_TIMEOUT);

    // Heartbeat LED
    SysCtlPeripheralEnable(HB_LED_PERIPH);
    while (!SysCtlPeripheralReady(HB_LED_PERIPH)) {}
    GPIOPinTypeGPIOOutput(HB_LED_PORT, HB_LED_PIN);

    IntMasterEnable();

    later(blink_leds);
    namedAction(blink_leds);
}

// ── Diagnostics ───────────────────────────────────────────────────────────

void show_timer(void) {
    print("  ticks: "); printDec(get_ticks());
    print("  uptime_s: "); printDec(getUptime());
    print("\n");
}
