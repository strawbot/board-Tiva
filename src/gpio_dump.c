/* =============================================================================
 * gpio_dump.c  —  Dump all GPIO pin states (TM4C123GH6PM, bare registers)
 *
 * Modelled after Nucleo411/Board/gpio_dump.c; adapted for TM4C hardware.
 *
 * Prints for every pin:
 *   PIN | NAME (user alias) | MODE | PMC | OD | PULL | DATA | -- | FUNCTION
 *
 * TM4C GPIO mode is derived from four registers:
 *   GPIOAMSEL=1          → ANALOG
 *   GPIODEN=0, AMSEL=0   → DISABL  (neither digital nor analog — reset default)
 *   GPIOAFSEL=1          → ALT     (GPIOPCTL PMC nibble selects the function)
 *   GPIODIR=1            → OUT
 *   otherwise            → IN
 *
 * Usage:
 *   1. Populate alias_table[] with your project's pin assignments.
 *   2. Call gpio_dump_all() — shows every pin on every clocked port.
 *      Optional filter: type a substring after the command (e.g. "pins cli")
 *      to show only pins whose name or pin label matches.
 *
 * Depends on: printers.h  →  print(), printDec(), tabTo(), maybeCr()
 *             inc/hw_memmap.h, inc/hw_types.h, inc/hw_gpio.h (TivaWare)
 * =============================================================================*/

#include <stdint.h>
#include <stdio.h>              /* snprintf                                   */
#include <ctype.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_gpio.h"
#include "printers.h"
#include "cli.h"

/* ── stristr — case-insensitive substring search ─────────────────────────────
 * Source: https://stackoverflow.com/a/27304609
 * Posted by Clifford, modified by community. See post 'Timeline' for history.
 * Retrieved 2026-03-24. License: CC BY-SA 3.0
 * ---------------------------------------------------------------------------*/
char *stristr(const char *str1, const char *str2)
{
    const char *p1 = str1;
    const char *p2 = str2;
    const char *r  = (*p2 == 0) ? str1 : 0;

    while (*p1 != 0 && *p2 != 0) {
        if (tolower((unsigned char)*p1) == tolower((unsigned char)*p2)) {
            if (r == 0) r = p1;
            p2++;
        } else {
            p2 = str2;
            if (r != 0) p1 = r + 1;
            if (tolower((unsigned char)*p1) == tolower((unsigned char)*p2)) {
                r = p1;
                p2++;
            } else {
                r = 0;
            }
        }
        p1++;
    }
    return (*p2 == 0) ? (char *)r : 0;
}

/* =============================================================================
 * 1.  PORT TABLE
 *     TM4C123GH6PM has six GPIO ports, A–F, with 8 pins each.
 *     RCGCGPIO (0x400FE608) gates the run-mode clock; bit n = port n (A=0…F=5).
 *     Ports whose clock gate is off are skipped — touching their registers
 *     before the clock is enabled causes a bus fault.
 * ===========================================================================*/

#define RCGCGPIO_REG  HWREG(0x400FE608u)

typedef struct {
    uint32_t    base;
    const char *prefix;
    uint8_t     rcgc_bit;
} PortEntry_t;

static const PortEntry_t port_table[] = {
    { GPIO_PORTA_BASE, "PA", 0 },
    { GPIO_PORTB_BASE, "PB", 1 },
    { GPIO_PORTC_BASE, "PC", 2 },
    { GPIO_PORTD_BASE, "PD", 3 },
    { GPIO_PORTE_BASE, "PE", 4 },
    { GPIO_PORTF_BASE, "PF", 5 },
};
#define NUM_PORTS  ( sizeof(port_table) / sizeof(port_table[0]) )

/* =============================================================================
 * 2.  PIN ALIAS TABLE
 *     Assign human-readable names to the pins your application uses.
 *     Format:  { port_index, pin, "NAME" }
 *     port_index is the row in port_table[] above  (0=PA, 1=PB, …, 5=PF)
 * ===========================================================================*/

typedef struct {
    uint8_t     port_idx;
    uint8_t     pin;
    const char *name;
} PinAlias_t;

static const PinAlias_t alias_table[] = {
    /* ── port 2 = GPIOC ─────────────────── */
    { 2, 0, "TCK"    },   /* PC0  JTAG clock         */
    { 2, 1, "TMS"    },   /* PC1  JTAG mode select    */
    { 2, 2, "TDI"    },   /* PC2  JTAG data in        */
    { 2, 3, "TDO"    },   /* PC3  JTAG data out / SWO */
    { 2, 4, "HB_LED" },   /* PC4  heartbeat LED       */

    /* ── port 3 = GPIOD ─────────────────── */
    { 3, 6, "CLI_RX" },   /* PD6  UART2 Rx            */
    { 3, 7, "CLI_TX" },   /* PD7  UART2 Tx  (NMI pin) */
};
#define NUM_ALIASES  ( sizeof(alias_table) / sizeof(alias_table[0]) )

/* =============================================================================
 * 3.  DECODE HELPERS
 * ===========================================================================*/

/* Derive the logical mode from AMSEL, AFSEL, DIR, and DEN register bits.
 * Returns a 6-char fixed-width label to align with Nucleo411's MODER column. */
static const char *decode_mode(uint32_t amsel, uint32_t afsel,
                                uint32_t dir,   uint32_t den, uint8_t pin)
{
    if ((amsel >> pin) & 1u) return "ANALOG";
    if (!((den >> pin) & 1u)) return "DISABL";
    if ((afsel >> pin) & 1u)  return "ALT   ";
    return ((dir >> pin) & 1u) ? "OUT   " : "IN    ";
}

/* GPIOPUR / GPIOPDR bits → pull label */
static const char *decode_pull(uint32_t pur, uint32_t pdr, uint8_t pin)
{
    if ((pur >> pin) & 1u) return "PU  ";
    if ((pdr >> pin) & 1u) return "PD  ";
    return "NONE";
}

/* Search alias_table; returns pointer to name string, or "" if not found */
static const char *find_alias(uint8_t port_idx, uint8_t pin)
{
    for (uint8_t i = 0; i < NUM_ALIASES; i++) {
        if (alias_table[i].port_idx == port_idx &&
            alias_table[i].pin      == pin)
        {
            return alias_table[i].name;
        }
    }
    return "";
}

/* =============================================================================
 * 4.  PRINT ONE PIN ROW
 *
 *  Column layout matches Nucleo411/Board/gpio_dump.c (80-char terminal).
 *  PMC replaces AF; OD replaces OTYP; DATA replaces IDR; ODR column shows "--"
 *  because TM4C has no separate output latch register.
 *
 *  Col  0  PIN      6 chars   "PD6   "
 *  Col  6  NAME    16 chars   "CLI_RX         "
 *  Col 22  MODE     8 chars   "ALT   " / "IN    " / "OUT   " / "ANALOG" / "DISABL"
 *  Col 30  PMC      5 chars   "PMC1 " or "--   "  (GPIOPCTL nibble, ALT only)
 *  Col 35  OD       4 chars   "OD  " or "PP  " or "--  "  (GPIOODR)
 *  Col 40  PULL     6 chars   "NONE" / "PU  " / "PD  "
 *  Col 46  DATA     5 chars   pin level from GPIODATA[0x3FC]
 *  Col 51  ODR      5 chars   "--  "  (no separate ODR on TM4C)
 *  Col 56  FUNCTION           alias or generic label
 * ===========================================================================*/

static void print_pin_row(uint8_t port_idx, uint8_t pin,
                          uint32_t data,  uint32_t dir,
                          uint32_t afsel, uint32_t open_drain,
                          uint32_t pur,   uint32_t pdr,
                          uint32_t den,   uint32_t amsel,
                          uint32_t pctl)
{
    char buf[12];

    uint32_t am  = (amsel      >> pin)        & 1u;
    uint32_t af  = (afsel      >> pin)        & 1u;
    uint32_t de  = (den        >> pin)        & 1u;
    uint32_t d   = (dir        >> pin)        & 1u;
    uint32_t od  = (open_drain >> pin)        & 1u;
    uint8_t  dbit = (uint8_t)((data >> pin)  & 1u);
    uint8_t  pmc  = (uint8_t)((pctl >> (pin * 4u)) & 0xFu);

    const char *alias = find_alias(port_idx, pin);

    /* ── PIN ───────────── col 0 */
    snprintf(buf, sizeof(buf), "%s%u", port_table[port_idx].prefix, pin);
    print(buf);
    tabTo(6);

    /* ── NAME ──────────── col 6 */
    print(alias);
    tabTo(22);

    /* ── MODE ──────────── col 22 */
    print(decode_mode(amsel, afsel, dir, den, pin));
    tabTo(30);

    /* ── PMC ───────────── col 30  (ALT function number; only shown for ALT) */
    if (!am && de && af) {
        print("PMC");
        printDec(pmc);
    } else {
        print("--");
    }
    tabTo(35);

    /* ── OD ────────────── col 35  (open-drain; meaningful for OUT/ALT only) */
    if (!am && de && (af || d)) {
        print(od ? "OD" : "PP");
    } else {
        print("--");
    }
    tabTo(40);

    /* ── PULL ──────────── col 40 */
    print(decode_pull(pur, pdr, pin));
    tabTo(46);

    /* ── DATA ──────────── col 46  (current pin level; GPIODATA all-bits read) */
    if (!am) {
        printDec(dbit);
    } else {
        print("-");
    }
    tabTo(51);

    /* ── ODR ───────────── col 51  (no separate output latch on TM4C) */
    print("--");
    tabTo(56);

    /* ── FUNCTION ──────── col 56 */
    if (alias[0] != '\0') {
        print(alias);
    } else {
        if (am)       { print("ANALOG"); }
        else if (!de) { print("DISABL"); }
        else if (af)  { print("PMC"); printDec(pmc); print(" (unnamed)"); }
        else if (d)   { print("GPIO_OUT"); }
        else          { print("GPIO_IN"); }
    }

    maybeCr();
}

/* =============================================================================
 * 5.  gpio_dump_all()
 *     Prints every pin on every port whose RCGCGPIO clock gate is enabled.
 *     Optional CLI filter: a word typed after the command restricts output to
 *     pins whose alias or pin label (e.g. "PD6") contains the substring.
 * ===========================================================================*/

static char *pin_match = NULL;

void gpio_dump_all(void)
{
    print("PIN   NAME            MODE    PMC  OD   PULL  DATA ODR  FUNCTION\n");
    print("----------------------------------------------------------------------\n");

    pin_match = (char *)parseWord(0);

    uint32_t rcgc = RCGCGPIO_REG;

    for (uint8_t p = 0; p < NUM_PORTS; p++) {
        if (!(rcgc & (1u << port_table[p].rcgc_bit))) { continue; }

        uint32_t base = port_table[p].base;

        /* Read all registers once — guarantees a consistent snapshot */
        uint32_t data       = HWREG(base + 0x3FCu);        /* GPIODATA, all-bits mask */
        uint32_t dir        = HWREG(base + GPIO_O_DIR);
        uint32_t afsel      = HWREG(base + GPIO_O_AFSEL);
        uint32_t open_drain = HWREG(base + GPIO_O_ODR);    /* open-drain enable */
        uint32_t pur        = HWREG(base + GPIO_O_PUR);
        uint32_t pdr        = HWREG(base + GPIO_O_PDR);
        uint32_t den        = HWREG(base + GPIO_O_DEN);
        uint32_t amsel      = HWREG(base + GPIO_O_AMSEL);
        uint32_t pctl       = HWREG(base + GPIO_O_PCTL);

        bool port_printed = false;
        for (uint8_t pin = 0; pin < 8u; pin++) {
            const char *alias = find_alias(p, pin);
            if (pin_match) {
                const char *match = stristr(alias, pin_match);
                if (match == NULL) {
                    char buf[6];
                    snprintf(buf, sizeof(buf), "%s%u", port_table[p].prefix, pin);
                    match = stristr(buf, pin_match);
                    if (match == NULL) { continue; }
                }
            }
            print_pin_row(p, pin, data, dir, afsel, open_drain,
                          pur, pdr, den, amsel, pctl);
            port_printed = true;
        }

        if (port_printed) { print("\r\n"); }
    }
}
