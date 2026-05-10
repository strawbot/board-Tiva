// usb_netif.h — lwIP netif backed by USB CDC-ECM on TM4C123
//
// Usage from board init:
//
//   struct netif g_netif;
//   usb_netif_init_hw();                          // clock, GPIO, USB hw setup
//   netif_add(&g_netif, &ip, &mask, &gw,
//             NULL, usb_netif_init, ethernet_input);
//   netif_set_default(&g_netif);
//
// Call usb_netif_poll(&g_netif) from the TimbreOS run loop (or via later()).
// USB0IntHandler must be registered in the interrupt vector table (startup_tm4c.s).

#ifndef USB_NETIF_H
#define USB_NETIF_H

#include "lwip/netif.h"

// ── Hardware bringup ──────────────────────────────────────────────────────────
// Configure USB clock, PD4/PD5 GPIO pins, force device mode.
// Call once before netif_add().
void usb_netif_init_hw(void);

// ── lwIP netif init callback ──────────────────────────────────────────────────
// Pass as the 'init' argument to netif_add().
err_t usb_netif_init(struct netif *netif);

// ── Run-loop pump ─────────────────────────────────────────────────────────────
// Call regularly (e.g. via later() / scheduled action) to hand received frames
// to lwIP and drive internal state. Very cheap when nothing is pending.
void usb_netif_poll(struct netif *netif);

// ── Diagnostics ───────────────────────────────────────────────────────────────
typedef enum {
    USB_ECM_RESET,       // bus reset or not yet connected
    USB_ECM_ADDRESSED,   // SET_ADDRESS received
    USB_ECM_CONFIGURED,  // SET_CONFIGURATION(1) received
    USB_ECM_ACTIVE,      // SET_INTERFACE(1,1) received — data endpoints live
} usb_ecm_status_t;

usb_ecm_status_t usb_netif_status(void);

// ISR event counters — cumulative since last usb_netif_init_hw().
typedef struct {
    uint32_t isr_total;      // total USB0 interrupts handled
    uint32_t resets;         // bus reset events
    uint32_t ep0_setups;     // EP0 SETUP packets dispatched
    uint32_t configured;     // SET_CONFIGURATION(1) received
    uint32_t setif;          // SET_INTERFACE(data,alt1) received
    uint32_t stalls;         // EP0 stalls issued (unrecognised requests)
    uint32_t ep2_tx;         // EP2 IN (TX-complete) interrupts
    uint32_t ep2_rx;         // EP2 OUT (frame received) interrupts
    uint8_t  devctl_at_init; // DEVCTL byte captured immediately after SESSION write
    uint8_t  dev_state;      // current usb_ecm_status_t value
    // Last 4 SETUP packets: [bmRequestType, bRequest, wValueL, wValueH]
    uint8_t  setup_log[4][4];
    uint8_t  setup_log_next; // index of oldest entry (ring position)
} usb_ecm_counters_t;

void usb_netif_get_counters(usb_ecm_counters_t *out);

// ── ISR ───────────────────────────────────────────────────────────────────────
// Registered in startup_tm4c.s as the USB0 interrupt handler.
// Reads USB hardware status and copies received frames into a ring buffer;
// actual lwIP processing happens in usb_netif_poll() (not in ISR context).
void USB0IntHandler(void);

#endif // USB_NETIF_H
