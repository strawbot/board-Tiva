// clocks.c — TIVA (TM4C123GH6PM) time base and TimbreOS clock port
//
// ── CLOCK ARCHITECTURE ────────────────────────────────────────────────────
//
// Timer0A — 32-bit periodic, 1 ms period @ 80 MHz.
//   ISR increments g_ms. get_ticks() returns g_ms.
//   tea.c owns getTime() = to_msec(get_ticks()) — with ONE_SECOND=1000 this
//   is an identity, so getTime() == get_ticks() == ms since boot.
//   timestamp.c is excluded from the build to avoid the duplicate definition.
//
// Timer1A — 32-bit one-shot.
//   set_delta_alarm(delta) loads (delta × 80 000) cycles and starts it.
//   ISR calls (*alarmEvent)() to dispatch due time events.
//   Maximum one-shot: DELTA_MAX_TICKS = 50 000 ms. Longer alarms are clamped;
//   the scheduler re-arms automatically.
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
#include "driverlib/sysctl.h"
#include "driverlib/timer.h"
#include "driverlib/interrupt.h"
#include "driverlib/gpio.h"

#include "tea.h"
#include "clocks.h"
#include "printers.h"

// ── ms uptime counter ─────────────────────────────────────────────────────
// Owned here. tea.c's getTime() = to_msec(get_ticks()) reads this via
// get_ticks(). timestamp.c is excluded from the build.

static volatile uint32_t g_ms = 0;

Long get_ticks(void) { return (Long)g_ms; }

// getUptime(): seconds since boot — declared in tea.h, provided by board.
uint32_t getUptime(void) { return g_ms / 1000u; }

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
    case GAP1:  hb_off(); phase = HB2;  t = msec(200) - msec(3); break;
    case HB2:   hb_on();  phase = GAP2; t = msec(3);             break;
    default:
    case GAP2:  hb_off(); phase = HB1;  t = msec(800) - msec(3); break;
    }
    in(t, blink_leds);
}

// ── Delta alarm — Timer1A one-shot ────────────────────────────────────────

#define CYCLES_PER_TICK  (80000000UL / ONE_SECOND)   // 80 000 cycles per ms
#define DELTA_MAX_TICKS  50000L                       // 50 s max one-shot

void over_due(void) {}

void set_delta_alarm(Long t) {
    if (t > DELTA_MAX_TICKS) t = DELTA_MAX_TICKS;
    if (t < 1)               t = 1;
    uint32_t load = (uint32_t)t * CYCLES_PER_TICK - 1UL;
    TimerDisable(TIMER1_BASE, TIMER_A);
    TimerLoadSet(TIMER1_BASE, TIMER_A, load);
    TimerEnable (TIMER1_BASE, TIMER_A);
}

void Timer1AIntHandler(void) {
    TimerIntClear(TIMER1_BASE, TIMER_TIMA_TIMEOUT);
    now(*alarmEvent);
}

// ── 1 ms tick — Timer0A periodic ─────────────────────────────────────────

void Timer0AIntHandler(void) {
    TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    g_ms++;
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

    // Timer0A: 1 ms periodic
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_TIMER0)) {}
    TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC);
    TimerLoadSet(TIMER0_BASE, TIMER_A, SysCtlClockGet() / 1000 - 1);
    IntEnable(INT_TIMER0A);
    TimerIntEnable(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    TimerEnable(TIMER0_BASE, TIMER_A);

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

void print_build_banner(void) {
    print("\nTIVA Built: ");
    print(__TIMESTAMP__);
    print("\n");
}

void show_timer(void) {
    print("  ticks: "); printDec(get_ticks());
    print("  uptime_s: "); printDec(getUptime());
    print("\n");
}
