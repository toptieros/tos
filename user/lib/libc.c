/* tOS userspace libc core: the freestanding C runtime shared by every app (C
 * and C++). It provides the mem/str helpers the compiler emits calls to, a real
 * heap over SYS_MMAP (malloc/free/realloc/calloc), and a small printf family.
 * The frame pool is all of RAM, so the heap scales with the machine -- it is not
 * bounded by the program code/stack window. */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "ulib.h"
#include "libc.h"

/* --- memory / string ------------------------------------------------------- */
void *memset(void *d, int c, size_t n) {
    unsigned char *p = (unsigned char *)d;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) a[i] = b[i];
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    if (a < b) for (size_t i = 0; i < n; i++) a[i] = b[i];
    else       for (size_t i = n; i > 0; i--) a[i - 1] = b[i - 1];
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = (const unsigned char *)x, *b = (const unsigned char *)y;
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i]; if (!a[i]) break; }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d;
}

/* --- heap -----------------------------------------------------------------
 * An explicit free list sorted by address, with first-fit allocation, block
 * splitting, and boundary coalescing. Every block carries a header with its
 * total size; free blocks also carry a next pointer. The arena grows by mmap'ing
 * >= 1 MiB chunks on demand. Coalescing only merges address-adjacent blocks, so
 * it is correct whether or not successive mmap chunks happen to abut. */
typedef struct blk {
    size_t       size;          /* total bytes of this block, header included */
    struct blk  *next;          /* next free block (address-sorted); junk when in use */
} blk;

#define HDR     (sizeof(blk))
#define ALIGN   16
#define MINBLK  (HDR + ALIGN)

static blk *freelist;           /* address-sorted singly-linked list of free blocks */

static size_t roundup(size_t n) { return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1); }

/* Insert a free block into the address-sorted list, coalescing with the
 * immediately-adjacent neighbours on either side. */
static void insert_free(blk *b) {
    blk **pp = &freelist;
    while (*pp && *pp < b) pp = &(*pp)->next;
    blk *next = *pp;
    /* merge forward */
    if (next && (char *)b + b->size == (char *)next) { b->size += next->size; b->next = next->next; }
    else b->next = next;
    *pp = b;
    /* merge with previous (walk again to find the predecessor) */
    if (pp != &freelist) {
        blk *prev = freelist;
        while (prev->next != b) prev = prev->next;
        if ((char *)prev + prev->size == (char *)b) { prev->size += b->size; prev->next = b->next; }
    }
}

static int grow(size_t need) {
    size_t chunk = need > (1u << 20) ? roundup(need) : (1u << 20);
    blk *b = (blk *)mmap_(chunk);
    if (!b) return 0;
    b->size = chunk;
    insert_free(b);
    return 1;
}

void *malloc(size_t n) {
    if (!n) n = 1;
    size_t need = roundup(n + HDR);
    if (need < MINBLK) need = MINBLK;
    for (int tries = 0; tries < 2; tries++) {
        blk **pp = &freelist;
        for (blk *b = freelist; b; pp = &b->next, b = b->next) {
            if (b->size < need) continue;
            if (b->size - need >= MINBLK) {           /* split: keep the tail free */
                blk *tail = (blk *)((char *)b + need);
                tail->size = b->size - need;
                tail->next = b->next;
                b->size = need;
                *pp = tail;
            } else {                                  /* take the whole block */
                *pp = b->next;
            }
            return (char *)b + HDR;
        }
        if (!grow(need)) return 0;                    /* out of memory */
    }
    return 0;
}

void free(void *p) {
    if (!p) return;
    blk *b = (blk *)((char *)p - HDR);
    insert_free(b);
}

void *calloc(size_t a, size_t b) {
    size_t n = a * b;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    blk *b = (blk *)((char *)p - HDR);
    size_t old = b->size - HDR;
    if (n <= old) return p;                           /* shrinking: keep the block */
    void *np = malloc(n);
    if (np) { memcpy(np, p, old); free(p); }
    return np;
}

/* --- printf family --------------------------------------------------------- */
static void put(char *buf, size_t size, size_t *pos, char c) {
    if (*pos + 1 < size) buf[*pos] = c;               /* leave room for the NUL */
    (*pos)++;
}
static void putnum(char *buf, size_t size, size_t *pos, uint64_t v, int base,
                   int isneg, int width, char pad) {
    char tmp[24]; int n = 0;
    const char *digits = "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; }
    if (isneg) tmp[n++] = '-';
    for (int i = n; i < width; i++) put(buf, size, pos, pad);   /* pad to width */
    while (n) put(buf, size, pos, tmp[--n]);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { put(buf, size, &pos, *f); continue; }
        f++;
        char pad = ' '; int width = 0;
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        int lng = 0;
        while (*f == 'l') { lng++; f++; }
        switch (*f) {
        case 'd': case 'i': {
            long long v = lng ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            int neg = v < 0; unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
            putnum(buf, size, &pos, u, 10, neg, width, pad); break;
        }
        case 'u': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned);
            putnum(buf, size, &pos, v, 10, 0, width, pad); break;
        }
        case 'x': case 'X': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned);
            putnum(buf, size, &pos, v, 16, 0, width, pad); break;
        }
        case 'p': {
            put(buf, size, &pos, '0'); put(buf, size, &pos, 'x');
            putnum(buf, size, &pos, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0, 0, ' '); break;
        }
        case 'c': put(buf, size, &pos, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) put(buf, size, &pos, *s++);
            break;
        }
        case '%': put(buf, size, &pos, '%'); break;
        default:  put(buf, size, &pos, '%'); if (*f) put(buf, size, &pos, *f); break;
        }
    }
    if (size) buf[pos < size ? pos : size - 1] = 0;
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    print(buf);                                       /* SYS_WRITE: NUL-terminated */
    return n;
}
