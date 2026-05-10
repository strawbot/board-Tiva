// main.c — TIVA (TM4C123GH6PM) TimbreOS entry point
//
// Startup sequence:
//
//   1. SysCtlClockSet()        — 80 MHz from PLL, 16 MHz crystal
//   2. init_tea()              — TimbreOS scheduler; calls init_clocks()
//                                internally, which starts Timer0A (1 ms tick),
//                                Timer1A (delta alarm), heartbeat LED, and
//                                enables interrupts.
//   3. uart_transport_init()   — UART2 on PD6/PD7 @ 115 200, uDMA TX
//   4. print_build_banner()    — safe to print now that the transport is up
//   5. init_cli()              — TimbreOS Forth-like CLI
//   6. usb_netif_init_hw()     — USB CDC-ECM hardware bringup (GPIO, clock)
//   7. lwip_init()             — lwIP core initialisation
//   8. netif_add()             — register USB netif, static 192.168.8.2/24
//   9. http_server_init()      — Robot HTTP engine
//  10. run() loop              — cooperative scheduler; drives lwIP timers and
//                                usb_netif_poll() via later()/after()
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
#include "canary.h"
#include "printers.h"
// lwIP
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_addr.h"
#include "lwip/apps/mdns.h"
#include "netif/ethernet.h"

// TIVA port layer
#include "usb_netif.h"

// Robot network stack
#include "http_server.h"

// TimbreOS internal (not in cli.h public header)
void init_cli(void);

// ── Global lwIP netif ─────────────────────────────────────────────────────────
static struct netif g_netif;

// ── lwIP / network run-loop actions ──────────────────────────────────────────
// sys_check_timeouts() must be called regularly (every few ms) in NO_SYS=1
// mode — it drives DHCP retries, TCP keepalives, ARP expiry, etc.
// usb_netif_poll() drains the USB RX ring into lwIP.

static void net_poll(void)
{
    usb_netif_poll(&g_netif);
    sys_check_timeouts();
    after(10, net_poll);   // re-schedule every 10 ms
}

// ── Network init — deferred until after the CLI banner ───────────────────────
// Called via later() so it runs inside the TimbreOS run loop (not in main's
// linear startup, which avoids any ordering issues with init_tea / interrupts).

static void network_init(void)
{
    // lwIP core.
    lwip_init();

    // Static IP — no DHCP server on a direct USB-to-Mac link.
    // Mac side: set USB Ethernet adapter to 192.168.8.1 / 255.255.255.0 manually.
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip,   192, 168, 8, 2);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw,   192, 168, 8, 1);

    netif_add(&g_netif, &ip, &mask, &gw,
              NULL, usb_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    // HTTP server — opens TCP listener once http_server_start() is called.
    // http_server_start() should be called from a link-up callback; for now
    // we start it immediately and let it wait for connections.
    http_server_init();
    http_server_start();

    // Schedule the lwIP/USB polling action.
    namedAction(net_poll);
    later(net_poll);

    // mDNS — advertise as tiva.local + _http._tcp service for Bonjour browsers
    mdns_resp_init();
    mdns_resp_add_netif(&g_netif, "tiva", 255);
    mdns_resp_add_service(&g_netif, "TIVA-Net", "_http",
                          DNSSD_PROTO_TCP, 80, 255, NULL, NULL);

    print("net: lwIP up, static 192.168.8.2/24, mDNS tiva.local\r\n");
}

// ─────────────────────────────────────────────────────────────────────────────

int main(void) {
    // Fill unused stack with 0xDEADBEEF before any deep call chain.
    stack_canary_init();

    // 80 MHz from PLL, 16 MHz external crystal.
    SysCtlClockSet(SYSCTL_SYSDIV_2_5 | SYSCTL_USE_PLL |
                   SYSCTL_OSC_MAIN   | SYSCTL_XTAL_16MHZ);

    // TimbreOS: start scheduler, timers, heartbeat, interrupts.
    init_tea();

    // UART2 transport for CLI.
    uart_transport_init();

    // Safe to print now.
    print_build_banner();

    // Forth-like CLI.
    init_cli();

    // USB hardware bringup (GPIO PD4/PD5, clock, device mode, interrupt).
    // Must be called before network_init() schedules lwIP / netif_add().
    usb_netif_init_hw();

    // Defer lwIP and netif init into the run loop — ensures TimbreOS scheduler
    // is fully running before lwip_init() touches its timer infrastructure.
    later(network_init);
    namedAction(network_init);
    // Cooperative scheduler — never returns.
    while (1) {
        run();
    }
}
