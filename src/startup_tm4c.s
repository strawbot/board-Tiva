.syntax unified
.thumb
.cpu cortex-m4
.fpu fpv4-sp-d16

/* Vector table */
    .section .isr_vector,"a",%progbits
    .word _estack
    .word ResetISR
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler

    /* IRQ0–IRQ18 */
    .rept 19
      .word IntDefaultHandler
    .endr
    /* IRQ19: Timer0A */
    .word Timer0AIntHandler
    /* IRQ20–IRQ239 */
    .rept 220
      .word IntDefaultHandler
    .endr

/* Reset handler */
    .section .text.ResetISR,"ax",%progbits
    .global ResetISR
    .thumb_func
ResetISR:
    /* Enable FPU - must happen before ANY other code when using hard float ABI */
    ldr    r0, =0xE000ED88    /* CPACR */
    ldr    r1, [r0]         
    orr    r1, r1, #(0xF << 20) /* CP10 + CP11 full access */
    str    r1, [r0]
    dsb                     
    isb                     

    /* Copy data section from flash to RAM */
    ldr r0, =__data_load_start
    ldr r1, =__data_start__
    ldr r2, =__data_end__
1:
    cmp r1, r2
    bge 2f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 1b
2:
    /* Zero initialize .bss */
    ldr r0, =__bss_start__
    ldr r1, =__bss_end__
3:
    cmp r0, r1
    bge 4f
    movs r2, #0
    str r2, [r0], #4
    b 3b
4:
    /* Call SystemInit if present */
    bl SystemInit
    /* Call main */
    bl main
hang:
    b hang

/* Default handlers */
    .section .text.DefaultHandlers,"ax",%progbits
    .global IntDefaultHandler
    .thumb_func
IntDefaultHandler:
    b .

    .weak NMI_Handler
    .set NMI_Handler, IntDefaultHandler
    .weak HardFault_Handler
    .set HardFault_Handler, IntDefaultHandler
    .weak MemManage_Handler
    .set MemManage_Handler, IntDefaultHandler
    .weak BusFault_Handler
    .set BusFault_Handler, IntDefaultHandler
    .weak UsageFault_Handler
    .set UsageFault_Handler, IntDefaultHandler
    .weak SVC_Handler
    .set SVC_Handler, IntDefaultHandler
    .weak DebugMon_Handler
    .set DebugMon_Handler, IntDefaultHandler
    .weak PendSV_Handler
    .set PendSV_Handler, IntDefaultHandler
    .weak SysTick_Handler
    .set SysTick_Handler, IntDefaultHandler
    .weak Timer0AIntHandler
    .set Timer0AIntHandler, IntDefaultHandler

    .extern __data_load_start
    .extern __data_start__
    .extern __data_end__
    .extern __bss_start__
    .extern __bss_end__
    .extern main
    .extern SystemInit

