// cli_transport_uart.c — UART2 interrupt-driven CLI transport for TIVA (TM4C123GH6PM)
//
// ── UART2 on PD6 (Rx) / PD7 (Tx), 115 200 baud, 8-N-1 ───────────────────
//
// PD7 is the NMI pin on TM4C123 and is commit-locked at reset.  The GPIO
// commit unlock sequence must run before any alternate-function or direction
// write to PD7 will take effect.
//
// ── TX: UART2 TX FIFO / UART_INT_TX interrupt ────────────────────────────
//
// output() is called by the TimbreOS main loop (EmitEvent) whenever a byte
// is queued in emitq.  It enables UART_INT_TX to kick off the TX stream.
//
// UART_INT_TX fires while the TX FIFO is at or below the TX threshold
// (UART_FIFO_TX1_8 = 2 bytes).  The ISR pumps bytes from emitq into the
// FIFO until the FIFO is full or emitq is empty, then disables UART_INT_TX
// to avoid spurious re-fires when the queue is drained.
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
#include "driverlib/interrupt.h"

#include "tea.h"
#include "cli.h"
#include "byteq.h"
#include "printers.h"

// ── output() — prime the TX FIFO, then arm the interrupt ─────────────────
// Called via EmitEvent on every byte queued into emitq.
//
// UART_INT_TX (TXRIS) is transition-triggered: it fires when the FIFO
// *drops to* the threshold, not merely because the FIFO is already empty.
// Enabling the interrupt on an idle (empty) FIFO produces no transition and
// TXRIS never asserts.  The fix is to prime the FIFO here directly; that
// fills it, and as it drains past the threshold the ISR takes over.
void output(void) {
    // Write as many bytes as the FIFO will accept right now.
    while (qbq(emitq) && !(HWREG(UART2_BASE + UART_O_FR) & UART_FR_TXFF)) {
        HWREG(UART2_BASE + UART_O_DR) = pullbq(emitq);
    }
    // If emitq still has data, arm the TX interrupt so the ISR drains it
    // as the FIFO falls back through the threshold.
    if (qbq(emitq)) {
        UARTIntEnable(UART2_BASE, UART_INT_TX);
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

    // TX FIFO at or below threshold — pump emitq into the FIFO.
    if (status & UART_INT_TX) {
        while (qbq(emitq) && !(HWREG(UART2_BASE + UART_O_FR) & UART_FR_TXFF)) {
            HWREG(UART2_BASE + UART_O_DR) = pullbq(emitq);
        }
        if (!qbq(emitq)) {
            // Queue drained — disable TX interrupt until next output() call.
            UARTIntDisable(UART2_BASE, UART_INT_TX);
        }
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
    // TX fires at ≤ 2 bytes remaining; RX fires at ≥ 2 bytes received.
    UARTFIFOLevelSet(UART2_BASE, UART_FIFO_TX1_8, UART_FIFO_RX1_8);

    // Enable RX and RX-timeout interrupts.
    // UART_INT_TX is enabled on demand by output(); not needed at init.
    UARTIntEnable(UART2_BASE, UART_INT_RX | UART_INT_RT);
    IntEnable(INT_UART2);

    UARTEnable(UART2_BASE);

    when(EmitEvent, output);
}
