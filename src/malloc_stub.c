// malloc_stub.c — static heap for TIVA hard-float build
//
// Provides strong-symbol malloc/free/calloc/realloc so the linker
// never reaches libc's nofp malloc objects (which cause VFP ABI
// mismatch errors with -mfloat-abi=hard).
//
// Strategy: first-fit free-list allocator over a 2 KB static pool.
// Block header is 8 bytes; minimum allocation is 8 bytes.
// Thread safety: interrupts are disabled around list traversal.
// (The TEA scheduler is cooperative so contention is rare; the
//  guard is there for ISR paths that might call malloc in future.)
//
// ── Pool sizing ───────────────────────────────────────────────────────────────
// The pool serves the TimbreOS dictionary (runtime word definitions) and any
// transient allocations during interpret/compile.  gpio_dump.c no longer uses
// snprintf, so newlib's _svfprintf_r / _malloc_r no longer touch this pool.
//
// To find the actual high-water mark at runtime, inspect _head after a
// representative session: walk the free-list and sum free bytes; subtract from
// POOL_BYTES.  Increase if cli.c reports "out of memory" on word definition.
//
// With the compile-time wordlist (wordlist.c) pre-populating most words, only
// interactively-defined words hit this pool.  2 KB handles ~50 small words.

#include <stdint.h>
#include <stddef.h>
#include "string.h"   // our inline shadow — no nofp libc object pulled in

// ── pool ─────────────────────────────────────────────────────────────────────

#define POOL_BYTES  2048u   // 2 KB — raise to 3072 if dictionary overflows

typedef struct Block {
    uint32_t      size;   // usable bytes (excludes header)
    uint32_t      free;   // 1 = free, 0 = allocated
    struct Block *next;   // next block in pool (NULL = last)
} Block;

static uint8_t  _pool[POOL_BYTES] __attribute__((aligned(8)));
static Block   *_head = NULL;

// ── init (called lazily on first malloc) ─────────────────────────────────────

static void pool_init(void) {
    _head = (Block *)_pool;
    _head->size = POOL_BYTES - sizeof(Block);
    _head->free = 1;
    _head->next = NULL;
}

// ── interrupt guard ───────────────────────────────────────────────────────────

static inline uint32_t primask_save(void) {
    uint32_t pm;
    __asm volatile ("mrs %0, primask\n\t cpsid i" : "=r"(pm) :: "memory");
    return pm;
}
static inline void primask_restore(uint32_t pm) {
    __asm volatile ("msr primask, %0" :: "r"(pm) : "memory");
}

// ── malloc ────────────────────────────────────────────────────────────────────

void *malloc(size_t size) {
    if (size == 0) return NULL;

    // align up to 8 bytes
    size = (size + 7u) & ~7u;

    uint32_t pm = primask_save();

    if (_head == NULL) pool_init();

    Block *b = _head;
    while (b) {
        if (b->free && b->size >= size) {
            // split if remainder is large enough for a new block
            if (b->size >= size + sizeof(Block) + 8u) {
                Block *nb = (Block *)((uint8_t *)b + sizeof(Block) + size);
                nb->size = b->size - size - sizeof(Block);
                nb->free = 1;
                nb->next = b->next;
                b->size  = size;
                b->next  = nb;
            }
            b->free = 0;
            primask_restore(pm);
            return (uint8_t *)b + sizeof(Block);
        }
        b = b->next;
    }

    primask_restore(pm);
    return NULL;   // out of memory
}

// ── free ──────────────────────────────────────────────────────────────────────

void free(void *ptr) {
    if (!ptr) return;

    uint32_t pm = primask_save();

    Block *b = (Block *)((uint8_t *)ptr - sizeof(Block));
    b->free = 1;

    // coalesce with next free neighbours
    while (b->next && b->next->free) {
        b->size += sizeof(Block) + b->next->size;
        b->next  = b->next->next;
    }

    primask_restore(pm);
}

// ── calloc ────────────────────────────────────────────────────────────────────

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void  *p     = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

// ── _sbrk ─────────────────────────────────────────────────────────────────────
// Newlib's _sbrk_r wrapper requires this symbol even when malloc is replaced.
// Return -1 to indicate that dynamic heap growth is not available; all
// allocation goes through the static pool above.

#include <stdint.h>
void *_sbrk(intptr_t increment) {
    (void)increment;
    return (void *)-1;
}

// ── realloc ───────────────────────────────────────────────────────────────────

void *realloc(void *ptr, size_t size) {
    if (!ptr)   return malloc(size);
    if (!size)  { free(ptr); return NULL; }

    Block *b = (Block *)((uint8_t *)ptr - sizeof(Block));
    if (b->size >= size) return ptr;   // fits in place

    void *np = malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, b->size < size ? b->size : size);
    free(ptr);
    return np;
}
