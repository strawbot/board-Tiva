// tiva_cli.c — CLI command implementations for the TIVA (TM4C123GH6PM) board.
// All functions are void(void) — output via print() / printers.h to emitq.
//
// Modelled after Nucleo411/Board/nucleo_cli.c; adapted for TM4C hardware:
//   • show_sys    — SysCtlClockGet() instead of LL_RCC; no bus prescalers
//   • show_timers — TIMER0–TIMER5 GPTM registers instead of STM32 TIM1–TIM14
//   • do_reboot   — identical NVIC_SystemReset() call
//   • show_cli    — identical dropped-character counter display

#include <stdbool.h>
#include <stdint.h>

#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/sysctl.h"

#include "tea.h"
#include "printers.h"
#include "clocks.h"
#include "tiva_cli.h"
#include "canary.h"

// ── Hardware timer survey ─────────────────────────────────────────────────────
//
// show_timers() — one-line summary per active half-timer of TIMER0–TIMER5:
//   clock-gate state (RCGCTIMER), TAEN/TBEN, mode, PSC, ILR, TV
//
// ILR (interval load / reload) and TV (timer value) are shown in hex.
// PSC (prescale) is decimal — only valid and shown in 16-bit individual mode.
// TM4C timers count DOWN; TV decrements from ILR to 0.
// Timers whose RCGCTIMER clock-gate bit is clear are skipped entirely.
// If GPTMCFG == 0 (32-bit combined) only the A half is shown.

// RCGCTIMER: Run-Mode Clock Gating Control — bit n enables TIMER(n).
#define RCGCTIMER_REG   HWREG(0x400FE604u)

// GPTM register offsets (from TM4C123GH6PM datasheet).
#define GPTM_CFG    0x000u   // GPTMCFG  — 0=32-bit combined, 4=16-bit individual
#define GPTM_TAMR   0x004u   // GPTMTAMR — bits[1:0]: 1=one-shot 2=periodic 3=capture
#define GPTM_TBMR   0x008u   // GPTMTBMR
#define GPTM_CTL    0x00Cu   // GPTMCTL  — TAEN=bit0, TBEN=bit8
#define GPTM_TAILR  0x028u   // Timer A interval load (reload value)
#define GPTM_TBILR  0x02Cu   // Timer B interval load
#define GPTM_TAPR   0x038u   // Timer A prescale  (16-bit mode only)
#define GPTM_TBPR   0x03Cu   // Timer B prescale
#define GPTM_TAV    0x050u   // Timer A value (live, counts down)
#define GPTM_TBV    0x054u   // Timer B value

static const char * const s_timer_mode[4] = { "---", "one-shot", "periodic", "capture" };

// Column positions — shared by header and every data row.
// "TIMER0A: " occupies cols 0-9 (9 chars + ": " = 11 chars total).
#define CT_ON    12   // "On"
#define CT_MODE  16   // mode string (longest: "one-shot" = 8 chars)
#define CT_PSC   26   // "PSC"
#define CT_ILR   33   // "ILR" (8 hex digits + 'h' = 9 chars)
#define CT_TV    44   // "TV"

// Print one half-timer row (A or B).
static void show_half_timer(const char *name, uint32_t base, bool isA, bool has_psc)
{
    uint32_t mr  = isA ? HWREG(base + GPTM_TAMR)  : HWREG(base + GPTM_TBMR);
    uint32_t ctl = HWREG(base + GPTM_CTL);
    uint32_t psc = isA ? HWREG(base + GPTM_TAPR)  : HWREG(base + GPTM_TBPR);
    uint32_t ilr = isA ? HWREG(base + GPTM_TAILR) : HWREG(base + GPTM_TBILR);
    uint32_t tv  = isA ? HWREG(base + GPTM_TAV)   : HWREG(base + GPTM_TBV);
    uint32_t en  = isA ? (ctl & 0x1u) : ((ctl >> 8) & 0x1u);

    print(name); print(": ");
    tabTo(CT_ON);   print(en ? "ON" : "--");
    tabTo(CT_MODE); print(s_timer_mode[mr & 0x3u]);
    if (has_psc) { tabTo(CT_PSC); printDec(psc); }
    tabTo(CT_ILR); dotnb(8, 8, ilr, 16); print("h");
    tabTo(CT_TV);  dotnb(8, 8, tv,  16); print("h");
    printCr();
}

void show_timers(void)
{
    static const struct {
        uint32_t base;
        char     nameA[9];
        char     nameB[9];
        uint8_t  bit;     // RCGCTIMER enable bit
    } td[] = {
        { TIMER0_BASE, "TIMER0A", "TIMER0B", 0 },
        { TIMER1_BASE, "TIMER1A", "TIMER1B", 1 },
        { TIMER2_BASE, "TIMER2A", "TIMER2B", 2 },
        { TIMER3_BASE, "TIMER3A", "TIMER3B", 3 },
        { TIMER4_BASE, "TIMER4A", "TIMER4B", 4 },
        { TIMER5_BASE, "TIMER5A", "TIMER5B", 5 },
    };

    // Header
    print("Timer");
    tabTo(CT_ON);   print("On");
    tabTo(CT_MODE); print("Mode");
    tabTo(CT_PSC);  print("PSC");
    tabTo(CT_ILR);  print("ILR");
    tabTo(CT_TV);   print("TV");
    printCr();

    uint32_t rcgc = RCGCTIMER_REG;

    for (unsigned i = 0; i < sizeof(td)/sizeof(td[0]); i++) {
        if (!(rcgc & (1u << td[i].bit))) { continue; }

        uint32_t cfg  = HWREG(td[i].base + GPTM_CFG);
        bool     psc  = (cfg == 4u);   // prescale register valid in 16-bit mode

        if (cfg == 0u) {
            // 32-bit combined — A half spans the full 32-bit counter; B unused.
            show_half_timer(td[i].nameA, td[i].base, true, false);
        } else {
            // 16-bit individual — A and B operate independently.
            show_half_timer(td[i].nameA, td[i].base, true,  psc);
            show_half_timer(td[i].nameB, td[i].base, false, psc);
        }
    }
    printCr();
}

// ── do_reboot ─────────────────────────────────────────────────────────────────

void do_reboot(void) {
    print("rebooting..."); printCr();
    SysCtlReset();
}

// ── show_stack ────────────────────────────────────────────────────────────────

void show_stack(void) {
    print("stack: ");
    stack_render(stack_check());
    printCr();
}

// ── show_cli ──────────────────────────────────────────────────────────────────

static Long dropped_chars = 0;

void show_cli(void) { print("\nDropped chars: "); printDec(dropped_chars); }

// ── show_sys ──────────────────────────────────────────────────────────────────
// The TM4C123GH6PM runs from a single PLL-derived system clock (80 MHz).
// There are no APB bus prescalers to report — just SYSCLK.

void show_sys(void) {
    uint32_t sysclk = SysCtlClockGet();
    print("SYSCLK:  "); printDec(sysclk / 1000000u); print(" MHz"); printCr();
    print("uptime:  "); printDec(get_ticks());         print(" ticks"); printCr();
    show_timer();
    show_stack();
}

