// project_defs.h — TimbreOS configuration for TM4C123GH6PM (TIVA board)
//
// Included by tea.h, timeout.h, and every TimbreOS source that needs the
// scheduler constants and platform macros.  Keep platform-specific headers
// here; TimbreOS sources must remain board-agnostic.

#ifndef PROJECT_DEFS_H_
#define PROJECT_DEFS_H_

#include "ttypes.h"   // Cell, Long, Byte, vector, …

// ── CLI parameters ────────────────────────────────────────────────────────
// RAM sizing notes (see CLAUDE.md RAM-audit):
//   LINE_LENGTH  → tib buffer (LINE_LENGTH + 8 bytes)
//   EMITQ_SIZE   → emitq byte-queue (EMITQ_SIZE + 16 bytes in .data)
//   KEYQ_SIZE    → keyq byte-queue  (KEYQ_SIZE  + 16 bytes in .data)
//   HERE_SPACE   → hereSpace compiled-word buffer (.bss)
// Increase LINE_LENGTH if long input lines are truncated.
// Increase EMITQ_SIZE if output characters are dropped at high burst rates.
// Increase HERE_SPACE if runtime word compilation overflows (CLI reports error).
#define CLI_PARAMETERS
#define CLI_TITLE       "ActiveRobot TIVA\n"
#define DCELLS          20
#define RCELLS          20
#define KEYQ_SIZE       80
#define LINE_LENGTH     100
#define EMITQ_SIZE      200
#define PAD_SIZE        20
#define PROMPTSTRING    "ar: "
#define CUSHION         LINE_LENGTH
#define HERE_SPACE      1000
#define OUTPUT_BLOCKED  output()
#define OUTPUT_FLUSH    output()

void output(void);

// ── TEA scheduler ──────────────────────────────────────────────────────────
// RAM sizing notes:
//   NUM_ACTIONS → actionq slot array (.data)
//   NUM_TE      → tes array + 4 hash-tables (teanames/teatimes + adjuncts, .bss)
//                 Each table ≈ NUM_TE × 25 bytes; all four together dominate.
//                 Current active TE count at runtime: blink_leds(1), alarm(1),
//                 CLI tick(~2), heartbeat(1) → peak ≈ 8.  20 gives 2.5× headroom.
//   N_EVENTS    → eventq ring buffer (8 bytes/event + 16-byte header, .data)
// Raise NUM_TE if tea.c asserts BLACK_HOLE (TE slots exhausted).
// Raise N_EVENTS if events are dropped under burst load.
#define NUM_ACTIONS  20
#define NUM_TE       20
#define TEA_TABLE    HASH8
#define N_EVENTS     50
#define FIRST_EVENT  (const char *)secs(5)

// ONE_SECOND = 80 000 000 → 1 tick = 1 cycle at 80 MHz (Timer0A, no ISR).
// msec(n) and secs(n) use 64-bit intermediates in tea.h so large ONE_SECOND is safe.
// tea.c signed-int delta arithmetic: INT_MAX/80000000 ≈ 26.8 s scheduling horizon;
// the scheduler re-arms automatically for longer durations.
#define ONE_SECOND  80000000UL

// ── Clocks hardware wiring (consumed by TimbreOS/clocks.c) ────────────────
// TIVA uses TivaWare timer APIs — all hardware clock functions live in
// src/clocks.c.  TimbreOS/clocks.c supplies the board-agnostic utilities
// (timestamp_to_utc, epoch_to_tm, tm_to_epoch, over_due, micro_sleep,
// print_build_banner).
// TIVA has no RTC, so get_utc() is not defined; CLOCK_HAS_SHOW avoids
// referencing it in the common show_timer().
#define CLOCK_HAS_BLINK   // src/clocks.c provides blink_leds() via TivaWare GPIO
#define CLOCK_HAS_INIT    // src/clocks.c provides init_clocks() via TivaWare timer
#define CLOCK_HAS_TICKS   // src/clocks.c provides get_ticks()
#define CLOCK_HAS_DELTA   // src/clocks.c provides set_delta_alarm() / delta_alarm()
#define CLOCK_HAS_SHOW    // src/clocks.c provides show_timer() (uses getUptime, not get_utc)

// ── Atomic sections (ARM Cortex-M4 CPSID / CPSIE) ─────────────────────────
// tea.h's safe(code) macro expands to:
//   ENTER_SAFE_REGION() code LEAVE_SAFE_REGION()
// with NO surrounding semicolons.  Each macro must therefore be a COMPLETE
// statement (semicolon included) so the next token (e.g. a type name in a
// declaration) does not produce "expected ';' before ..." errors.
#define ENTER_SAFE_REGION()  __asm volatile ("cpsid i" ::: "memory");
#define LEAVE_SAFE_REGION()  __asm volatile ("cpsie i" ::: "memory");

// ── Interrupt-context detection ────────────────────────────────────────────
// Reads IPSR; non-zero while inside any exception handler.
static inline int _tiva_in_irq(void) {
    uint32_t ipsr;
    __asm volatile ("mrs %0, ipsr" : "=r" (ipsr));
    return (int)(ipsr & 0x1FFu);
}
#define in_interrupt()  _tiva_in_irq()
#define IN_INTERRUPT()  _tiva_in_irq()

// ── CPU clock and high-resolution cycle counter ────────────────────────────
// DWT CYCCNT ticks at 80 MHz (12.5 ns). Enabled in init_clocks().
// tea.c uses SYS_TO_US/MS/NS to convert raw cycle-counter deltas for the
// action profiler (machineStats / namedAction).
#define CLOCK_MHZ       80u
#define DWT_CYCCNT_REG  (*(volatile uint32_t *)0xE0001004u)
#define sysTicks()      ((Long)DWT_CYCCNT_REG)

#define SYS_TO_NS(n)  ((unsigned long long)(n) * 1000u / CLOCK_MHZ)
#define SYS_TO_US(n)  ((unsigned long long)(n) / CLOCK_MHZ)
#define SYS_TO_MS(n)  ((unsigned long long)(n) / ((unsigned long long)CLOCK_MHZ * 1000u))
#define US_TO_SYS(n)  ((n) * CLOCK_MHZ)

// ── Tick counter ───────────────────────────────────────────────────────────
// get_ticks() returns raw Timer0A cycles (80 MHz, no ISR, no division).
Long get_ticks(void);

// ── Delta alarm ────────────────────────────────────────────────────────────
// Timer1A one-shot. set_delta_alarm(delta) fires (*alarmEvent)() after
// `delta` ticks. Implemented in clocks.c.
void set_delta_alarm(Long t);
void over_due(void);

#endif /* PROJECT_DEFS_H_ */
