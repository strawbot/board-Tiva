// arch/sys_arch.h — lwIP sys layer stubs for NO_SYS=1 / TimbreOS
//
// With NO_SYS=1 lwIP does not use semaphores, mutexes, or mailboxes.
// This header satisfies the types lwip/sys.h expects even when those
// abstractions are compiled out.

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

#include <stdint.h>

// Timeout sentinel: used by lwIP to mean "wait forever".
#define SYS_ARCH_TIMEOUT  0xffffffffUL

// Stub types — never instantiated at runtime with NO_SYS=1.
typedef uint32_t sys_sem_t;
typedef uint32_t sys_mutex_t;
typedef uint32_t sys_mbox_t;
typedef uint32_t sys_thread_t;

// sys_now() is the only function actually called at runtime.
// Implemented in sys_arch.c using get_ticks() (1 ms resolution).
uint32_t sys_now(void);

#endif // LWIP_ARCH_SYS_ARCH_H
