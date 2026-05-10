// sys_arch.c — lwIP sys layer for NO_SYS=1 / TimbreOS on TM4C
//
// Only sys_now() is needed at runtime with NO_SYS=1.  It must return
// a monotonically increasing millisecond count; get_ticks() delivers
// exactly that (Timer0A increments the counter every 1 ms).

#include "lwip/sys.h"
#include "clocks.h"    // get_ticks()

// Called by lwIP timeouts.c to drive all internal protocol timers.
// Resolution: 1 ms (ONE_SECOND = 1000 in project_defs.h).
uint32_t sys_now(void)
{
    return (uint32_t)get_ticks();
}

// lwip_assert_handler — called by LWIP_PLATFORM_ASSERT (arch/cc.h).
// Routes into the TimbreOS system_failure path so an lwIP assertion
// halts with the same behaviour as any other fatal error.
void lwip_assert_handler(const char *msg)
{
    (void)msg;
    // system_failure() disables interrupts and spins — debugger can inspect
    // the call stack and 'msg' via the caller's frame.
    extern void system_failure(long reason);
    system_failure(0xBAD1DEAD);
}
