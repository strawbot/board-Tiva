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
#include "inc/hw_ints.h"
#include "driverlib/sysctl.h"
#include "driverlib/usb.h"
#include "driverlib/interrupt.h"
#include "driverlib/rom.h"

#include "tea.h"
#include "printers.h"
#include "clocks.h"
#include "tiva_cli.h"
#include "usb_netif.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
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

// ── show_usb ──────────────────────────────────────────────────────────────────
// Prints USB CDC-ECM device state and the current IP if the link is active.

void show_usb(void) {
    static const char *const s_state[] = {
        "reset", "addressed", "configured", "active"
    };
    usb_ecm_status_t st = usb_netif_status();
    print("USB state:   "); print(s_state[st]); print("\r\n");

    struct netif *n = netif_default;
    if (!n) { print("netif:       (none)\r\n"); return; }

    print("netif:       ");
    print(netif_is_up(n)      ? "up "      : "down ");
    print(netif_is_link_up(n) ? "link-up"  : "link-down");
    print("\r\n");

    if (netif_is_up(n) && netif_is_link_up(n)) {
        print("IP:          "); print(ip4addr_ntoa(netif_ip4_addr(n))); print("\r\n");

        struct dhcp *d = netif_dhcp_data(n);
        print("DHCP:        ");
        if (!d)                            print("(static)");
        else if (dhcp_supplied_address(n)) print("bound");
        else                               print("searching");
        print("\r\n");
    }
}

// ── debug_usb ─────────────────────────────────────────────────────────────────
// Raw hardware register dump + ISR counters.
//
// Register offsets from TM4C123GH6PM datasheet (Table 14-3 USB, Table 10-6 GPIO).
// Defined here to avoid depending on TivaWare hw_usb.h / hw_gpio.h which are
// in the TivaWare inc/ path but not all constants are guaranteed present.
//
// Interpretation ladder — each line should be "ok" before worrying about later ones:
//   RCGCUSB=0  → SysCtlPeripheralEnable(USB0) was ignored; peripheral is dead
//   SOFTCONN=0 → USBDevConnect() write was ignored; macOS will never see D+
//   VBUS<3     → cable not connected or host not supplying power
//   ISR=0      → interrupt never fired (vector table or IntEnable problem)
//   resets=0   → host never sent a bus reset (enumeration never started)
//   ep0=0      → host never sent a SETUP packet (stuck after reset)

// USB register byte offsets from USB0_BASE
#define DBG_USB_FADDR   0x000u   // Function address (byte)
#define DBG_USB_POWER   0x001u   // Power control (byte): bit6=SOFTCONN bit3=RESET
#define DBG_USB_DEVCTL  0x060u   // Device control (byte): bits[4:3]=VBUS bit2=HOST

// GPIO Port D analog-mode-select register offset
#define DBG_GPIO_AMSEL  0x528u   // Analog Mode Select (word): bits 4,5 for PD4/PD5

// RCGCUSB: Run-Mode Clock Gating for USB (Sysctl, base 0x400FE000 + offset 0x628)
#define DBG_RCGCUSB     HWREG(0x400FE628u)

// Sysctl register addresses for clock / peripheral-ready diagnostics
#define DBG_PLLSTAT     HWREG(0x400FE168u)   // PLL lock status: bit0=LOCK
#define DBG_RCC         HWREG(0x400FE060u)   // Run-Mode Clock Config
#define DBG_PRSUSB      HWREG(0x400FEA28u)   // USB peripheral ready: bit0=R0

void debug_usb(void)
{
    // ── 0. System clock and USB PHY clock ────────────────────────────────
    // USB requires: (a) main PLL locked, (b) MOSC selected, (c) USB peripheral
    // ready (PRSUSB.R0=1).  If any of these are wrong the 60 MHz PHY clock is
    // absent and all USB register reads (including VBUS comparator) are invalid.
    {
        uint32_t pllstat = DBG_PLLSTAT;
        uint32_t rcc     = DBG_RCC;
        uint32_t prsusb  = DBG_PRSUSB;
        uint32_t sysclk  = SysCtlClockGet();

        print("SYSCLK:    "); printDec(sysclk / 1000000u); print(" MHz");
        print("  PLL: ");
        print((pllstat & 0x1u) ? "locked" : "NOT LOCKED -- USB clock absent");
        {
            // OSCSRC is bits 5:4 of RCC (2-bit field, mask 0x30).
            uint8_t src = (uint8_t)((rcc >> 4) & 0x3u);
            if      (src == 0) print("  src=MOSC");
            else if (src == 1) print("  src=PIOSC(wrong! USB needs MOSC+PLL)");
            else               { print("  src=0x"); dotnb(1, 1, src, 16); print("(wrong!)"); }
        }
        // BYPASS is RCC bit 11.  When SYSCTL_SYSDIV_2_5 is used TivaWare sets
        // RCC2.USERCC2=1 and RCC2.BYPASS2 takes over — a locked PLL (above)
        // already confirms it is not bypassed, so no extra check needed here.
        printCr();

        print("USB rdy:   ");
        print((prsusb & 0x1u) ? "yes" : "NO -- USB peripheral not ready (PHY clock absent)");
        printCr();
    }

    // ── 1. USB clocks ─────────────────────────────────────────────────────
    uint32_t rcgcusb = DBG_RCGCUSB;
    print("RCGCUSB:   0x"); dotnb(2, 2, rcgcusb & 0xFF, 16);
    print("  USB0 clock: ");
    print((rcgcusb & 0x1u) ? "ON" : "OFF -- peripheral dead, all reg writes ignored");
    printCr();

    // SYSCTL_RCC2.USBPWRDN (bit 14) powers the USB PLL on TM4C123.
    // When set (reset default) the USB PLL is off → analog domain dead.
    // When cleared the USB PLL runs at 60 MHz → VBUS comparator and D+ live.
    // NOTE: 0x400FE7C8 (USBCC) does not exist on TM4C123 — it's TM4C129-only.
    uint32_t rcc2 = HWREG(0x400FE070u);
    print("RCC2:      0x"); dotnb(2, 8, rcc2, 16);
    print("  USB PLL (USBPWRDN clr): ");
    print(!(rcc2 & 0x00004000u) ? "ON (USB PLL powered up)"
                                : "OFF -- USB PLL powered down, analog domain dead!");
    printCr();

    // ── 2. POWER.SOFTCONN — did USBDevConnect() assert D+? ───────────────
    uint8_t power = HWREGB(USB0_BASE + DBG_USB_POWER);
    print("POWER:     0x"); dotnb(2, 2, power, 16);
    print("  SOFTCONN(D+): ");
    print((power & 0x40u) ? "asserted -- host can see device"
                          : "NOT asserted -- host sees nothing");
    printCr();

    // ── 3. DEVCTL — VBUS level and bus mode ──────────────────────────────
    uint8_t devctl = HWREGB(USB0_BASE + DBG_USB_DEVCTL);
    uint8_t vbus   = (devctl >> 3) & 0x3u;   // bits[4:3]
    static const char *const s_vbus[4] = {
        "0 (none -- cable out?)",
        "1 (SessionEnd -- partial connection?)",
        "2 (AValid)",
        "3 (valid -- cable in)"
    };
    print("DEVCTL:    0x"); dotnb(2, 2, devctl, 16);
    print("  VBUS="); print(s_vbus[vbus]);
    print((devctl & 0x04u) ? "  mode=HOST(wrong!)" : "  mode=device");
    printCr();

    // ── 3b. GPCS — device mode configuration ─────────────────────────────
    // offset 0x41C: DEVMOD (bit 0) = B-device role; DEVMODOTG (bit 1) = 1
    // means forced device mode (bypass OTG) which also gates the VBUS
    // comparator.  We use DEVMOD=1, DEVMODOTG=0 (GPCS=0x01) so OTG VBUS
    // comparator stays active and can sense voltage for D+ pull-up gating.
    // Expected steady state: GPCS=0x01.  poll() re-asserts DEVMOD if cleared.
    uint8_t gpcs = HWREGB(USB0_BASE + 0x041Cu);
    print("GPCS:      0x"); dotnb(2, 2, gpcs, 16);
    print("  forced-device: ");
    print((gpcs & 0x01u) ? "yes (USBDevMode ok)"
                         : "NO -- DEVMOD cleared (OTG override?)");
    printCr();

    // ── 4. FADDR — USB address assigned by host ───────────────────────────
    uint8_t faddr = HWREGB(USB0_BASE + DBG_USB_FADDR);
    print("FADDR:     0x"); dotnb(2, 2, faddr, 16);
    print("  addr="); printDec(faddr & 0x7Fu);
    print(faddr ? "  (SET_ADDRESS done)" : "  (not yet addressed)");
    printCr();

    // ── 5a. GPIO PD AMSEL — PD4/PD5 must be in analog mode for USB ──────
    uint32_t amsel_d = HWREG(GPIO_PORTD_BASE + DBG_GPIO_AMSEL);
    print("AMSEL PD:  0x"); dotnb(2, 2, amsel_d & 0xFF, 16);
    print("  PD4/5 analog: ");
    print(((amsel_d & 0x30u) == 0x30u) ? "yes"
                                        : "NO -- D+/D- not in analog mode");
    printCr();

    // ── 5b. GPIO PB AMSEL — PB0/PB1 must be in analog mode for USB ──────
    uint32_t amsel_b = HWREG(GPIO_PORTB_BASE + DBG_GPIO_AMSEL);
    print("AMSEL PB:  0x"); dotnb(2, 2, amsel_b & 0xFF, 16);
    print("  PB0(ID) analog: ");
    print((amsel_b & 0x01u) ? "yes" : "NO");
    print("  PB1(VBUS) analog: ");
    print((amsel_b & 0x02u) ? "yes" : "NO -- VBUS comparator input floating");
    printCr();

    // ── 5c. DEVCTL captured at init — did the SESSION write stick? ────────
    usb_ecm_counters_t c_init;
    usb_netif_get_counters(&c_init);
    print("DEVCTL@init: 0x"); dotnb(2, 2, c_init.devctl_at_init, 16);
    if (c_init.devctl_at_init & 0x01u) {
        uint8_t vbus_init = (c_init.devctl_at_init >> 3) & 0x3u;
        print("  SESSION stuck (good)  VBUS@init="); printDec(vbus_init);
    } else {
        print("  SESSION cleared immediately -- VBUS comparator saw <SessionEnd");
    }
    printCr();

    // ── 6. ISR event counters ─────────────────────────────────────────────
    static const char *const s_state_names[] = {
        "RESET", "ADDRESSED", "CONFIGURED", "ACTIVE"
    };
    uint8_t ds = c_init.dev_state < 4u ? c_init.dev_state : 0u;
    printCr();
    print("dev_state: "); print(s_state_names[ds]); printCr();
    print("ISR total: "); printDec(c_init.isr_total);
    if (!c_init.isr_total) print("  <-- interrupt never fired");
    printCr();
    print("  resets:     "); printDec(c_init.resets);        printCr();
    print("  EP0 rx:     "); printDec(c_init.ep0_setups);    printCr();
    print("  configured: "); printDec(c_init.configured);
    if (!c_init.configured) print("  <-- SET_CONFIGURATION never received");
    printCr();
    print("  setif:      "); printDec(c_init.setif);
    if (!c_init.setif) print("  <-- SET_INTERFACE(data,alt1) never received");
    printCr();
    print("  stalls:     "); printDec(c_init.stalls);
    if (c_init.stalls) print("  <-- host sent request(s) we didn't handle");
    printCr();
    print("  EP2 tx:     "); printDec(c_init.ep2_tx);        printCr();
    print("  EP2 rx:     "); printDec(c_init.ep2_rx);        printCr();

    // ── 7. Last SETUP requests (oldest → newest) ──────────────────────────
    // Decode: type bits 6:5 = 00 standard / 01 class / 10 vendor
    //         bRequest 0x06 = GET_DESCRIPTOR, 0x05 = SET_ADDRESS, etc.
    //         wValue high byte = descriptor type for GET_DESCRIPTOR
    static const char *const s_req_type[] = { "Std", "Cls", "Vnd", "???" };
    static const char *const s_std_req[]  = {
        "GET_STATUS","CLR_FEAT","?","SET_FEAT","?",
        "SET_ADDR","GET_DESC","SET_DESC","GET_CFG","SET_CFG",
        "GET_IF","SET_IF","SYNCH_FR"
    };
    static const char *const s_desc_type[] = {
        "?","DEVICE","CONFIG","STRING","IF","EP","?","QUAL","?",
        "?","?","?","?","?","?","BOS"
    };
    printCr();
    print("Last SETUP log (oldest first):\r\n");
    uint8_t next = c_init.setup_log_next;
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t idx = (next + i) % 4;
        uint8_t rtype = c_init.setup_log[idx][0];
        uint8_t rreq  = c_init.setup_log[idx][1];
        uint8_t vlo   = c_init.setup_log[idx][2];
        uint8_t vhi   = c_init.setup_log[idx][3];
        if (rtype == 0 && rreq == 0 && vlo == 0 && vhi == 0) {
            print("  [empty]\r\n"); continue;
        }
        uint8_t tcat = (rtype >> 5) & 0x3u;
        print("  bmType=0x"); dotnb(2, 2, rtype, 16);
        print(" ("); print(s_req_type[tcat < 3 ? tcat : 3]); print(")");
        print("  bReq=0x");  dotnb(2, 2, rreq, 16);
        if (tcat == 0 && rreq < 13) { print(" "); print(s_std_req[rreq]); }
        if (tcat == 0 && rreq == 6) {   // GET_DESCRIPTOR
            uint8_t dt = vhi < 16 ? vhi : 0;
            print(" type="); print(s_desc_type[dt]);
            print(" idx="); dotnb(2, 1, vlo, 10);
        }
        printCr();
    }
}

// ── probe_usb_pins ────────────────────────────────────────────────────────────
// Temporarily switches D+ (PD5) and D- (PD4) out of USB analog mode and reads
// them digitally under pull-up and pull-down to detect broken or swapped traces.
//
// Interpretation (cable plugged into powered Mac):
//
//   Pull-UP applied:
//     LOW  — expected.  The Mac's 15 kΩ host pull-down overcomes our ~75 kΩ
//             GPIO pull-up (V ≈ 0.55 V).  The line is electrically connected.
//     HIGH — bad.  No host pull-down is reaching this pin — open trace/solder.
//
//   Pull-DOWN applied:
//     LOW  — expected.  Both our pull-down and the host pull-down agree.
//     HIGH — very bad.  Something else is driving this line high.
//
// If D+ and D- give different results the wires may be swapped.
// After the measurement the pins are restored to USB analog mode.
//
// GPIO register offsets (from inc/hw_gpio.h / TM4C123 datasheet)
#define PROBE_O_DIR    0x400u
#define PROBE_O_PUR    0x510u
#define PROBE_O_PDR    0x514u
#define PROBE_O_DEN    0x51Cu
#define PROBE_O_AMSEL  0x528u
#define PROBE_DATA_ALL 0x3FCu   // GPIODATA with all-bits mask

static void probe_one(const char *label, uint32_t base, uint8_t pin)
{
    uint32_t mask = (uint32_t)(1u << pin);

    // --- save original register state ---
    uint32_t orig_amsel = HWREG(base + PROBE_O_AMSEL);
    uint32_t orig_den   = HWREG(base + PROBE_O_DEN);
    uint32_t orig_dir   = HWREG(base + PROBE_O_DIR);
    uint32_t orig_pur   = HWREG(base + PROBE_O_PUR);
    uint32_t orig_pdr   = HWREG(base + PROBE_O_PDR);

    // --- switch from USB analog to digital input ---
    HWREG(base + PROBE_O_AMSEL) &= ~mask;   // disable analog (connect digital buffer)
    HWREG(base + PROBE_O_DEN)   |=  mask;   // enable digital function
    HWREG(base + PROBE_O_DIR)   &= ~mask;   // input direction
    HWREG(base + PROBE_O_PUR)   &= ~mask;   // no pull yet
    HWREG(base + PROBE_O_PDR)   &= ~mask;

    // --- read with pull-UP ---
    HWREG(base + PROBE_O_PUR) |= mask;
    SysCtlDelay(SysCtlClockGet() / 3000);   // ~1 ms settle
    uint8_t pu = (uint8_t)((HWREG(base + PROBE_DATA_ALL) >> pin) & 1u);

    // --- read with pull-DOWN ---
    HWREG(base + PROBE_O_PUR) &= ~mask;
    HWREG(base + PROBE_O_PDR) |=  mask;
    SysCtlDelay(SysCtlClockGet() / 3000);
    uint8_t pd = (uint8_t)((HWREG(base + PROBE_DATA_ALL) >> pin) & 1u);

    // --- restore original state (USB analog mode) ---
    HWREG(base + PROBE_O_PDR)   = orig_pdr;
    HWREG(base + PROBE_O_PUR)   = orig_pur;
    HWREG(base + PROBE_O_DIR)   = orig_dir;
    HWREG(base + PROBE_O_DEN)   = orig_den;
    HWREG(base + PROBE_O_AMSEL) = orig_amsel;

    // --- report ---
    print(label); print(":  PU="); printDec(pu);
    print(pu ? "(HIGH -- open? or cable out?)" : "(LOW  -- host pull-down present)");
    print("  PD="); printDec(pd);
    print(pd ? "(HIGH -- unexpected driver!)" : "(LOW  -- ok)");
    printCr();
}

void probe_usb_pins(void)
{
    print("Probing USB data lines (cable must be connected to powered Mac):\r\n");
    probe_one("D- PD4", GPIO_PORTD_BASE, 4);
    probe_one("D+ PD5", GPIO_PORTD_BASE, 5);
    printCr();
    print("Both PU=LOW  : lines intact, host pull-downs present\r\n");
    print("D+ PU=HIGH   : D+ trace open or cable not connected\r\n");
    print("D- PU=HIGH   : D- trace open\r\n");
    print("D+ and D- differ: wires may be swapped on the PCB\r\n");
}

// ── probe_vbus ────────────────────────────────────────────────────────────────
// Diagnostic: is the VBUS comparator actually connected to PB1?
//
// With AMSEL=1 on PB1 and 5 V VBUS on the cable, DEVCTL.VBUS should read 3
// (above BValid = 4.4 V).  We persistently see 0.  This command tests whether
// the comparator input is wired to PB1 at all:
//
//  Step A: read DEVCTL.VBUS in current state (AMSEL=1, should be 3, is 0).
//  Step B: temporarily clear AMSEL (digital mode, comparator input disconnected).
//          Read DEVCTL.VBUS — in digital mode the comparator input is floating;
//          if VBUS changes, the comparator WAS responding to PB1.
//  Step C: restore AMSEL=1, wait 2 ms for comparator to re-sample, read again.
//
// Interpretation:
//   B != A  →  comparator IS connected to PB1, the floating input changed reading.
//   C != A  →  comparator now sees the correct voltage; AMSEL toggle "woke" it.
//   A=B=C=0 →  comparator NOT seeing PB1 at all — hardware defect (open trace,
//               wrong BOM, or PB1 not connected to VBUS via R6).

void probe_vbus(void)
{
    static const char *const s_v[4] = { "0(<0.2V)", "1(~0.2-0.8V)", "2(~0.8-4.4V)", "3(>4.4V=valid)" };

    print("VBUS comparator probe (cable must be connected to powered Mac):\r\n");

    // ── A: current state ──────────────────────────────────────────────────
    uint8_t devctl_a = HWREGB(USB0_BASE + 0x060u);
    print("  A) AMSEL=1 (USB analog): VBUS=");
    print(s_v[(devctl_a >> 3) & 0x3u]);
    printCr();

    // ── B: temporarily switch PB1 to digital (disconnects comparator input) ─
    uint32_t orig_amsel = HWREG(GPIO_PORTB_BASE + PROBE_O_AMSEL);
    uint32_t orig_den   = HWREG(GPIO_PORTB_BASE + PROBE_O_DEN);
    uint32_t orig_dir   = HWREG(GPIO_PORTB_BASE + PROBE_O_DIR);

    HWREG(GPIO_PORTB_BASE + PROBE_O_AMSEL) &= ~0x2u;  // PB1 AMSEL=0
    HWREG(GPIO_PORTB_BASE + PROBE_O_DEN)   |=  0x2u;  // PB1 digital enable
    HWREG(GPIO_PORTB_BASE + PROBE_O_DIR)   &= ~0x2u;  // PB1 input direction
    SysCtlDelay(SysCtlClockGet() / 3000);              // ~1 ms settle

    // Read PB1 GPIO data: 0x008 = GPIODATA with bit mask 0x2 (PB1 only).
    // This reads the *physical* voltage on the PB1 pad as a digital level.
    // HIGH → VBUS trace actually reaches PB1 (hardware connection intact).
    // LOW  → PB1 is floating or grounded — VBUS trace not connected.
    uint8_t pb1_pin = (HWREG(GPIO_PORTB_BASE + 0x008u) != 0u) ? 1u : 0u;

    uint8_t devctl_b = HWREGB(USB0_BASE + 0x060u);
    print("  B) AMSEL=0 (GPIO digital): VBUS=");
    print(s_v[(devctl_b >> 3) & 0x3u]);
    print("  PB1_pad="); print(pb1_pin ? "HIGH(voltage present on pin)" : "LOW(no voltage -- VBUS not wired to PB1?)");
    printCr();

    // ── C: restore analog, wait for comparator to re-sample ──────────────
    HWREG(GPIO_PORTB_BASE + PROBE_O_DIR)   = orig_dir;
    HWREG(GPIO_PORTB_BASE + PROBE_O_DEN)   = orig_den;
    HWREG(GPIO_PORTB_BASE + PROBE_O_AMSEL) = orig_amsel;
    SysCtlDelay(SysCtlClockGet() / 500);               // ~2 ms settle

    uint8_t devctl_c = HWREGB(USB0_BASE + 0x060u);
    print("  C) AMSEL=1 restored: VBUS=");
    print(s_v[(devctl_c >> 3) & 0x3u]);
    printCr();

    // ── D: test VBUS comparator with DEVMODOTG=0 (OTG mode) ──────────────
    // Hypothesis: forced device mode (DEVMODOTG=1) power-gates the VBUS
    // comparator as an optimisation.  Temporarily clear DEVMODOTG to let
    // the OTG state machine re-enable the comparator and re-read VBUS.
    // This is a read-only test — we restore GPCS immediately after.
    uint32_t orig_gpcs = HWREG(USB0_BASE + 0x041Cu);
    HWREG(USB0_BASE + 0x041Cu) = orig_gpcs & ~0x02u;   // clear DEVMODOTG
    SysCtlDelay(SysCtlClockGet() / 1000);               // ~1 ms
    uint8_t devctl_d = HWREGB(USB0_BASE + 0x060u);
    uint8_t vd = (devctl_d >> 3) & 0x3u;
    print("  D) DEVMODOTG=0 (OTG comparator): VBUS=");
    print(s_v[vd]);
    printCr();
    HWREG(USB0_BASE + 0x041Cu) = orig_gpcs;             // restore forced device mode
    SysCtlDelay(SysCtlClockGet() / 1000);
    printCr();

    uint8_t va = (devctl_a >> 3) & 0x3u;
    uint8_t vc = (devctl_c >> 3) & 0x3u;

    if (va == 3 && vc == 3) {
        print("RESULT: comparator works in forced-device mode -- VBUS=3 stable.\r\n");
    } else if (!pb1_pin) {
        print("RESULT: PB1_pad=LOW -- VBUS is NOT wired to PB1.\r\n");
        print("  Connect USB VBUS to PB1 via 10k resistor to fix VBUS sensing.\r\n");
    } else if (vd > 0 && va == 0) {
        // The key finding: comparator reads 0 in forced-device mode but
        // non-zero in OTG mode → DEVMODOTG=1 gates the comparator.
        print("RESULT: VBUS comparator is GATED by forced device mode (DEVMODOTG=1).\r\n");
        print("  D)="); printDec(vd);
        print(" but A)=C)=0 -- comparator only works when DEVMODOTG=0.\r\n");
        print("  Fix: init with DEVMOD=1 + DEVMODOTG=0 + SESSION=1.\r\n");
        print("  This keeps OTG comparator active while forcing B-device role.\r\n");
    } else {
        print("RESULT: PB1 has voltage but comparator reads 0 in both modes.\r\n");
        print("  Hardware VBUS sense fault -- check VDD33USB supply on board.\r\n");
    }
}

// ── vbus_poll ─────────────────────────────────────────────────────────────────
// Monitors DEVCTL.VBUS and the USB ISR counter in real time for 10 seconds.
//
// Primary use: hot-plug diagnostic.
//   1. Power the board from an external 3.3V supply (USB cable unplugged).
//   2. Flash and run this firmware.
//   3. Type "vbus-poll" — the command starts a 10-second live monitor.
//   4. Plug in the USB cable.
//
// What to watch for:
//   VBUS goes 0 → 3  : VBUS comparator works; D+ gate should open; Mac enumerates.
//   VBUS stays 0     : PB1 has no electrical path to USB VBUS (hardware fault).
//   ISR ticks up     : host sent a bus reset — enumeration has begun.
//   "[CHANGE]" tag   : any transition in VBUS or ISR is highlighted.
//
// The command also fires automatically on each USB ISR increment so you see
// the exact moment the host resets the bus.

#define VP_INTERVALS  25u   // 25 × 400 ms = 10 s
#define VP_MS         400u

static uint8_t  s_vp_left;
static uint8_t  s_vp_last_vbus;
static uint8_t  s_vp_last_gpcs;
static uint32_t s_vp_last_isr;

static void vp_tick(void)
{
    uint8_t  devctl = HWREGB(USB0_BASE + 0x060u);
    uint8_t  vbus   = (devctl >> 3) & 0x3u;
    uint8_t  gpcs   = HWREGB(USB0_BASE + 0x041Cu);

    usb_ecm_counters_t c;
    usb_netif_get_counters(&c);

    bool changed = (vbus != s_vp_last_vbus) || (gpcs != s_vp_last_gpcs)
                || (c.isr_total != s_vp_last_isr);
    bool heartbeat = ((VP_INTERVALS - s_vp_left) % 5u == 0u);

    if (changed || heartbeat) {
        print(changed ? "[CHANGE] " : "         ");
        print("VBUS="); printDec(vbus);
        if      (vbus == 0) print("(none)  ");
        else if (vbus == 1) print("(sessn) ");
        else if (vbus == 2) print("(avalid)");
        else                print("(VALID) ");
        print("  DEVCTL=0x"); dotnb(2, 2, devctl, 16);
        print("  GPCS=0x"); dotnb(2, 2, gpcs, 16);
        print((gpcs & 0x01u) ? "(dev-ok)" : "(OTG!)  ");
        print("  ISR="); printDec(c.isr_total);
        print("  resets="); printDec(c.resets);
        if (vbus == 3) print("  <-- D+ gate open, host should enumerate");
        printCr();
        s_vp_last_vbus = vbus;
        s_vp_last_gpcs = gpcs;
        s_vp_last_isr  = c.isr_total;
    }

    if (s_vp_left > 0u) {
        s_vp_left--;
        after(VP_MS, vp_tick);
    } else {
        print("vbus-poll done.\r\n");
    }
}

void vbus_poll(void)
{
    s_vp_left      = VP_INTERVALS;
    s_vp_last_vbus = 0xFFu;         // force first print
    s_vp_last_gpcs = 0xFFu;
    s_vp_last_isr  = 0xFFFFFFFFu;
    print("Watching VBUS+GPCS for 10 s (400 ms ticks) -- hot-plug USB cable now:\r\n");
    later(vp_tick);
}

// ── usb_reconnect ─────────────────────────────────────────────────────────────
// Simulates unplugging and replugging the USB cable.
// Drops D+ for 50 ms so the host registers a disconnect, then reconnects.
// Useful when the cable was already in at boot and enumeration stalled,
// or any time you want to force re-enumeration without pulling the cable.

static void usb_do_connect(void)
{
    HWREGB(USB0_BASE + 0x060u) |= 0x01u;   // DEVCTL |= SESSION
    USBDevConnect(USB0_BASE);
    print("USB: reconnected\r\n");
}

void usb_reconnect(void)
{
    USBDevDisconnect(USB0_BASE);
    print("USB: disconnected — reconnecting in 50 ms\r\n");
    after(msec(50), usb_do_connect);
}

// ── test_usb_isr ──────────────────────────────────────────────────────────────
// Manually pends the USB0 interrupt (INT_USB0) via NVIC software-set-pending.
// If USB0IntHandler is correctly wired in the vector table, s_isr_total will
// increment before this function returns.
//
// Pass / Fail tells us:
//   PASS — vector table entry for IRQ44 points at USB0IntHandler, IntEnable
//           worked. Hardware is not sending interrupts for some other reason
//           (PHY not driving D+, VBUS comparator, etc.) but the ISR path works.
//   FAIL — vector table entry still points at IntDefaultHandler (the .rept
//           block was not split correctly), OR IntEnable(INT_USB0) was never
//           called, OR a BASEPRI / FAULTMASK is masking the interrupt.

void test_usb_isr(void)
{
    usb_ecm_counters_t c;
    usb_netif_get_counters(&c);
    uint32_t before = c.isr_total;
    print("ISR count before pend: "); printDec(before); printCr();

    // Pend INT_USB0 through the NVIC — fires immediately if interrupt is
    // enabled and nothing is masking it.
    IntPendSet(INT_USB0);

    // One NOP to give the pipeline a cycle to take the pending interrupt
    // before we read the counter.  (The ISR fires between instructions
    // when PRIMASK=0 and the interrupt is enabled.)
    __asm volatile ("nop");

    usb_netif_get_counters(&c);
    uint32_t after = c.isr_total;
    print("ISR count after pend:  "); printDec(after); printCr();

    if (after > before) {
        print("USB ISR delivery: PASS -- vector table + IntEnable correct\r\n");
        print("  (D+ pull-up not working for a different reason)\r\n");
    } else {
        print("USB ISR delivery: FAIL -- ISR never fired\r\n");
        print("  Check: startup_tm4c.s vector at index 60 (IRQ44)\r\n");
        print("         IntEnable(INT_USB0) called in usb_netif_init_hw\r\n");
        print("         PRIMASK / BASEPRI not blocking interrupts\r\n");
    }
}

// ── do_dfu ────────────────────────────────────────────────────────────────────
// Hand control to the TM4C123 ROM USB DFU bootloader.
//
// ROM_UpdateUSB() is the on-chip USB DFU update routine burned into ROM at
// manufacture time. It reconfigures USB for the DFU class and runs a minimal
// firmware-download loop — the host sees "STELLARIS DFU DEVICE" or "LM Flash
// Programmer" compatible device. The function never returns.
//
// This is a hardware-layer diagnostic: if macOS/Linux can enumerate the ROM
// DFU device but our CDC-ECM device was invisible, the problem is in our USB
// stack (descriptors, state machine, etc.) rather than in the hardware.
// If even the ROM device is invisible the problem is hardware (VBUS, D+/D−).
//
// Clock note: ROM_UpdateUSB() reconfigures the system clock internally and
// does not depend on the current clock frequency.

void do_dfu(void)
{
    print("Entering ROM DFU bootloader — host should see LM Flash / DFU device\r\n");
    print("(Does not return. Power-cycle or reflash to recover.)\r\n");

    // Let the UART output drain before we kill interrupts.
    SysCtlDelay(SysCtlClockGet() / 10);     // ~100 ms at 80 MHz

    // Soft-disconnect so the host registers a clean detach before ROM reconnects.
    USBDevDisconnect(USB0_BASE);
    SysCtlDelay(SysCtlClockGet() / 20);     // ~50 ms

    // Disable all interrupts — the ROM bootloader takes full ownership.
    IntMasterDisable();

    // Jump to ROM.  Never returns.
    ROM_UpdateUSB(0);
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

