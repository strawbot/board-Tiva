// usb_netif.c — USB CDC-ECM network interface for TM4C123 / lwIP
//
// Implements USB CDC-ECM (Ethernet Control Model, USB class 02/06/00).
// The host sees a standard USB Ethernet adapter; no drivers needed on
// macOS or Linux. Windows 10+ also supports CDC-ECM natively.
//
// Architecture:
//   - USB0 ISR copies raw Ethernet frames from bulk-OUT FIFO into a
//     fixed ring buffer (no heap, no pbuf allocation in ISR context).
//   - usb_netif_poll() drains the ring, allocates pbufs, calls ethernet_input().
//   - lwIP low_level_output() copies a pbuf chain into the bulk-IN FIFO.
//   - EP0 state machine handles standard USB requests + CDC class requests.
//
// Endpoint layout:
//   EP0        control      64 B   (standard)
//   EP1 IN     interrupt    16 B   CDC notifications (Network Connection)
//   EP2 IN     bulk        512 B   Ethernet frames: device → host
//   EP2 OUT    bulk        512 B   Ethernet frames: host → device
//
// FIFO layout (2 KB USB SRAM, 8-byte blocks):
//   blocks  0– 7  (  64 B): EP0 (implicit)
//   blocks  8–15  (  64 B): EP1 IN  — address 8
//   blocks 16–31  ( 128 B): EP2 IN  — address 16  (64 B FS MPS; 128 B FIFO for double-buffer)
//   blocks 32–47  ( 128 B): EP2 OUT — address 32  (64 B FS MPS; 128 B FIFO for double-buffer)
//
// TX for EP2 IN: Ethernet frames up to 1514 B are sent 64 B at a time.
// Each TX-complete ISR sends the next chunk (tx_send_chunk) until done.

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

// TivaWare driverlib
#include "driverlib/usb.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_ints.h"

// lwIP
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

// TimbreOS
#include "printers.h"
#include "project_defs.h"   // ENTER_SAFE_REGION / LEAVE_SAFE_REGION

#include "usb_netif.h"

// ── Configuration ─────────────────────────────────────────────────────────────

#define ECM_MTU             1514    // max Ethernet frame (excl. preamble/FCS)
#define ECM_RX_RING_FRAMES  2       // ring buffer depth — 2 × 1516 B = 3 KB

// Statically-assigned locally-administered MAC address (U/L bit set).
// Change the last 3 bytes to make it unique per board if needed.
static const uint8_t s_mac[6] = { 0x02, 0x00, 0x54, 0x4D, 0x34, 0x43 };
                                 // 0x02 = locally administered, unicast
                                 // "TM4C" in the last bytes

// ── USB descriptor constants ──────────────────────────────────────────────────

#define USB_CLASS_CDC           0x02
#define USB_SUBCLASS_ECM        0x06
#define USB_PROTOCOL_NONE       0x00

#define CDC_CS_INTERFACE        0x24
#define CDC_CS_ENDPOINT         0x25

#define CDC_FUNC_HDR            0x00
#define CDC_FUNC_UNION          0x06
#define CDC_FUNC_ECM            0x0F

#define CDC_REQ_SET_INTERFACE           0x0B    // standard
#define CDC_REQ_SET_ETH_PACKET_FILTER   0x43    // class-specific

#define EP1_ADDR        0x81    // EP1 IN
#define EP2_IN_ADDR     0x82    // EP2 IN
#define EP2_OUT_ADDR    0x02    // EP2 OUT

// ── Descriptor tables ─────────────────────────────────────────────────────────
//
// String 4 is the MAC address in ASCII hex — "02005454D344343" style.
// CDC-ECM spec §5.4: iMACAddress field points to a string of exactly 12
// uppercase hex characters representing the 6-byte MAC.

// String 4: MAC address as 12 ASCII hex chars (UTF-16LE in USB string descriptor)
// MAC 02:00:54:4D:34:43 → iMACAddress string "0200544D3443" (12 chars, 24 bytes data)
// bLength = 2 (header) + 24 (data) = 26.
// USB strings are UTF-16LE: each ASCII character is the char byte followed by 0x00.
static const uint8_t s_str_mac[] = {
    26, 0x03,   // bLength=26, bDescriptorType=STRING
    '0',0, '2',0, '0',0, '0',0, '5',0, '4',0,
    '4',0, 'D',0, '3',0, '4',0, '4',0, '3',0,
};

static const uint8_t s_str_lang[]    = { 4, 0x03, 0x09, 0x04 };          // English
static const uint8_t s_str_mfr[]     = { 18, 0x03,
    'T',0, 'i',0, 'm',0, 'b',0, 'r',0, 'e',0, 'W',0, 'S',0 };
static const uint8_t s_str_product[] = { 18, 0x03,
    'T',0, 'I',0, 'V',0, 'A',0, '-',0, 'N',0, 'e',0, 't',0 };

static const uint8_t *s_strings[] = {
    s_str_lang,     // index 0
    s_str_mfr,      // index 1
    s_str_product,  // index 2
    NULL,           // index 3 (serial — not used)
    s_str_mac,      // index 4 — iMACAddress
};

// Device descriptor
static const uint8_t s_dev_desc[] = {
    18, 0x01,           // bLength, bDescriptorType=DEVICE
    0x00, 0x02,         // bcdUSB 2.00
    0xEF,               // bDeviceClass (Misc — required with IAD)
    0x02,               // bDeviceSubClass
    0x01,               // bDeviceProtocol (IAD)
    64,                 // bMaxPacketSize0
    0xA0, 0x04,         // idVendor  (0x04A0 — placeholder)
    0xEC, 0x0E,         // idProduct (0x0EEC — placeholder)
    0x00, 0x01,         // bcdDevice 1.00
    1, 2, 0,            // iManufacturer, iProduct, iSerialNumber
    1,                  // bNumConfigurations
};

// Full configuration descriptor (config + IAD + interfaces + endpoints)
static const uint8_t s_cfg_desc[] = {
    // Configuration descriptor
    9, 0x02,            // bLength, bDescriptorType=CONFIG
    88, 0,              // wTotalLength (9+8+9+5+5+13+7+9+9+7+7 = 88)
    2,                  // bNumInterfaces (control + data)
    1,                  // bConfigurationValue
    0,                  // iConfiguration
    0xC0,               // bmAttributes (self-powered, no remote wakeup)
    50,                 // bMaxPower (100 mA)

    // IAD (Interface Association Descriptor)
    8, 0x0B,            // bLength, bDescriptorType=IAD
    0,                  // bFirstInterface
    2,                  // bInterfaceCount
    USB_CLASS_CDC,      // bFunctionClass
    USB_SUBCLASS_ECM,   // bFunctionSubClass
    USB_PROTOCOL_NONE,  // bFunctionProtocol
    0,                  // iFunction

    // Interface 0 — CDC Control
    9, 0x04,            // bLength, bDescriptorType=INTERFACE
    0,                  // bInterfaceNumber
    0,                  // bAlternateSetting
    1,                  // bNumEndpoints (notification EP)
    USB_CLASS_CDC,      // bInterfaceClass
    USB_SUBCLASS_ECM,   // bInterfaceSubClass
    USB_PROTOCOL_NONE,  // bInterfaceProtocol
    0,                  // iInterface

    // CDC Header Functional Descriptor
    5, CDC_CS_INTERFACE, CDC_FUNC_HDR, 0x10, 0x01,   // bcdCDC 1.10

    // CDC Union Functional Descriptor
    5, CDC_CS_INTERFACE, CDC_FUNC_UNION,
    0,                  // bControlInterface
    1,                  // bSubordinateInterface0

    // CDC Ethernet Functional Descriptor
    13, CDC_CS_INTERFACE, CDC_FUNC_ECM,
    4,                  // iMACAddress (string index 4)
    0, 0, 0, 0,         // bmEthernetStatistics (none)
    0xEA, 0x05,         // wMaxSegmentSize = 1514
    0, 0,               // wNumberMCFilters
    0,                  // bNumberPowerFilters

    // EP1 IN — interrupt (notification)
    7, 0x05,            // bLength, bDescriptorType=ENDPOINT
    EP1_ADDR,           // bEndpointAddress (EP1 IN)
    0x03,               // bmAttributes (interrupt)
    16, 0,              // wMaxPacketSize
    255,                // bInterval (255 ms)

    // Interface 1, alt 0 — CDC Data (no endpoints — required by spec)
    9, 0x04,
    1, 0, 0,            // bInterfaceNumber=1, bAlternateSetting=0, bNumEndpoints=0
    0x0A, 0x00, 0x00, 0,

    // Interface 1, alt 1 — CDC Data (bulk endpoints active)
    9, 0x04,
    1, 1, 2,            // bInterfaceNumber=1, bAlternateSetting=1, bNumEndpoints=2
    0x0A, 0x00, 0x00, 0,

    // EP2 IN — bulk, Ethernet frames device→host
    7, 0x05,
    EP2_IN_ADDR,        // bEndpointAddress (EP2 IN)
    0x02,               // bmAttributes (bulk)
    0x40, 0x00,         // wMaxPacketSize = 64 (FS bulk max)
    0,                  // bInterval (bulk: ignored)

    // EP2 OUT — bulk, Ethernet frames host→device
    7, 0x05,
    EP2_OUT_ADDR,       // bEndpointAddress (EP2 OUT)
    0x02,               // bmAttributes (bulk)
    0x40, 0x00,         // wMaxPacketSize = 64 (FS bulk max)
    0,                  // bInterval
};

// ── EP0 state machine ─────────────────────────────────────────────────────────

typedef enum {
    EP0_IDLE,
    EP0_TX,             // sending descriptor/response in chunks
    EP0_RX,             // receiving data (SET_ETH_PACKET_FILTER body)
    EP0_STALL,
} ep0_state_t;

static ep0_state_t   s_ep0_state;
static const uint8_t *s_ep0_tx_ptr;
static uint16_t       s_ep0_tx_remaining;
static uint16_t       s_ep0_tx_requested;   // what the host asked for

// ── Device state ──────────────────────────────────────────────────────────────

typedef enum {
    DEV_RESET,
    DEV_ADDRESSED,
    DEV_CONFIGURED,
    DEV_ACTIVE,         // interface 1 alt 1 selected — data endpoints live
} dev_state_t;

static dev_state_t  s_dev_state;
static struct netif *s_netif;       // registered lwIP interface

// ── ISR event counters (read by debug-usb, never reset after init) ────────────

static volatile uint32_t s_isr_total;       // every USB0 interrupt
static volatile uint32_t s_cnt_reset;       // USB bus resets seen
static volatile uint32_t s_cnt_ep0;         // EP0 SETUP packets dispatched
static volatile uint32_t s_cnt_configured;  // SET_CONFIGURATION(1) received
static volatile uint32_t s_cnt_setif;       // SET_INTERFACE(data, alt1) received
static volatile uint32_t s_cnt_stall;       // EP0 stalls issued (unhandled requests)
static volatile uint32_t s_cnt_ep2tx;       // EP2 IN (TX-complete) interrupts
static volatile uint32_t s_cnt_ep2rx;       // EP2 OUT (frame received) interrupts

// Deferred SET_ADDRESS: USB spec §9.4.6 requires the device to complete the
// STATUS phase (ZLP) still responding at address 0, then switch to the new
// address.  We store the new address here and apply it in the EP0 TX-complete
// interrupt (i.e. after the host ACKs the STATUS ZLP), not in handle_setup().
static volatile uint8_t  s_pending_addr;    // 0 = no pending change

// Ring of last 4 SETUP packets seen — each entry is {bmRequestType, bRequest, wValueL, wValueH}
#define SETUP_LOG_DEPTH  4
static volatile uint8_t  s_setup_log[SETUP_LOG_DEPTH][4];
static volatile uint8_t  s_setup_log_idx;   // next write position (wraps)
static          uint8_t  s_devctl_at_init; // DEVCTL snapshotted right after SESSION write

// ── RX ring buffer (populated in ISR, drained in poll) ────────────────────────

#define RING_FRAME_SIZE  (ECM_MTU + 4)  // +4 for 32-bit length header

typedef struct {
    uint16_t  len;
    uint8_t   data[ECM_MTU];
} rx_frame_t;

static rx_frame_t  s_rx_ring[ECM_RX_RING_FRAMES];
static volatile uint8_t s_rx_head;   // written by ISR
static volatile uint8_t s_rx_tail;   // read   by poll

// ── TX state ─────────────────────────────────────────────────────────────────
// Full Speed bulk max packet = 64 bytes.  Ethernet frames up to 1514 bytes
// require up to 24 IN transactions.  We copy the frame into s_tx_buf once and
// send it 64 bytes at a time; each TX-complete interrupt sends the next chunk.

#define EP2_MPS  64u

static volatile bool     s_tx_busy;          // true while EP2 IN FIFO is draining
static          uint8_t  s_tx_buf[ECM_MTU];  // flat copy of the frame being sent
static          uint16_t s_tx_len;           // total bytes to send
static          uint16_t s_tx_pos;           // next byte to send

// ── Helpers ───────────────────────────────────────────────────────────────────

static void ep0_send(const uint8_t *data, uint16_t len, uint16_t requested)
{
    s_ep0_tx_ptr       = data;
    s_ep0_tx_remaining = len;
    s_ep0_tx_requested = requested;
    s_ep0_state        = EP0_TX;

    // Send first chunk (up to 64 bytes).
    uint16_t chunk = (len > 64u) ? 64u : len;
    USBEndpointDataPut(USB0_BASE, USB_EP_0, (uint8_t *)data, chunk);
    s_ep0_tx_ptr       += chunk;
    s_ep0_tx_remaining -= chunk;

    if (s_ep0_tx_remaining == 0) {
        // All data fits in this one packet.  TRANS_IN_LAST sets the DATAEND
        // bit, telling the MUSB controller the data phase is complete so it
        // will accept the host's status-phase OUT ZLP.  Without DATAEND the
        // controller stays in the data phase and NAKs the status ZLP forever.
        USBEndpointDataSend(USB0_BASE, USB_EP_0, USB_TRANS_IN_LAST);
        s_ep0_state = EP0_IDLE;
    } else {
        USBEndpointDataSend(USB0_BASE, USB_EP_0, USB_TRANS_IN);
    }
}

static void ep0_zlp(void)
{
    USBEndpointDataPut(USB0_BASE, USB_EP_0, NULL, 0);
    USBEndpointDataSend(USB0_BASE, USB_EP_0, USB_TRANS_IN_LAST);
    s_ep0_state = EP0_IDLE;
}

static void ep0_stall(void)
{
    s_cnt_stall++;
    USBDevEndpointStall(USB0_BASE, USB_EP_0, USB_EP_DEV_IN);
    s_ep0_state = EP0_IDLE;
}

// ── CDC Network Connection notification ──────────────────────────────────────
// CDC spec §6.3.1. Sent on EP1 IN to tell the host whether the link is up.

static const uint8_t s_notif_connected[] = {
    0xA1,           // bmRequestType (class, interface, device-to-host)
    0x00,           // bNotificationCode = NETWORK_CONNECTION
    0x01, 0x00,     // wValue = 1 (connected)
    0x00, 0x00,     // wIndex = interface 0
    0x00, 0x00,     // wLength = 0
};
static const uint8_t s_notif_disconnected[] = {
    0xA1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void send_network_notification(bool connected)
{
    const uint8_t *notif = connected ? s_notif_connected : s_notif_disconnected;
    USBEndpointDataPut(USB0_BASE, USB_EP_1, (uint8_t *)notif, 8);
    USBEndpointDataSend(USB0_BASE, USB_EP_1, USB_TRANS_IN);
}

// ── Endpoint configuration helpers ───────────────────────────────────────────

static void data_endpoints_enable(void)
{
    // EP2 IN — bulk, 64 B packet (FS max), 128 B FIFO (double-buffer) at block 16
    USBFIFOConfigSet(USB0_BASE, USB_EP_2, 16, USB_FIFO_SZ_128, USB_EP_DEV_IN);
    USBDevEndpointConfigSet(USB0_BASE, USB_EP_2, 64,
                            USB_EP_DEV_IN | USB_EP_MODE_BULK);

    // EP2 OUT — bulk, 64 B packet (FS max), 128 B FIFO (double-buffer) at block 32
    USBFIFOConfigSet(USB0_BASE, USB_EP_2, 32, USB_FIFO_SZ_128, USB_EP_DEV_OUT);
    USBDevEndpointConfigSet(USB0_BASE, USB_EP_2, 64,
                            USB_EP_DEV_OUT | USB_EP_MODE_BULK);

    USBIntEnableEndpoint(USB0_BASE,
                         USB_INTEP_DEV_IN_2 | USB_INTEP_DEV_OUT_2);
    s_tx_busy = false;
}

static void data_endpoints_disable(void)
{
    USBIntDisableEndpoint(USB0_BASE,
                          USB_INTEP_DEV_IN_2 | USB_INTEP_DEV_OUT_2);
    USBFIFOFlush(USB0_BASE, USB_EP_2, USB_EP_DEV_IN);
    USBFIFOFlush(USB0_BASE, USB_EP_2, USB_EP_DEV_OUT);
    s_tx_busy = false;
}

// ── EP0 setup-packet handler ──────────────────────────────────────────────────

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

static void handle_setup(void)
{
    uint8_t      buf[8];
    uint32_t     len = 8;
    usb_setup_t *s   = (usb_setup_t *)buf;

    USBEndpointDataGet(USB0_BASE, USB_EP_0, buf, &len);
    USBDevEndpointDataAck(USB0_BASE, USB_EP_0, false);

    // Guard: a STATUS-phase ZLP from the host looks like an OUT_PKTRDY event
    // on some MUSB revisions.  It arrives with len=0 and is not a real SETUP.
    if (len < 8) { return; }

    // Log this SETUP for post-mortem diagnostics.
    {
        uint8_t i = s_setup_log_idx;
        s_setup_log[i][0] = buf[0];   // bmRequestType
        s_setup_log[i][1] = buf[1];   // bRequest
        s_setup_log[i][2] = buf[2];   // wValue low
        s_setup_log[i][3] = buf[3];   // wValue high
        s_setup_log_idx   = (i + 1u) % SETUP_LOG_DEPTH;
    }

    uint8_t  type  = s->bmRequestType & 0x60;   // bits 6:5
    uint8_t  req   = s->bRequest;
    uint16_t val   = s->wValue;
    uint16_t idx   = s->wIndex;
    uint16_t reqlen = s->wLength;

    // ── Standard requests ──────────────────────────────────────────────────

    if (type == 0x00) {   // Standard
        switch (req) {
        case 0x05:  // SET_ADDRESS
            // Do NOT call USBDevAddrSet here.  The STATUS ZLP must be sent
            // while the device still answers at address 0 (the address the
            // host used for this entire control transfer).  Applying the new
            // address before ep0_zlp() causes the ZLP to be sent from the
            // new address; the host's STATUS IN token targets address 0 and
            // gets no response → timeout → reset → endless SET_ADDRESS loop.
            // We defer the write to FADDR until the TX-complete interrupt for
            // the ZLP fires (see the s_pending_addr check in USB0IntHandler).
            s_pending_addr = val & 0x7Fu;
            s_dev_state    = DEV_ADDRESSED;
            ep0_zlp();
            return;

        case 0x09:  // SET_CONFIGURATION
            if (val == 1) {
                s_cnt_configured++;
                s_dev_state = DEV_CONFIGURED;
                // EP1 IN — interrupt, 64 B FIFO at block 8
                USBFIFOConfigSet(USB0_BASE, USB_EP_1, 8, USB_FIFO_SZ_64,
                                 USB_EP_DEV_IN);
                USBDevEndpointConfigSet(USB0_BASE, USB_EP_1, 16,
                                        USB_EP_DEV_IN | USB_EP_MODE_INT);
                USBIntEnableEndpoint(USB0_BASE, USB_INTEP_DEV_IN_1);
            } else {
                s_dev_state = DEV_ADDRESSED;
                data_endpoints_disable();
            }
            ep0_zlp();
            return;

        case 0x0B:  // SET_INTERFACE
            if (idx == 1) {  // data interface
                if (val == 1) {
                    s_cnt_setif++;
                    data_endpoints_enable();
                    s_dev_state = DEV_ACTIVE;
                    send_network_notification(true);
                    if (s_netif) netif_set_link_up(s_netif);
                } else {
                    data_endpoints_disable();
                    s_dev_state = DEV_CONFIGURED;
                    send_network_notification(false);
                    if (s_netif) netif_set_link_down(s_netif);
                }
            }
            ep0_zlp();
            return;

        case 0x00:  // GET_STATUS
            { static const uint8_t z[2] = {0,0};
              ep0_send(z, 2, reqlen); }
            return;

        case 0x06:  // GET_DESCRIPTOR
            switch (val >> 8) {
            case 0x01:  // DEVICE
                ep0_send(s_dev_desc,
                         sizeof(s_dev_desc) < reqlen ? sizeof(s_dev_desc) : reqlen,
                         reqlen);
                return;
            case 0x02:  // CONFIGURATION
                ep0_send(s_cfg_desc,
                         sizeof(s_cfg_desc) < reqlen ? sizeof(s_cfg_desc) : reqlen,
                         reqlen);
                return;
            case 0x03:  // STRING
                { uint8_t sidx = val & 0xFF;
                  if (sidx < sizeof(s_strings)/sizeof(s_strings[0])
                      && s_strings[sidx]) {
                      uint16_t slen = s_strings[sidx][0];
                      ep0_send(s_strings[sidx],
                               slen < reqlen ? slen : reqlen,
                               reqlen);
                      return;
                  }
                }
                break;
            }
            ep0_stall();
            return;

        case 0x08:  // GET_CONFIGURATION
            { uint8_t c = (s_dev_state >= DEV_CONFIGURED) ? 1 : 0;
              ep0_send(&c, 1, reqlen); }
            return;

        case 0x0A:  // GET_INTERFACE
            { uint8_t alt = (s_dev_state == DEV_ACTIVE) ? 1 : 0;
              ep0_send(&alt, 1, reqlen); }
            return;

        default:
            ep0_stall();
            return;
        }
    }

    // ── CDC class requests ────────────────────────────────────────────────

    if (type == 0x20) {   // Class
        switch (req) {
        case CDC_REQ_SET_ETH_PACKET_FILTER:
            // Host sets multicast/broadcast filter bits.
            // We accept all frames; just ACK.
            ep0_zlp();
            return;

        default:
            ep0_stall();
            return;
        }
    }

    ep0_stall();
}

// ── EP0 continuation (TX data phase) ─────────────────────────────────────────

static void handle_ep0_in(void)
{
    if (s_ep0_state != EP0_TX) return;

    // ep0_send already handles the single-packet case (sets IDLE + TRANS_IN_LAST).
    // We only arrive here for multi-packet responses where remaining > 0.
    if (s_ep0_tx_remaining == 0) {
        s_ep0_state = EP0_IDLE;
        return;
    }

    uint16_t chunk = (s_ep0_tx_remaining > 64u) ? 64u : s_ep0_tx_remaining;
    USBEndpointDataPut(USB0_BASE, USB_EP_0, (uint8_t *)s_ep0_tx_ptr, chunk);
    s_ep0_tx_ptr       += chunk;
    s_ep0_tx_remaining -= chunk;

    if (s_ep0_tx_remaining == 0) {
        // Last packet — set DATAEND so the controller can accept the status ZLP.
        USBEndpointDataSend(USB0_BASE, USB_EP_0, USB_TRANS_IN_LAST);
        s_ep0_state = EP0_IDLE;
    } else {
        USBEndpointDataSend(USB0_BASE, USB_EP_0, USB_TRANS_IN);
    }
}

// ── TX chunk sender (called from ISR and from low_level_output) ───────────────

static void tx_send_chunk(void)
{
    uint16_t remaining = s_tx_len - s_tx_pos;
    if (remaining == 0) {
        s_tx_busy = false;
        return;
    }
    uint16_t chunk = (remaining > EP2_MPS) ? EP2_MPS : remaining;
    USBEndpointDataPut(USB0_BASE, USB_EP_2, s_tx_buf + s_tx_pos, chunk);
    USBEndpointDataSend(USB0_BASE, USB_EP_2, USB_TRANS_IN);
    s_tx_pos += chunk;
    // s_tx_busy stays true until all chunks (and optional ZLP) are sent.
}

// ── USB0 ISR ─────────────────────────────────────────────────────────────────
// Runs at interrupt priority. Keeps work minimal:
//   - EP0: parse SETUP, drive TX continuation
//   - EP2 OUT: copy frame bytes into ring buffer
//   - EP2 IN: clear tx_busy flag
//   - Reset: reconfigure endpoints

void USB0IntHandler(void)
{
    s_isr_total++;

    uint32_t ctrl = USBIntStatusControl(USB0_BASE);
    uint32_t ep   = USBIntStatusEndpoint(USB0_BASE);

    // ── Bus reset ─────────────────────────────────────────────────────────
    if (ctrl & USB_INTCTRL_RESET) {
        s_cnt_reset++;
        s_dev_state        = DEV_RESET;
        s_ep0_state        = EP0_IDLE;
        s_ep0_tx_remaining = 0;
        s_pending_addr     = 0;   // cancel any deferred address change
        s_tx_busy          = false;
        s_rx_head          = s_rx_tail = 0;
        if (s_netif) {
            netif_set_link_down(s_netif);
        }
        return;
    }

    // ── EP0 ───────────────────────────────────────────────────────────────
    if (ep & USB_INTEP_0) {
        uint32_t st = USBEndpointStatus(USB0_BASE, USB_EP_0);

        if (st & USB_DEV_EP0_SETUP_END) {
            // Host abandoned the previous transfer (sent a new SETUP early).
            // Cancel any deferred address change — the SET_ADDRESS that
            // requested it was never completed.
            s_pending_addr = 0;
            USBDevEndpointStatusClear(USB0_BASE, USB_EP_0, USB_DEV_EP0_SETUP_END);
            s_ep0_state = EP0_IDLE;
        }

        // Apply deferred SET_ADDRESS now that the STATUS ZLP has been ACKed
        // (or a new SETUP arrived — either way the old STATUS phase is done).
        // This must happen BEFORE handle_setup() so the device responds to
        // the host's next request at the correct (new) address.
        if (s_pending_addr != 0) {
            USBDevAddrSet(USB0_BASE, s_pending_addr);
            s_pending_addr = 0;
        }

        if (st & USB_DEV_EP0_OUT_PKTRDY) {
            s_cnt_ep0++;
            handle_setup();
        } else if (s_ep0_state == EP0_TX) {
            handle_ep0_in();
        }
    }

    // ── EP1 IN (notification sent — nothing to do) ────────────────────────
    // ep & USB_INTEP_DEV_IN_1: ACK received, notification delivered.

    // ── EP2 IN (TX complete) ──────────────────────────────────────────────
    if (ep & USB_INTEP_DEV_IN_2) {
        s_cnt_ep2tx++;
        if (s_tx_pos < s_tx_len) {
            tx_send_chunk();   // more chunks remain — keep sending
        } else {
            // All chunks sent.  If the last chunk was exactly EP2_MPS bytes
            // the host can't tell the transfer is done (it looks like more
            // data might follow).  Send a ZLP to terminate the transfer.
            if ((s_tx_len % EP2_MPS) == 0) {
                USBEndpointDataPut(USB0_BASE, USB_EP_2, NULL, 0);
                USBEndpointDataSend(USB0_BASE, USB_EP_2, USB_TRANS_IN);
                // ZLP TX-complete will fire once more; s_tx_pos == s_tx_len
                // so we hit the else branch and clear s_tx_busy then.
            } else {
                s_tx_busy = false;
            }
        }
    }

    // ── EP2 OUT (frame received from host) ────────────────────────────────
    if (ep & USB_INTEP_DEV_OUT_2) {
        s_cnt_ep2rx++;
        uint32_t avail = USBEndpointDataAvail(USB0_BASE, USB_EP_2);
        if (avail > 0 && avail <= ECM_MTU) {
            uint8_t next = (s_rx_head + 1) % ECM_RX_RING_FRAMES;
            if (next != s_rx_tail) {    // ring not full
                rx_frame_t *f = &s_rx_ring[s_rx_head];
                uint32_t    sz = avail;
                USBEndpointDataGet(USB0_BASE, USB_EP_2, f->data, &sz);
                USBDevEndpointDataAck(USB0_BASE, USB_EP_2, false);
                f->len    = (uint16_t)sz;
                s_rx_head = next;
            } else {
                // Ring full — drop: re-arm OUT without reading
                USBDevEndpointDataAck(USB0_BASE, USB_EP_2, false);
            }
        } else if (avail > ECM_MTU) {
            // Oversized frame — discard
            USBFIFOFlush(USB0_BASE, USB_EP_2, USB_EP_DEV_OUT);
            USBDevEndpointDataAck(USB0_BASE, USB_EP_2, false);
        }
    }
}

// ── lwIP low_level_output — send a pbuf chain as one Ethernet frame ───────────

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
    (void)netif;

    if (s_dev_state != DEV_ACTIVE) return ERR_IF;
    if (s_tx_busy)                 return ERR_WOULDBLOCK;

    // Flatten the pbuf chain into the flat TX buffer.
    uint16_t total = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if (total + q->len > sizeof(s_tx_buf)) return ERR_MEM;
        memcpy(s_tx_buf + total, q->payload, q->len);
        total += q->len;
    }
    if (total == 0) return ERR_OK;

    s_tx_len  = total;
    s_tx_pos  = 0;
    s_tx_busy = true;
    tx_send_chunk();   // sends first 64-byte chunk; ISR continues the rest
    return ERR_OK;
}

// ── usb_netif_poll — called from run loop, drains RX ring ───────────────────

void usb_netif_poll(struct netif *netif)
{
    // GPCS guard: re-assert forced device mode if the OTG machine cleared it.
    if (!(HWREG(USB0_BASE + 0x041Cu) & 0x01u)) {
        USBDevMode(USB0_BASE);
        USBDevConnect(USB0_BASE);
    }

    while (s_rx_tail != s_rx_head) {
        rx_frame_t *f    = &s_rx_ring[s_rx_tail];
        struct pbuf *p   = pbuf_alloc(PBUF_RAW, f->len, PBUF_POOL);
        if (p) {
            pbuf_take(p, f->data, f->len);
            if (netif->input(p, netif) != ERR_OK)
                pbuf_free(p);
        }
        // Advance tail even if alloc failed — don't stall the ring.
        ENTER_SAFE_REGION();
        s_rx_tail = (s_rx_tail + 1) % ECM_RX_RING_FRAMES;
        LEAVE_SAFE_REGION();
    }
}

// ── lwIP netif init callback ─────────────────────────────────────────────────

err_t usb_netif_init(struct netif *netif)
{
    s_netif = netif;

    netif->linkoutput = low_level_output;
    netif->output     = etharp_output;
    netif->mtu        = 1500;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                      | NETIF_FLAG_ETHERNET;

    SMEMCPY(netif->hwaddr, s_mac, 6);
    netif->hwaddr_len = 6;

#if LWIP_NETIF_HOSTNAME
    netif->hostname = "tiva";
#endif

    return ERR_OK;
}

// ── usb_netif_init_hw — one-time hardware setup ──────────────────────────────

void usb_netif_init_hw(void)
{
    // Power up the USB PLL BEFORE enabling the USB peripheral.
    //
    // On TM4C123 the USB PHY 60 MHz clock comes from the USB PLL, which is a
    // separate power domain from the APB register bus clock gated by RCGCUSB.
    // SysCtlPeripheralEnable(USB0) only sets RCGCUSB — it gates APB register
    // access but does NOT start the USB PLL.  Without the PLL the entire USB
    // analog domain is dead: VBUS comparator always reads 0, D+ pull-up has no
    // drive, and FS differential drivers never switch.
    //
    // SYSCTL_RCC2 at 0x400FE070, bit 14 = USBPWRDN:
    //   1 (reset default) → USB PLL is powered down
    //   0                 → USB PLL operates normally
    //
    // The TI DFU bootloader (bl_usbfuncs.c, USBBLInit) does exactly this:
    //   HWREG(SYSCTL_RCC2) &= ~SYSCTL_RCC2_USBPWRDN;
    //
    // NOTE: 0x400FE7C8 (USBCC) does NOT exist on TM4C123 — that is a
    // TM4C129-only register.  Writing it on TM4C123 has no effect.
    HWREG(0x400FE070u) &= ~0x00004000u;   // SYSCTL_RCC2: clear USBPWRDN — power up USB PLL

    // Enable USB peripheral and GPIO clocks, then wait for each to be ready.
    //
    // IMPORTANT: SysCtlDelay(3) is NOT sufficient here — 3 cycles is ~38 ns at
    // 80 MHz, which is not enough for the USB peripheral to start its 60 MHz PLL
    // clock.  Use SysCtlPeripheralReady() (polls PRSUSB/PRSGIO) just like
    // clocks.c does for Timer0A and Timer1A.
    SysCtlPeripheralEnable(SYSCTL_PERIPH_USB0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_USB0)) {}
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB)) {}
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOD)) {}

    // PD4 = USB0DM (D−), PD5 = USB0DP (D+): must be in USB analog mode so the
    // PHY drives them directly (digital input buffer off, analog path to PHY on).
    GPIOPinTypeUSBAnalog(GPIO_PORTD_BASE, GPIO_PIN_4 | GPIO_PIN_5);

    // PB0 = USB0ID, PB1 = USB0VBUS: must be in USB analog mode so the OTG VBUS
    // comparator can sense the supply voltage.  Without AMSEL=1 on PB1 the
    // comparator input is disconnected and DEVCTL.VBUS reads 0 permanently.
    // TM4C123GH6PM data sheet §14.3.2: "Configure PB0 and PB1 using
    // GPIOPinTypeUSBAnalog()."
    GPIOPinTypeUSBAnalog(GPIO_PORTB_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    // Force device mode — bypasses OTG ID-pin and VBUS sensing so SOFTCONN
    // is the sole control over the D+ pull-up.  USBDevMode() writes
    // DEVMOD|DEVMODOTG (0x03) to GPCS (offset 0x41C).
    //
    // NOTE: Do NOT call USBClockEnable on TM4C123. That function is for
    // TM4C129; on TM4C123 it switches USB from PLL (60 MHz) to ~16 MHz IOSC.
    USBDevMode(USB0_BASE);   // GPCS = 0x03

    // Snapshot DEVCTL for debug-usb.
    s_devctl_at_init = HWREGB(USB0_BASE + 0x060u);

    // Initialise EP0 state
    s_ep0_state        = EP0_IDLE;
    s_ep0_tx_remaining = 0;
    s_dev_state        = DEV_RESET;
    s_rx_head          = 0;
    s_rx_tail          = 0;
    s_tx_busy          = false;
    s_netif            = NULL;

    // Zero ISR counters
    s_isr_total      = 0;
    s_cnt_reset      = 0;
    s_cnt_ep0        = 0;
    s_cnt_configured = 0;
    s_cnt_setif      = 0;
    s_cnt_stall      = 0;
    s_cnt_ep2tx      = 0;
    s_cnt_ep2rx      = 0;
    s_pending_addr   = 0;
    s_setup_log_idx  = 0;
    for (uint8_t i = 0; i < SETUP_LOG_DEPTH; i++) {
        s_setup_log[i][0] = s_setup_log[i][1] =
        s_setup_log[i][2] = s_setup_log[i][3] = 0;
    }

    // Enable control and endpoint interrupts
    USBIntEnableControl(USB0_BASE,
                        USB_INTCTRL_RESET   |
                        USB_INTCTRL_SUSPEND |
                        USB_INTCTRL_RESUME  |
                        USB_INTCTRL_DISCONNECT);
    USBIntEnableEndpoint(USB0_BASE, USB_INTEP_0);

    IntEnable(INT_USB0);

    // TM4C123 is Full Speed only.  HSENAB (POWER bit 5) is set to 1 by the
    // USB peripheral reset default and enables the HS chirp state machine.
    // On an FS-only PHY the chirp sequence never completes, which prevents
    // the PHY from asserting the D+ pull-up as an FS device.  Clear it
    // explicitly before asserting SOFTCONN.
    HWREGB(USB0_BASE + 0x001u) &= ~0x20u;   // POWER: clear HSENAB (bit 5) — TM4C123 is FS-only

    // Attach to bus — host will see us and start enumeration
    USBDevConnect(USB0_BASE);
    print("USB: CDC-ECM attached\r\n");
}

// ── Diagnostics ───────────────────────────────────────────────────────────────

usb_ecm_status_t usb_netif_status(void)
{
    return (usb_ecm_status_t)s_dev_state;
}

void usb_netif_get_counters(usb_ecm_counters_t *out)
{
    out->isr_total      = s_isr_total;
    out->resets         = s_cnt_reset;
    out->ep0_setups     = s_cnt_ep0;
    out->configured     = s_cnt_configured;
    out->setif          = s_cnt_setif;
    out->stalls         = s_cnt_stall;
    out->ep2_tx         = s_cnt_ep2tx;
    out->ep2_rx         = s_cnt_ep2rx;
    out->devctl_at_init = s_devctl_at_init;
    out->dev_state      = (uint8_t)s_dev_state;
    out->setup_log_next = s_setup_log_idx;
    for (uint8_t i = 0; i < SETUP_LOG_DEPTH; i++) {
        out->setup_log[i][0] = s_setup_log[i][0];
        out->setup_log[i][1] = s_setup_log[i][1];
        out->setup_log[i][2] = s_setup_log[i][2];
        out->setup_log[i][3] = s_setup_log[i][3];
    }
}
