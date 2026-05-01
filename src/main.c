#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "pinout.h"
#include "tick_timer.h"

#define LED_PORT    GPIO_PORTC_BASE
#define LED_PIN     GPIO_PIN_4

int main(void)
{
    /* 80 MHz from PLL, 16 MHz crystal */
    SysCtlClockSet(SYSCTL_SYSDIV_2_5 | SYSCTL_USE_PLL |
                   SYSCTL_OSC_MAIN   | SYSCTL_XTAL_16MHZ);

    /* Start 1 ms hardware tick timer */
    tick_timer_init();

    /* Apply SysConfig pin assignments */
    PinoutSet();

    /* Configure LED pin as output */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOC);
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOC)) {}
    GPIOPinTypeGPIOOutput(LED_PORT, LED_PIN);

    uint32_t last_toggle = tick_ms();
    bool     led_on      = false;

    while(1)
    {
        if (tick_ms() - last_toggle >= msec(500))
        {
            last_toggle += msec(500);          /* advance by fixed step — no drift */
            led_on = !led_on;
            GPIOPinWrite(LED_PORT, LED_PIN, led_on ? LED_PIN : 0);
        }

        /* other work goes here — non-blocking */
    }
}
