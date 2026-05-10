// lwipopts.h — lwIP compile-time configuration for TM4C / TimbreOS
//
// Tuned for:
//   - USB CDC-ECM as the sole network interface
//   - HTTP server (Robot/net/http)
//   - NO_SYS=1 (driven by net_poll() in TimbreOS run loop)
//   - 32 KB total SRAM on TM4C123GH6PM — every byte counts
//
// RAM budget (approximate):
//   PBUF pool  6 × 512  =  3 072 B
//   MEM heap            =  5 120 B
//   MEMP pools          ~  1 500 B
//   USB RX ring 2×1516  =  3 032 B
//   lwIP stats          ~    300 B
//   TimbreOS + stack    ~  8 000 B
//   HTTP conns  4×~1.5K ~  6 000 B
//   ─────────────────────────────
//   Total               ~ 27 000 B  (<32 KB)

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// ── OS / threading ────────────────────────────────────────────────────────────
#define NO_SYS                   1
#define SYS_LIGHTWEIGHT_PROT     0
#define LWIP_NETCONN             0
#define LWIP_SOCKET              0

// ── Memory ────────────────────────────────────────────────────────────────────
#define MEM_ALIGNMENT            4
#define MEM_SIZE                 5120   // 5 KB general heap
#define MEMP_MEM_INIT            1

// ── pbuf pool ─────────────────────────────────────────────────────────────────
// Default: 16 × ~1530 B = 24 KB.  Override both to fit in 32 KB SRAM.
// 512-B buffers chain automatically for frames > 512 B (lwIP handles this).
// 6 entries: enough for 2 full-size Ethernet frames (3 bufs each) in flight.
#define PBUF_POOL_SIZE           6
#define PBUF_POOL_BUFSIZE        512

// ── MEMP pool sizing ──────────────────────────────────────────────────────────
#define MEMP_NUM_PBUF            6
#define MEMP_NUM_TCP_PCB         4
#define MEMP_NUM_TCP_PCB_LISTEN  2
#define MEMP_NUM_TCP_SEG         8
#define MEMP_NUM_UDP_PCB         2
#define MEMP_NUM_SYS_TIMEOUT     12   // TCP + ARP + DHCP + AutoIP + DNS + mDNS + margin

// ── ARP ───────────────────────────────────────────────────────────────────────
#define LWIP_ARP                 1
#define ARP_TABLE_SIZE           4

// ── IP ────────────────────────────────────────────────────────────────────────
#define LWIP_IPV4                1
#define LWIP_IPV6                0
#define IP_REASSEMBLY            0
#define IP_FRAG                  0

// ── IGMP (required for mDNS multicast) ───────────────────────────────────────
#define LWIP_IGMP                    1

// ── mDNS responder (Bonjour — tiva.local) ────────────────────────────────────
#define LWIP_MDNS_RESPONDER          1
#define MDNS_MAX_SERVICES            1   // just _http._tcp
#define LWIP_NUM_NETIF_CLIENT_DATA   1   // mDNS stores per-netif state here

// ── DHCP + AutoIP ─────────────────────────────────────────────────────────────
// On a direct USB-to-Mac link there is no DHCP server, so DHCP will time out.
// AutoIP (RFC 3927) probes for a free 169.254.x.x address as fallback.
// LWIP_DHCP_AUTOIP_COOP makes dhcp_start() handle the DHCP→AutoIP transition
// automatically — no extra call needed in network_init().
#define LWIP_DHCP                1
#define DHCP_DOES_ARP_CHECK      0
#define LWIP_AUTOIP              1
#define LWIP_DHCP_AUTOIP_COOP    1

// ── DNS ───────────────────────────────────────────────────────────────────────
#define LWIP_DNS                 1

// ── TCP ───────────────────────────────────────────────────────────────────────
#define LWIP_TCP                 1
#define TCP_MSS                  536    // conservative MSS for USB path
#define TCP_WND                  (2 * TCP_MSS)
#define TCP_SND_BUF              (2 * TCP_MSS)
#define TCP_SND_QUEUELEN         6

// ── UDP / ICMP / Ethernet ─────────────────────────────────────────────────────
#define LWIP_UDP                 1
#define LWIP_ICMP                1
#define LWIP_ETHERNET            1

// ── Checksums (software; no HW offload on USB path) ──────────────────────────
#define CHECKSUM_GEN_IP          1
#define CHECKSUM_GEN_UDP         1
#define CHECKSUM_GEN_TCP         1
#define CHECKSUM_GEN_ICMP        1
#define CHECKSUM_CHECK_IP        0
#define CHECKSUM_CHECK_UDP       0
#define CHECKSUM_CHECK_TCP       0
#define CHECKSUM_CHECK_ICMP      0

// ── Netif callbacks ───────────────────────────────────────────────────────────
#define LWIP_NETIF_LINK_CALLBACK 1

// ── Random (dns.c transaction-ID randomisation) ───────────────────────────────
#include "clocks.h"
static inline uint32_t lwip_rand_impl(void) {
    static uint32_t s = 1;
    s = (uint32_t)get_ticks() ^ (s * 1664525u + 1013904223u);
    return s;
}
#define LWIP_RAND() lwip_rand_impl()

// ── Stats ─────────────────────────────────────────────────────────────────────
#define LWIP_STATS               1   // show-net reads these
#define LWIP_STATS_DISPLAY       0
#define LWIP_DEBUG               0
#define LWIP_PLATFORM_DIAG(x)    do {} while(0)

#endif // LWIPOPTS_H
