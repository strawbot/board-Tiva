// stdlib.h — inline stdlib shadow for TIVA hard-float build
//
// Shadows the system <stdlib.h> so no nofp libc object is pulled in.
// malloc/free/calloc/realloc are declared extern — strong symbols live in
// src/malloc_stub.c.  qsort is provided here as a static implementation.

#ifndef STDLIB_H_
#define STDLIB_H_

#include <stddef.h>   // size_t — from the compiler

// ── heap (strong symbols in src/malloc_stub.c) ────────────────────────────────
void *malloc (size_t size);
void  free   (void  *ptr);
void *calloc (size_t nmemb, size_t size);
void *realloc(void  *ptr, size_t size);

// ── integer arithmetic ────────────────────────────────────────────────────────
static inline int  abs (int  n)  { return n < 0 ? -n : n; }
static inline long labs(long n)  { return n < 0 ? -n : n; }

// ── string → number ───────────────────────────────────────────────────────────
static inline unsigned long strtoul(const char *s, char **endptr, int base) {
    while (*s == ' ' || *s == '\t') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1]=='x'||s[1]=='X')) { s += 2; }
    unsigned long n = 0;
    const char *start = s;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        n = n * (unsigned long)base + (unsigned long)d;
        s++;
    }
    (void)start;
    if (endptr) *endptr = (char *)s;
    return n;
}

static inline long strtol(const char *s, char **endptr, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    unsigned long u = strtoul(s, endptr, base);
    return neg ? -(long)u : (long)u;
}

static inline int atoi(const char *s) {
    int n = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}
static inline long atol(const char *s) { return (long)atoi(s); }

// ── qsort — insertion sort (stable, no recursion, no stack blowup) ────────────
// Acceptable for the small arrays (≤ NUM_ACTIONS = 40) used by tea.c.
static inline void qsort(void *base, size_t nmemb, size_t size,
                          int (*cmp)(const void *, const void *)) {
    unsigned char *b = (unsigned char *)base;
    unsigned char  tmp[64];   // max element size we expect (Short = 2 bytes)
    size_t s = size < sizeof(tmp) ? size : sizeof(tmp);

    for (size_t i = 1; i < nmemb; i++) {
        // copy element i into tmp
        unsigned char *pi = b + i * size;
        unsigned char *t  = tmp;
        for (size_t k = 0; k < s; k++) t[k] = pi[k];

        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * size, tmp) > 0) {
            // shift element j-1 up to j
            unsigned char *src = b + (j - 1) * size;
            unsigned char *dst = b +  j      * size;
            for (size_t k = 0; k < s; k++) dst[k] = src[k];
            j--;
        }
        // write tmp into position j
        unsigned char *dst = b + j * size;
        for (size_t k = 0; k < s; k++) dst[k] = tmp[k];
    }
}

// ── program termination (stub — no OS to return to) ───────────────────────────
static inline void abort(void) { while (1) {} }
static inline void exit (int  code) { (void)code; while (1) {} }

#endif /* STDLIB_H_ */
