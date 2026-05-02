// cli_transport_uart.h — UART2 / uDMA CLI transport for TIVA
//
// UART2 on PD6 (Rx) / PD7 (Tx) at 115200 baud.
// Implements output() (emitq → uDMA TX) and key-input via RX interrupt.

#ifndef CLI_TRANSPORT_UART_H_
#define CLI_TRANSPORT_UART_H_

// Call once after init_tea(), before print_build_banner() / init_cli().
void uart_transport_init(void);

#endif /* CLI_TRANSPORT_UART_H_ */
