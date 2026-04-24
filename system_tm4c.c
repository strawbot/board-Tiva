#include <stdint.h>

/* Minimal SystemInit for TM4C123: enable FPU (if present) and set up clock
   placeholder. For many simple apps the default clock is fine. */

void SystemInit(void) {
    /* Enable full access to CP10 and CP11 (FPU) */
    volatile uint32_t *CPAC = (uint32_t *)0xE000ED88;
    *CPAC |= (0xF << 20);

    /* Small delay to let FPU enable take effect */
    for (volatile int i = 0; i < 1000; i++) { __asm__("nop"); }

    /* Optionally configure system clock here. Keep default. */
}
