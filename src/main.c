#include <stdint.h>
#include "tm4c123gh6pm.h"

#define SYSCTL_RCGCGPIO_R       (*((volatile uint32_t *)0x400FE608))
#define GPIO_PORTF_DIR_R        (*((volatile uint32_t *)0x40025400 + (0x400/4)))
#define GPIO_PORTF_DEN_R        (*((volatile uint32_t *)0x4002551C))
#define GPIO_PORTF_DATA_R       (*((volatile uint32_t *)0x400253FC))

void delay(volatile uint32_t count) {
    while (count--) { __asm__("nop"); }
}

int main(void) {
    /* Enable GPIOF clock */
    SYSCTL_RCGCGPIO_R |= 0x20;
    /* small delay for clock to stabilize */
    for (volatile int i = 0; i < 1000; i++) { __asm__("nop"); }

    /* PF1 as output (red LED on many Tiva boards) */
    GPIO_PORTF_DIR_R |= (1 << 1);
    GPIO_PORTF_DEN_R |= (1 << 1);

    while (1) {
        GPIO_PORTF_DATA_R |= (1 << 1);   /* LED on */
        delay(2000000);
        GPIO_PORTF_DATA_R &= ~(1 << 1);  /* LED off */
        delay(2000000);
    }
    return 0;
}
