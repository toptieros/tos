/* Human-readable byte sizes for the Files app (status-bar free space §6, Get Info §8,
 * a future Size column §1): 1024-based, one decimal past bytes -- 900 -> "900 B",
 * 1536 -> "1.5 KB", 1887436 -> "1.8 MB" (rounded). Pure + standalone (builds the string by hand,
 * no stdio/float) so it unit-tests on the host and compiles into the freestanding app. */
#pragma once
#include <stdint.h>

/* internal: append decimal v (>=0) to out[p..cap), return new position */
static inline int hb_putu_(char *out, int cap, int p, uint32_t v) {
    char t[12]; int n = 0;
    if (v == 0) { if (p < cap - 1) out[p++] = '0'; return p; }
    while (v && n < 12) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < cap - 1) out[p++] = t[--n];
    return p;
}
static inline int hb_puts_(char *out, int cap, int p, const char *s) {
    while (*s && p < cap - 1) out[p++] = *s++;
    return p;
}

static inline void human_bytes(uint32_t bytes, char *out, int cap) {
    if (cap <= 0) return;
    int p = 0;
    if (bytes < 1024u) {                              /* plain bytes, no decimal */
        p = hb_putu_(out, cap, p, bytes);
        p = hb_puts_(out, cap, p, " B");
        out[p < cap ? p : cap - 1] = 0;
        return;
    }
    uint32_t div; const char *unit;
    if (bytes < 1024u * 1024u)            { div = 1024u;               unit = " KB"; }
    else if (bytes < 1024u * 1024u * 1024u) { div = 1024u * 1024u;       unit = " MB"; }
    else                                   { div = 1024u * 1024u * 1024u; unit = " GB"; }
    uint32_t tenths = (uint32_t)(((uint64_t)bytes * 10u + div / 2u) / div);  /* rounded */
    uint32_t whole = tenths / 10u, frac = tenths % 10u;
    p = hb_putu_(out, cap, p, whole);
    p = hb_puts_(out, cap, p, ".");
    p = hb_putu_(out, cap, p, frac);
    p = hb_puts_(out, cap, p, unit);
    out[p < cap ? p : cap - 1] = 0;
}
