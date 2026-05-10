// arch/cc.h — lwIP compiler/platform configuration for TM4C (ARM Cortex-M4)
//
// Included by lwip/arch.h via #include "arch/cc.h".
// Keep this file self-contained: no TIVA-specific peripheral headers.

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>   // abort()

// ── Byte order ────────────────────────────────────────────────────────────────
// Cortex-M4 is little-endian.
#define BYTE_ORDER  LITTLE_ENDIAN

// ── Integer types ─────────────────────────────────────────────────────────────
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;

// ── Printf format specifiers ──────────────────────────────────────────────────
// lwIP uses these when LWIP_DEBUG is enabled.  All output is silenced by
// LWIP_PLATFORM_DIAG in lwipopts.h so the exact format doesn't matter, but
// the macros must exist.
#define U16_F  "u"
#define S16_F  "d"
#define X16_F  "x"
#define U32_F  "lu"
#define S32_F  "ld"
#define X32_F  "lx"
#define SZT_F  "u"

// ── Struct packing ────────────────────────────────────────────────────────────
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT  __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x)  x

// ── Assertions ────────────────────────────────────────────────────────────────
// Route lwIP assertions to the TimbreOS system_failure path (defined in board.c).
// Declared extern to avoid pulling in any board header here.
extern void lwip_assert_handler(const char *msg);
#define LWIP_PLATFORM_ASSERT(x)  do { lwip_assert_handler(x); } while(0)

#endif // LWIP_ARCH_CC_H
