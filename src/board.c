// board.c — TIVA board-level hooks required by TimbreOS
//
// system_failure(): called by tea.c BLACK_HOLE() when a scheduler invariant
//   is violated (out of action slots, out of time-event slots, etc.).
//   Disables interrupts and spins — gives the debugger something to catch.
//
// visible_word(): called by help.c printif() to filter CLI help output.
//   Returns true to show all words; override to hide board-private words.

#include <stdbool.h>
#include "tea.h"      // Long, system_failure declaration

// ── system_failure ────────────────────────────────────────────────────────────

void system_failure(Long reason) {
    (void)reason;
    __asm volatile ("cpsid i" ::: "memory");   // disable all interrupts
    while (1) {}                               // trap for debugger
}

// ── visible_word ──────────────────────────────────────────────────────────────

bool visible_word(char *s) {
    (void)s;
    return true;   // show all words in help output
}
