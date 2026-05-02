// cli_transport_uart.c — UART2 / uDMA CLI transport for TIVA (TM4C123GH6PM)
//
// ── UART2 on PD6 (Rx) / PD7 (Tx), 115 200 baud, 8-N-1 ───────────────────
//
// PD7 is the NMI pin on TM4C123 and is commit-locked at reset.  The GPIO
// commit unlock sequence must run before any alternate-function or direction
// write to PD7 will take effect.
//
// ── TX: uDMA channel 13 (UART2 TX primary assignment) ────────────────────
//
// output() is called by the TimbreOS main loop (OUTPUT_BLOCKED / OUTPUT_FLUSH)
// to drain the emitq byte queue.  It copies up to TX_BUF_SIZE bytes into a
// DMA-safe staging buffer and starts a basic-mode uDMA transfer to the UART2
// data register.  The UART2 ISR's DMATX flag fires when the transfer
// completes; if emitq still has data a new transfer is kicked off immediately.
//
// ── RX: UART FIFO / timeout interrupt ────────────────────────────────────
//
// Received bytes are pushed into the CLI key queue via keyIn().

#include <stdint.h>
#include <stdbool.h>

#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "inc/hw_types.h"
#include "inc/hw_gpio.h"
#include "inc/hw_uart.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/uart.h"
#include "driverlib/udma.h"
#include "driverlib/interrupt.h"

#include "tea.h"
#include "cli.h"
#include "byteq.h"
#include "printers.h"

// ── uDMA control table ────────────────────────────────────────────────────
// Must be 1024-byte aligned; one 32-byte entry per channel × 32 channels × 2
// (primary + alternate) = 1024 bytes.
static uint8_t udma_ctrl_table[1024] __attribute__((aligned(1024)));

// ── TX staging buffer ─────────────────────────────────────────────────────
#define TX_BUF_SIZE  64u

static uint8_t   tx_buf[TX_BUF_SIZE];
static volatile bool tx_dma_busy = false;

// uDMA primary channel 13 = UART2 TX (TM4C123 Table 9-1)
#define UART2_TX_DMA_CH  13u

// ── Internal: start a DMA TX burst ───────────────────────────────────────
static void tx_start_dma(void) {
    uint32_t n = 0;
    while (n < TX_BUF_SIZE && qbq(emitq)) {
        tx_buf[n++] = pullbq(emitq);
    }
    if (n == 0) {
        tx_dma_busy = false;
        return;
    }
    tx_dma_busy = true;

    uDMAChannelTransferSet(UART2_TX_DMA_CH | UDMA_PRI_SELECT,
                           UDMA_MODE_BASIC,
                           tx_buf,
                           (void *)(UART2_BASE + UART_O_DR),
                           n);
    uDMAChannelEnable(UART2_TX_DMA_CH);
}

// ── output() — drain emitq via DMA ───────────────────────────────────────
// Called from the main loop via OUTPUT_BLOCKED / OUTPUT_FLUSH.
void output(void) {
    if (!tx_dma_busy && qbq(emitq)) {
        tx_start_dma();
    }
}

// ── UART2 ISR ─────────────────────────────────────────────────────────────
void UART2IntHandler(void) {
    uint32_t status = UARTIntStatus(UART2_BASE, true);
    UARTIntClear(UART2_BASE, status);

    // RX FIFO / idle timeout → push bytes into CLI key queue.

    if (status & (UART_INT_RX | UART_INT_RT)) {
        while (UARTCharsAvail(UART2_BASE)) {
            int32_t c = UARTCharGetNonBlocking(UART2_BASE);
            if (c >= 0) {
                keyIn((Byte)c);
            }
        }
    }

    // DMA TX done → restart if more data is waiting.
    if (status & UART_INT_DMATX) {
        tx_dma_busy = false;
        tx_start_dma();
    }
}

// ── uart_transport_init ───────────────────────────────────────────────────
void uart_transport_init(void) {
    // ── GPIO Port D: PD6 = U2Rx, PD7 = U2Tx ─────────────────────────────
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOD)) {}

    // PD7 is NMI-locked at reset — must unlock before alternate function.
    HWREG(GPIO_PORTD_BASE + GPIO_O_LOCK) = GPIO_LOCK_KEY;
    HWREG(GPIO_PORTD_BASE + GPIO_O_CR)  |= GPIO_PIN_7;
    HWREG(GPIO_PORTD_BASE + GPIO_O_LOCK) = 0;

    GPIOPinConfigure(GPIO_PD6_U2RX);
    GPIOPinConfigure(GPIO_PD7_U2TX);
    GPIOPinTypeUART(GPIO_PORTD_BASE, GPIO_PIN_6 | GPIO_PIN_7);

    // ── UART2: 115 200, 8-N-1 ────────────────────────────────────────────
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART2);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UART2)) {}
    UARTConfigSetExpClk(UART2_BASE, SysCtlClockGet(), 115200u,
                        UART_CONFIG_WLEN_8 |
                        UART_CONFIG_STOP_ONE |
                        UART_CONFIG_PAR_NONE);
    UARTFIFOEnable(UART2_BASE);
    // RX fires at 1/8 full (2 bytes); TX threshold irrelevant — DMA drives TX.
    UARTFIFOLevelSet(UART2_BASE, UART_FIFO_TX1_8, UART_FIFO_RX1_8);

    // Enable RX, RX-timeout, and DMA-TX-done interrupts.
    UARTIntEnable(UART2_BASE, UART_INT_RX | UART_INT_RT | UART_INT_DMATX);
    IntEnable(INT_UART2);

    // ── uDMA ─────────────────────────────────────────────────────────────
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UDMA);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_UDMA)) {}
    uDMAEnable();
    uDMAControlBaseSet(udma_ctrl_table);

    // Channel 13: UART2 TX, primary select, basic mode, byte transfers.
    uDMAChannelAttributeDisable(UART2_TX_DMA_CH,
                                UDMA_ATTR_ALTSELECT  |
                                UDMA_ATTR_USEBURST   |
                                UDMA_ATTR_HIGH_PRIORITY |
                                UDMA_ATTR_REQMASK);
    uDMAChannelControlSet(UART2_TX_DMA_CH | UDMA_PRI_SELECT,
                          UDMA_SIZE_8     |
                          UDMA_SRC_INC_8  |
                          UDMA_DST_INC_NONE |
                          UDMA_ARB_4);

    // Wire uDMA to UART2 TX FIFO.
    UARTDMAEnable(UART2_BASE, UART_DMA_TX);

    UARTEnable(UART2_BASE);

    when(EmitEvent, output);
}
