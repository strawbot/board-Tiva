#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_nvic.h"
#include "inc/hw_types.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "pinout.h"   // SysConfig-generated

// Adjust to match your SysConfig output — e.g., PF1 for yellow
#define LED_PORT_BASE   GPIO_PORTC_BASE
#define LED_PERIPH      SYSCTL_PERIPH_GPIOC
#define LED_PIN         GPIO_PIN_4

int main(void)
{
    // 80 MHz from PLL, 16 MHz crystal
    SysCtlClockSet(SYSCTL_SYSDIV_2_5 | SYSCTL_USE_PLL |
                   SYSCTL_OSC_MAIN   | SYSCTL_XTAL_16MHZ);

    PinoutSet();   // applies all pin assignments from SysConfig

    // Then enable peripherals and configure via TivaWare driverlib directly
    SysCtlPeripheralEnable(LED_PERIPH);
    while (!SysCtlPeripheralReady(LED_PERIPH)) {}
    while(1) {
        GPIOPinTypeGPIOOutput(LED_PORT_BASE, LED_PIN);
        SysCtlDelay(SysCtlClockGet() / 3);   // ~1 s on
        GPIOPinWrite(LED_PORT_BASE, LED_PIN, 0);
        SysCtlDelay(SysCtlClockGet() / 3);   // ~1 s off
    }
}
