/* Pure path helper for the Files app's "Duplicate" action (files-app §12): given an
 * item's path, build the k-th Finder-style copy name -- "<stem> copy<ext>" for k==1,
 * then "<stem> copy 2", "copy 3", ... -- so the caller can loop k against sys_exists
 * to find a free name. The extension is whatever follows the LAST dot in the basename
 * ("a.tar.gz" -> "a.tar copy.gz"); a name with no dot keeps no extension.
 *
 * No FS, no libc -- builds the string by hand -- so this header unit-tests on the host
 * (tests/unit/t_dupname.c) and also compiles into the freestanding app. */
#pragma once

/* internal: append s to out[pos..cap), return the new position (NUL added by caller) */
static inline int dup_put_(char *out, int cap, int pos, const char *s) {
    while (*s && pos < cap - 1) out[pos++] = *s++;
    return pos;
}
/* internal: append decimal n (n>=1) */
static inline int dup_putint_(char *out, int cap, int pos, int n) {
    char tmp[12]; int t = 0;
    if (n <= 0) { if (pos < cap - 1) out[pos++] = '0'; return pos; }
    while (n > 0 && t < 12) { tmp[t++] = (char)('0' + n % 10); n /= 10; }
    while (t > 0 && pos < cap - 1) out[pos++] = tmp[--t];
    return pos;
}

/* see the file header comment. out is always NUL-terminated within cap. */
static inline void dup_candidate(char *out, int cap, const char *full, int k) {
    int n = 0; while (full[n]) n++;
    int slash = -1, dot = -1;
    for (int i = 0; i < n; i++) if (full[i] == '/') slash = i;
    for (int i = slash + 1; i < n; i++) if (full[i] == '.') dot = i;
    int stem_end = (dot > slash) ? dot : n;                 /* basename stem ends at the ext-dot */
    int pos = 0;
    for (int i = 0; i <= slash && pos < cap - 1; i++) out[pos++] = full[i];          /* dir + '/' */
    for (int i = slash + 1; i < stem_end && pos < cap - 1; i++) out[pos++] = full[i]; /* stem     */
    pos = dup_put_(out, cap, pos, " copy");
    if (k >= 2) { pos = dup_put_(out, cap, pos, " "); pos = dup_putint_(out, cap, pos, k); }
    for (int i = stem_end; i < n && pos < cap - 1; i++) out[pos++] = full[i];         /* ".ext"   */
    if (pos > cap - 1) pos = cap - 1;
    out[pos] = 0;
}
